# Concurrent VM mutation: TLB shootdown + serialised frame refcounts

Local acceptance, 2026-11-21, macOS arm64 host, homebrew qemu-system-x86_64,
kernel built by the staged self-hosted fakecc toolchain. LOCAL result only —
the Linux-CI-only reference-oracle gate and the host-executed fragment tests
remain CI-scoped (see docs/evidence/native-wait-cancel-20261118/RESULT.md).
No commit/push claimed.

## What was implemented

The two SMP correctness gaps called out in
docs/native-kernel-architecture.md ("concurrent VM mutation / shared
physical-frame accounting / TLB shootdowns remain unsolved") are closed:

* **TLB shootdown (vector-35 IPI, Linux smp_call_function shape)**: three
  mutation sites — `unmap_present_pages` (munmap/exit), `apply_prot_present`
  (mprotect) and the COW fork's parent RO demotion (`cow_copy_pt`) — now
  publish a shootdown for the mutated range BEFORE releasing any frames.
  mem/as.c stays IPI-agnostic: it maintains the per-CPU installed-AS mirror
  (`as_current_h`, written by `as_activate`) and calls an injected kernel
  hook; the kernel (kernel/sched.c) pushes range entries into PER-CPU
  mailboxes (`as_sd_*`, 16-deep, per-queue spinlock), sends vector-35, and
  waits for per-CPU monotonic done markers with IRQs enabled and no locks
  held. The vector-35 handler (kernel/isr.c) drains its own mailbox at IRQ
  speed: invlpg per page for small ranges, full CR3 reload above 64 pages,
  only when the target CPU still runs the targeted address space.
  NO global lock: two CPUs fork concurrently and each wait for their own
  acknowledgements without deadlock (a sender never holds a lock while
  waiting; a handler only takes its own queue lock). Waiting happens with
  IRQs enabled and a bounded timeout dump — fail-visible, never a silent
  machine freeze.
* **Frame refcounts serialised**: the COW/fault/unmap/fork refcount paths
  now take a leaf spinlock (`ref_lock`, order ref_lock -> pmm_lock; never
  held across the shootdown hook or any wait). The decide-and-mutate step
  of the COW fault (shared-check, private-copy, demote) is atomic as a
  whole; fork's ref bumps and unmap/destroy's decrements cannot lose
  updates. `frame_refs` widened u16 -> u32.
* New memory-mapping primitive: `VMA_PHYS` + `as_map_phys_h` (device/
  driver frames at a fixed physical base; demand-filled, pinned via
  frame_refs=2 so unmap never frees driver-owned buffers) — the facility
  the RAW zero-copy net rings already rely on.

## fakecc constraints discovered (documented, worked around)

* **The .S assembler SILENTLY DROPS `lock cmpxchg`**: the instruction
  vanished (only mov+ret survived), which first broke the refcount helpers
  invisibly. A naked-C-bytes variant kept correct machine code but fakecc
  ignores `__attribute__((naked))` and still spills arguments through the
  caller's %rbp, corrupting the caller's locals. Resolution: no CAS
  primitive at all — a leaf ref_lock spinlock serialises the counters.
* 2-dimensional struct arrays and volatile-qualified struct members fail
  to parse ("incomplete element type") — the shootdown mailboxes use
  parallel flat arrays (the RAWO precedent).
* `as_dispatch_fault` resolves against the INSTALLED address space
  (current CR3), not its handle argument — kernel-side fault callers must
  activate the target AS first.
* Kernel worker threads CANNOT hold a user address space across
  preemption: the scheduler reloads CR3 from the task's as_h on resume.
  Concurrent-VM tests therefore drive the fault dispatcher explicitly
  (`as_dispatch_fault`) instead of dereferencing user VAs.

## Deterministic proof (kernel/memtest.c vm_concurrent_selftest)

4 kernel workers storm the VM layer for 60 rounds total; on SMP2/4 two
workers additionally share ONE address space across two CPUs (munmap/
remap/mprotect churn on disjoint slices, both CPUs installing it). Every
round forks (COW RO demotion + refcount), switches both ASes, munmaps and
re-maps a slice. PASS on SMP1, SMP2 and SMP4:

* `private COW storms: 18 forks, no stale-TLB corruption` — every fork
  completed, no stage failures.
* `cross-CPU shared space: munmap/mprotect storms, 18 shootdowns` (SMP2/4)
  — the shootdown counter advanced by exactly the phase's mutation count
  with both CPUs installing the shared space.
* No page-fault crashes, no double-frees, no hangs; the pre-existing
  boot-level tests (VMA/COW selftest, process lifetime, [U] mprotect
  RO/RW) all pass unchanged in the same boots.

## Known issue (tracked, surfaced by this slice)

* ~1 page leaks per fork+release cycle (measured: fork window allocates
  exactly 4 table frames — PML4/PDPT/PD/PT — and the teardown walk sees
  some destroyed ASes with EMPTY PML4s while others walk their hierarchy;
  the accounting drift is exactly 1 page per fork across all worker
  modes). It is a teardown/refcount-gap investigation of its own; the
  stress test reports it as [WARN] rather than hiding it. NOT a shootdown
  failure and NOT a stale-TLB corruption.

## Remaining limitations (deliberate)

* One outstanding shootdown shape: senders serialise per-target queue
  push; no batching of multiple ranges into one IPI (bounded by the
  64-page chunking in unmap).
* `ref_lock` is one global leaf lock — correct and bounded, not
  per-frame-optimised; contention shows only under COW-storm workloads.
* Shootdown targets are "CPUs whose installed AS matches" — a CPU that
  loaded the AS and switched away before the IPI needs no flush (CR3
  reload clears), which the ack-anyway protocol covers.
