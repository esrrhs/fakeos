# Block / virtio modern path (notes)

fakeos today is a **workstation-shaped** hobby OS, not a Linux replacement.
Storage and net bring-up intentionally stay on the simplest QEMU devices that
the in-tree drivers already speak.

## What ships today

| Path | QEMU hint | Driver | Notes |
|------|-----------|--------|-------|
| VirtIO-blk (legacy) | `-device virtio-blk-pci,disable-modern=on,...` | `drivers/virtio_blk.c` | PIO legacy virtio-pci; multi-sector R/W; FLUSH best-effort |
| ATA PIO | IDE disk | `drivers/ata.c` | Identify + LBA28 PIO; fallback when virtio absent |
| VirtIO-net (legacy) | `virtio-net-pci,disable-modern=on` | `drivers/virtio_net.c` | INTx + I/O APIC; DHCP/TCP/UDP |

`drivers/blk.c` picks virtio-blk first, else ATA. Ext2 rides on that block
layer (journal-lite markers, multi-bg, triple-indirect).

## Why not AHCI / modern virtio yet

- **Legacy virtio-pci** is enough for daily bring-up and keeps the MMIO/queue
  surface small for fakecc freestanding code.
- **AHCI (SATA)** needs: PCI class `0x0106` BAR map, GHC/ports, command-list +
  FIS receive, at least one DMA slot, IRQ or MSI — a sizable driver before it
  beats ATA PIO for QEMU.
- **Modern virtio 1.x** needs capability-list parsing (common/notify/ISR/device
  cfg), 64-bit features, and often MSI-X. QEMU's `disable-modern=on` exists
  precisely so guests can stay on the transitional/legacy path.

## Suggested implementation order

1. **PCI probe notes in-tree** *(done)*: log AHCI (`class 01.06`) with BAR0/ABAR
   and CAP/VS peek via HHDM when ABAR is low; walk vendor caps and log VirtIO
   modern `cfg_type` when present. Still no queue/DMA programming.
2. **AHCI read path** *(next)*: single port, one command slot, 512-byte DMA
   identity + READ DMA EXT; keep ATA PIO / legacy virtio as fallback.
3. **VirtIO modern blk/net**: parse caps, map notify, feature bit negotiation;
   dual-compile or runtime select vs legacy.
4. **MSI/MSI-X**: after INTx paths are solid.

Until then, Makefile/`make qemu` keep `disable-modern=on` for blk and net.
This document is the roadmap marker so workstation work does not pretend AHCI
or virtio-1.0 are done.

