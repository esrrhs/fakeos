# Block / virtio modern path (notes)

fakeos today is a **workstation-shaped** hobby OS, not a Linux replacement.
Storage and net bring-up intentionally stay on the simplest QEMU devices that
the in-tree drivers already speak.

## What ships today

| Path | QEMU hint | Driver | Notes |
|------|-----------|--------|-------|
| VirtIO-blk (legacy) | `-device virtio-blk-pci,disable-modern=on,...` | `drivers/virtio_blk.c` | PIO legacy virtio-pci; multi-sector R/W; FLUSH best-effort |
| **VirtIO-blk (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_blk.c` | MMIO common/notify/ISR/device caps; MSI-X/MSI vector 41; IRQ wake + poll fallback |
| **AHCI (SATA)** | `make qemu-ahci` / `-device ahci` + `ide-hd` | `drivers/ahci.c` | Single port, multi-slot CL; DMA R/W/FLUSH; async NCQ with per-slot bounce (depth 4) when CAP.SNCQ; public blk API still sync/serialized; MSI/MSI-X or INTx vector 42 + `sched_sleep`; ABAR relocated into boot HHDM when QEMU maps it high |
| ATA PIO | IDE disk | `drivers/ata.c` | Identify + LBA28 PIO; fallback when virtio/AHCI absent |
| VirtIO-net (legacy) | `virtio-net-pci,disable-modern=on` | `drivers/virtio_net.c` | MSI-X/MSI or INTx; DHCP/TCP/UDP |
| **VirtIO-net (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_net.c` | Same cap map as blk; MSI-X/MSI vector 40 on RX |

`drivers/blk.c` preference: **virtio-blk → AHCI → ATA**. Ext2 rides on that
block layer (journal-lite markers, multi-bg, triple-indirect).

## Modern virtio status

- **Legacy virtio-pci** remains the default for `make qemu` / `qemu-disk` /
  `make test` (`disable-modern=on`) so bring-up stays small and deterministic.
- **Modern virtio 1.x** for **blk and net**: PCI vendor-cap walk maps
  common/notify/ISR/device cfg; feature select 0/1; queue_desc/driver/device;
  notify_off × multiplier. `vmm_map_mmio` brings PCI-hole BARs into HHDM.
- **MSI / MSI-X**: `pci_msi_enable` / `pci_msix_enable_one` (MSI first, then
  MSI-X; both mask INTx). Net vector 40; blk vector 41. Blk `sched_sleep`s on
  used-ring IRQ when the scheduler is live (spin fallback otherwise).
- **AHCI IRQ**: MSI/MSI-X (preferred) or INTx on vector 42; `sched_sleep` on completion with poll fallback.
- **AHCI async NCQ**: multi-slot CL + per-slot bounce (depth 4); large R/W pipelines multiple FPDMA commands (multi-outstanding). Public blk API remains sync and serializes one transfer via `busy`. Selftest: `[AHCI] [PASS] async NCQ multi-outstanding`.

## Suggested next steps

1. Multi-vector MSI-X (per-queue) for net TX/RX split.
2. Cross-caller AHCI bio queue (true async beyond single-transfer pipeline).
3. AP trampoline + per-CPU stacks (SMP stubs/ICR helpers already land).

Until then, Makefile default keeps `disable-modern=on` for blk and net; use
`make qemu-modern` to exercise the MMIO + MSI-X path.
