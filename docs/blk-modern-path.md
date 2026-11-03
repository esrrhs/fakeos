# Block / virtio modern path (notes)

fakeos today is a **workstation-shaped** hobby OS, not a Linux replacement.
Storage and net bring-up intentionally stay on the simplest QEMU devices that
the in-tree drivers already speak.

## What ships today

| Path | QEMU hint | Driver | Notes |
|------|-----------|--------|-------|
| VirtIO-blk (legacy) | `-device virtio-blk-pci,disable-modern=on,...` | `drivers/virtio_blk.c` | PIO legacy virtio-pci; multi-sector R/W; FLUSH best-effort |
| **VirtIO-blk (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_blk.c` | MMIO common/notify/ISR/device caps; polled completion; optional MSI |
| **AHCI (SATA)** | `make qemu-ahci` / `-device ahci` + `ide-hd` | `drivers/ahci.c` | Single port, one slot; DMA R/W/FLUSH; ABAR relocated into boot HHDM when QEMU maps it high |
| ATA PIO | IDE disk | `drivers/ata.c` | Identify + LBA28 PIO; fallback when virtio/AHCI absent |
| VirtIO-net (legacy) | `virtio-net-pci,disable-modern=on` | `drivers/virtio_net.c` | INTx + I/O APIC; DHCP/TCP/UDP |

`drivers/blk.c` preference: **virtio-blk → AHCI → ATA**. Ext2 rides on that
block layer (journal-lite markers, multi-bg, triple-indirect).

## Modern virtio status

- **Legacy virtio-pci** remains the default for `make qemu` / `qemu-disk` /
  `make test` (`disable-modern=on`) so bring-up stays small and deterministic.
- **Modern virtio 1.x** is implemented for **blk**: PCI vendor-cap walk maps
  common/notify/ISR/device cfg; feature select 0/1; queue_desc/driver/device;
  notify_off × multiplier. Tried when legacy I/O BAR0 is absent, or for
  modern-only device id `0x1042`. `pci_msi_enable` may program a single MSI
  vector (completion still polled).
- **VirtIO-net modern** and **MSI-X / IRQ-driven blk** are not done.
- **AHCI IRQ / NCQ**: still polled; multi-slot NCQ not started.

## Suggested next steps

1. Modern virtio-net (same cap map helpers) + optional MSI on net RX.
2. MSI-X multi-vector; blk used-ring IRQ instead of poll.
3. AHCI IRQ / NCQ.

Until then, Makefile default keeps `disable-modern=on` for blk and net; use
`make qemu-modern` to exercise the MMIO path. This document is the roadmap
marker so workstation work does not pretend MSI-X or modern-net are done.
