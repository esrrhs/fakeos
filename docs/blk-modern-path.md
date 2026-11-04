# Block / virtio modern path (notes)

fakeos today is a **workstation-shaped** hobby OS, not a Linux replacement.
Storage and net bring-up intentionally stay on the simplest QEMU devices that
the in-tree drivers already speak.

## What ships today

| Path | QEMU hint | Driver | Notes |
|------|-----------|--------|-------|
| VirtIO-blk (legacy) | `-device virtio-blk-pci,disable-modern=on,...` | `drivers/virtio_blk.c` | PIO legacy virtio-pci; multi-sector R/W; FLUSH best-effort |
| **VirtIO-blk (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_blk.c` | MMIO common/notify/ISR/device caps; MSI-X/MSI vector 41; IRQ wake + poll fallback |
| **AHCI (SATA)** | `make qemu-ahci` / `-device ahci` + `ide-hd` | `drivers/ahci.c` | Single port, multi-slot CL; DMA R/W/FLUSH; async NCQ + bio queue (depth 4 HW / 8 bios) when CAP.SNCQ; sync blk API + `blk_async_rw` end_io; MSI/MSI-X or INTx vector 42 + `sched_sleep`; ABAR relocated into boot HHDM when QEMU maps it high |
| ATA PIO | IDE disk | `drivers/ata.c` | Identify + LBA28 PIO; fallback when virtio/AHCI absent |
| VirtIO-net (legacy) | `virtio-net-pci,disable-modern=on` | `drivers/virtio_net.c` | MSI/MSI-X or INTx; DHCP/TCP/UDP |
| **VirtIO-net (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_net.c` | Same cap map as blk; multi-vector MSI-X RX=40/TX=43 (per-queue); single MSI/MSI-X or INTx fallback; TX `sched_sleep` when multi |

`drivers/blk.c` preference: **virtio-blk → AHCI → ATA**. Ext2 rides on that
block layer (journal-lite descriptor/revoke ring + 8-slot meta-copy + revoke-aware replay + file data-path blk_async bounce via blk_async batch, multi-bg, triple-indirect).

## Modern virtio status

- **Legacy virtio-pci** remains the default for `make qemu` / `qemu-disk` /
  `make test` (`disable-modern=on`) so bring-up stays small and deterministic.
- **Modern virtio 1.x** for **blk and net**: PCI vendor-cap walk maps
  common/notify/ISR/device cfg; feature select 0/1; queue_desc/driver/device;
  notify_off × multiplier. `vmm_map_mmio` brings PCI-hole BARs into HHDM.
- **MSI / MSI-X**: `pci_msi_enable` / `pci_msix_enable_one` / `pci_msix_enable_n`.
  Modern virtio-net prefers multi-vector MSI-X (RX vector 40 entry 0, TX vector
  43 entry 1) via `pci_msix_enable_n`; falls back to single MSI then MSI-X then
  INTx. Blk vector 41; AHCI 42. Blk/AHCI/TX `sched_sleep` on used-ring IRQ when
  the scheduler is live (spin fallback otherwise).
- **AHCI IRQ**: MSI/MSI-X (preferred) or INTx on vector 42; `sched_sleep` on completion with poll fallback.
- **VirtIO-net multi-vector MSI-X**: modern path programs queue 0→entry0 (RX/40),
  queue 1→entry1 (TX/43); `virtio_net_msix_selftest` briefly `sti`s and checks TX
  IRQ count (`[VNET] [PASS] MSI-X TX/RX split`). Modern VERSION_1 uses 12-byte
  `virtio_net_hdr` (includes `num_buffers`); legacy stays at 10.
- **AHCI async NCQ + bio queue**: multi-slot CL + per-slot bounce (depth 4); large R/W pipelines multiple FPDMA commands. Shared bio queue (8) lets overlapping callers share the NCQ window (cross-caller).
- **blk_async end_io**: `blk_async_rw` / `ahci_async_rw` submit without waiting; `end_io(private, ok)` from `blk_async_poll` or a concurrent sync-waiter pump (IRQ only wakes — qlock is not IRQ-safe). Sync `blk_read`/`blk_write` unchanged. VirtIO/ATA sync-fallback completes inline then cb. Selftests: `[AHCI] [PASS] async NCQ multi-outstanding`, `[AHCI] [PASS] bio queue cross-caller`, `[AHCI] [PASS] blk_async end_io`.
- **Ext2 ordered meta-copy on blk_async**: `jl_write_meta_copies` / `jl_replay_meta_copies` pipeline up to **8** pool writes through `blk_async_write_batch` (bounce slots; AHCI overlaps NCQ in waves of 4; VirtIO/ATA inline). Replay skips homes listed in a committed REVOKE. Selftests: `[EXT2] [PASS] journal ordered blk_async meta-copy`, `journal copy pool grow`, `journal revoke-aware meta-copy replay`.
- **Ext2 data-path blk_async**: full-block file writes queue into an 8-slot bounce and submit via `blk_async_write_batch` before ORDERED commit/sync; `bread` drains only on hit. Selftest: `[EXT2] [PASS] data-path blk_async full-block`.

## Init post-COW login stall (TCG + VirtIO-blk) — fixed

**Symptom:** after COW/pthread/signal smoke, init never reached `fakeos login:` /
`ud:` under `make qemu-disk` TCG; AHCI (`qemu-ahci`) was fine. Hang sat on the
first userspace Ext2 op (`open("/test.txt")` / `mkdir("/mnt/home")`).

**Cause:** `ext2_selftest` runs *before* `sched_selftest`, so VirtIO-blk used the
spin+`io_wait` path (TCG retires host AIO). Once the scheduler was live, the
wait loop preferred `sched_sleep` on MSI — under TCG the VCPU stops polling,
the request never completes, MSI never arrives (deadlock). AHCI DMA does not
need that poll, so AHCI reached login.

**Fix:** VirtIO-blk completion waits with `io_wait` + `sched_yield` (no
chan-sleep). Init `file_test` uses Ramfs `/tmp/...` so VFS smoke does not
depend on Ext2 routing for unclaimed paths.

## Suggested next steps

1. Tiny CFS nesting polish (roots must stay NONE, not BSS 0) — low priority.
2. mmap-posix gaps (MAP_FIXED / mprotect / more POSIX) when a smokeable bite appears.
3. Full jbd-style revoke/descriptor interaction beyond the lite ring (still not full jbd).
4. VirtIO-net TX wait has a similar sleep pattern; revisit if TCG TX stalls show up.

Until then, Makefile default keeps `disable-modern=on` for blk and net; use
`make qemu-modern` to exercise the MMIO + MSI-X path.
