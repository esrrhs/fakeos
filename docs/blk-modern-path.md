# Block / virtio modern path (notes)

fakeos today is a **workstation-shaped** hobby OS, not a Linux replacement.
Storage and net bring-up intentionally stay on the simplest QEMU devices that
the in-tree drivers already speak.

## What ships today

| Path | QEMU hint | Driver | Notes |
|------|-----------|--------|-------|
| VirtIO-blk (legacy) | `-device virtio-blk-pci,disable-modern=on,...` | `drivers/virtio_blk.c` | PIO legacy virtio-pci; multi-sector R/W; FLUSH best-effort |
| **AHCI (SATA)** | `make qemu-ahci` / `-device ahci` + `ide-hd` | `drivers/ahci.c` | Single port, one slot; DMA R/W/FLUSH; ABAR relocated into boot HHDM when QEMU maps it high |
| ATA PIO | IDE disk | `drivers/ata.c` | Identify + LBA28 PIO; fallback when virtio/AHCI absent |
| VirtIO-net (legacy) | `virtio-net-pci,disable-modern=on` | `drivers/virtio_net.c` | INTx + I/O APIC; DHCP/TCP/UDP |

`drivers/blk.c` preference: **virtio-blk → AHCI → ATA**. Ext2 rides on that
block layer (journal-lite markers, multi-bg, triple-indirect).

## Why not modern virtio yet

- **Legacy virtio-pci** is enough for daily bring-up and keeps the MMIO/queue
  surface small for fakecc freestanding code.
- **Modern virtio 1.x** needs capability-list parsing (common/notify/ISR/device
  cfg), 64-bit features, and often MSI-X. QEMU's `disable-modern=on` exists
  precisely so guests can stay on the transitional/legacy path.

## Suggested implementation order

1. **PCI probe notes in-tree** *(done)*: log AHCI (`class 01.06`) with BAR0/ABAR
   and CAP/VS peek via HHDM when ABAR is low; walk vendor caps and log VirtIO
   modern `cfg_type` when present.
2. **AHCI read/write path** *(done)*: single port, one command slot, DMA
   IDENTIFY + READ/WRITE DMA EXT + FLUSH; keep ATA PIO / legacy virtio as
   fallback. `make qemu-ahci` exercises this backend.
3. **VirtIO modern blk/net** *(next)*: parse caps, map notify, feature bit
   negotiation; dual-compile or runtime select vs legacy.
4. **MSI/MSI-X**: after INTx paths are solid.
5. **AHCI IRQ / NCQ**: still polled; multi-slot NCQ not started.

Until modern virtio lands, Makefile/`make qemu` keep `disable-modern=on` for
blk and net. This document is the roadmap marker so workstation work does not
pretend virtio-1.0 or MSI are done.
