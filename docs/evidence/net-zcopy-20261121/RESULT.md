# Zero-copy raw device rings (AF_XDP analogue) + async TX

Local acceptance, 2026-11-21, macOS arm64 host, homebrew qemu-system-x86_64
with QEMU user-net (10.0.2.0/24, guest lease 10.0.2.15), kernel built by the
staged self-hosted fakecc toolchain. LOCAL result only — the host-side
fragment tests (tools/test_net_wire_io.py, test_net_irq_work.py,
test_net_tcp_io.py) execute fakecc natively and remain Linux-CI-only on this
host (same deferral reasoning as docs/evidence/native-wait-cancel-20261118/
RESULT.md). No commit/push claimed.

## What was implemented

The goal: a modern, natively concurrent network module with TWO interfaces —
the easy upper layer and a direct zero-copy bottom layer — per the Linux
lessons (skb copies cost; synchronous TX serialises; descriptor rings with
in-place buffers are the fast path; AF_XDP/umem is the modern raw shape).

### Layer 1 — easy upper interface (unchanged contract)

The socket ABI (socket/bind/connect/sendto/recvfrom/listen/accept/shutdown
+ poll) keeps per-socket kmutex + wait-queue blocking semantics, and the
UDP/TCP/ARP/DHCP/ICMP stack now reads received frames IN PLACE from driver
buffers instead of copying each frame into a staging buffer first
(kernel/net.c worker switched from virtio_net_rx-copy to
virtio_net_rx_take). Socket payload copies into per-socket queues remain
(POSIX semantics).

### Layer 2 — zero-copy raw rings (new syscalls 509-512)

* `raw_create()` -> ring handle (RAW_MAX=2 concurrent rings, owner-pid
  bound). Each ring allocates a contiguous 64 KiB frame pool (32 x 2048 B,
  buddy order 4) and a shared control page.
* `raw_map(handle, uva)` maps the control page and the WHOLE frame pool
  read-write into the owner's address space — packet bytes live once in
  DMA-visible memory. Needs the new `VMA_PHYS` mapping kind (mem/as.c):
  faults resolve directly to the fixed physical base, frames are pinned
  (frame_refs=2) so munmap never frees driver-owned buffers to the buddy.
* `raw_tx_kick(handle, max)`: recycles completed wire submissions
  (in-order), then processes pending TX entries: frames addressed to the
  host stack LOOP BACK through the RX ring in the same physical buffer
  (deterministic zero-copy turnaround, protocol path also fed), everything
  else chains into the NIC asynchronously — `virtio_net_tx_submit` writes a
  two-descriptor chain (pre-allocated virtio-net header + the caller's
  buffer) and notifies; NO staging copy, NO synchronous wait.
* `raw_close(handle)`: returns parked RX slots to the driver, drains
  in-flight TX, frees ring memory.
* Slot discipline: one shared frame pool with single-word atomic-exchange
  slot flags — the pool is shared lock-free between the kernel (RX
  producer, TX consumer) and user (RX consumer, TX producer), SPSC per
  direction. Backpressure: a slow consumer makes the worker drop frames
  (counted) rather than stall the stack.

Driver additions (drivers/virtio_net.c): `virtio_net_rx_take` (zero-copy RX
handoff, slot not re-posted), `virtio_net_rx_post_slot` (refill),
`virtio_net_tx_submit`/`virtio_net_tx_poll_completed` (async TX with a
32-deep per-submission virtio-net header pool carved from the existing
arena; completions in submission order), `virtio_net_tx_async_inflight`.
The legacy synchronous `virtio_net_tx` is untouched for protocol frames.

Two fakecc constraints shaped the code (both documented in
docs/evidence/native-caps-20261121/RESULT.md): 5-argument calls miscompile
(`as_map_phys_h` deliberately takes 4 arguments with a fixed RW protection),
and no preprocessor directives.

## Deterministic proof

scripts/test_net_raw.py (SMP2, QEMU user-net, guest /bin/init --net-raw):

1. ring create/map: control page (magic/count/frame size/local IP/MAC) and
   the 64 KiB frame pool map into the probe's address space.
2. the probe builds a complete Ethernet+IPv4+UDP frame IN a TX pool slot
   (IP checksum computed in user code), publishes it on the TX ring, kicks.
3. because the frame's destination IP is the host's own, the kick loops it
   back through the RX ring — **the returned RX slot equals the TX slot**
   (zero-copy turnaround, no buffer moved or copied) and all 11 payload
   bytes read back intact.
4. the SAME datagram was delivered by the protocol stack to a UDP socket
   bound to the probe port (dual-path evidence: raw zero-copy and easy
   socket interfaces coexist over one NIC).
5. consumed slots return through atomic-exchange flags; ring close and
   mmap/munmap of the mapped range complete cleanly.
6. the boot also asserted the wait/cancel 18-case suite stays failures=0
   and no [FAIL] marker anywhere.

`__RAW_RC=0`, evidence retained in build/net-raw/smp2.serial.log.

## Bugs found and fixed during this slice

* RAWO_* control-page offsets were written as byte offsets but used as u32
  indexes — every field was misaligned; both kernel and user now share the
  same u32-index layout, and the probe asserts magic/count/frame-size
  before use.
* sys_raw_create returned a loop variable clobbered by later loops
  (handle 7 instead of 1) — explicit index variable now.
* User test initially built the IPv4 header with wrong src/dst offsets
  (clobbering the checksum field) — corrected to checksum at +24, src at
  +26, dst at +30.
* as_map_phys_h originally took 5 arguments — fakecc 5-argument
  miscompilation (documented) made the protection parameter arrive
  corrupted; reduced to 4 arguments with fixed RW protection.

## Remaining limitations (deliberate)

* RAW rings receive every driver frame that the worker takes; per-ring
  filtering (BPF-like) and fan-out to multiple rings are later slices.
* Wire TX completes only on the next kick (no TX-completion IRQ wake yet);
  the 32-deep in-flight cap bounds outstanding submissions.
* One RX worker thread (protocol + delivery) as before; per-CPU RX
  sharding is a later slice.
* Ring memory is bounded (2 rings x 64 KiB frames + control pages);
  no flow-control accounting yet beyond slot backpressure.
