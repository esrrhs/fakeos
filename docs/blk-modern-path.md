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
| **VirtIO-net (modern)** | `make qemu-modern` / `disable-legacy=on` | `drivers/virtio_net.c` | Same cap map as blk; multi-vector MSI-X RX=40/TX=43 (per-queue); single MSI/MSI-X or INTx fallback; TX `io_wait`+`sched_yield` (no chan-sleep; TCG-safe like blk) |

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
  INTx. Blk vector 41; AHCI 42. AHCI still `sched_sleep`s on completion with
  poll fallback. VirtIO-blk and VirtIO-net TX wait with `io_wait`+`sched_yield`
  (no chan-sleep) so QEMU/TCG retires host AIO.
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

## VirtIO-net TX TCG wait — fixed

Same class as the blk login stall: multi-vector MSI-X TX used `sched_sleep` once
the scheduler was live. Under TCG that stops VCPU poll so host AIO/TX completion
never lands. TX now waits with `io_wait` + `sched_yield`; vector-43
`sched_wakeup` remains a harmless fast path if the IRQ arrives mid-spin.
Modern VERSION_1 hdr stays 12 bytes.

## mmap POSIX slice — landed

File `MAP_FIXED` builds `VMA_FILE` at a caller VA (overlap rejected, no silent
unmap). `mprotect(2)` (nr 10) changes VMA prot with munmap-style splits and
updates present leaf PTEs (COW-shared private pages stay RO until write fault).
Init smokes: `[U][PASS] file MAP_FIXED mmap` / `mprotect RO/RW`.

## stat / fstat / newfstatat — landed

Linux x86-64 `struct stat` (144 bytes) via `stat(4)` / `fstat(5)` / `lstat(6)` /
`newfstatat(262)`. Ramfs fills mode/size/rdev for files/dirs/char nodes;
Ext2 reads on-disk mode/size/nlink; pipes → `S_IFIFO`, sockets → `S_IFSOCK`.
`newfstatat` covers `AT_FDCWD`, relative names under a directory fd, and
`AT_EMPTY_PATH`. `ls -l` uses `fstatat(dirfd, name)` for mode/size. Init smoke:
`[U][PASS] stat/fstat/newfstatat`.

## /proc/self/{status,maps,cmdline,environ} — landed

Synthetic Ramfs nodes. Open snapshots live content:
`status` (Name/Pid/PPid/Threads/VmSize), `maps` (VMA lines + `[stack]`/`[heap]`),
`cmdline` (NUL-separated argv from exec/pid1), `environ` (NUL-separated
`NAME=val` from execve envp; empty envp seeds `FAKECC_PKG=/src`). Init smoke:
`[U][PASS] /proc/self status/maps/cmdline`.

## getppid / uname / nanosleep / clock_gettime — landed

Linux x86-64 nr `getppid(110)`, `uname(63)`, `nanosleep(35)`, `clock_gettime(228)`.
`uname` fills 6×65 `utsname` (`fakeos` / `0.1.0` / `x86_64`). Time is 100 Hz
`sched_ticks` (10 ms); no RTC yet so `CLOCK_REALTIME` tracks boot-relative like
`CLOCK_MONOTONIC`/`BOOTTIME`. `nanosleep` yields until the tick deadline (poll-style).
Shell builtins: `ppid`, `uname`[`-a`], `sleep N`. Init smoke:
`[U][PASS] getppid/uname/nanosleep/clock_gettime`.

## PROT_NONE + mmap addr hint — landed

`PROT_NONE` (prot 0) is stored without the historical `| PROT_READ`. Faults on a
NONE VMA are fatal. `mprotect(PROT_NONE)` keeps present frames but clears
`PTE_U`/`PTE_W` (guard pages must not lose data); lifting prot restores `PTE_U`.
Non-`MAP_FIXED` non-zero `addr` is a POSIX hint for anon and file maps (try VA,
else `mmap_next`). `MAP_FIXED` still rejects overlaps (no silent replace). Init
smoke: `[U][PASS] PROT_NONE + mmap addr hint`.

## symlink / readlink — landed

Linux nr `symlink(88)` / `readlink(89)`. Ramfs `INO_SYMLINK` + Ext2 `S_IFLNK`
(fast symlink in `i_block` when target < 60). Path walks follow intermediate
and (for open/stat/chdir) final symlinks with `SYMLOOP_MAX=8`; absolute Ext2
targets are remapped through `/mnt`/`/home` `persist_rel`. `lstat` /
`newfstatat(AT_SYMLINK_NOFOLLOW)` / `readlink` leave the final component.
`nice(2)` moved to fakeos nr 503 (Linux x86-64 has no `nice`; 89 is readlink).
Shell builtins: `ln -s`, `readlink`. `ls -l` uses NOFOLLOW and shows `l`.
Init smoke: `[U][PASS] symlink/readlink` (Ramfs + Ext2 when mounted).

## getcwd physical + shell path prompt + ls -l -> — landed

`chdir` stores a **physical** cwd (follow every symlink component; `CWD_LEN=128`
matches `PATH_KMAX`). `getcwd`/`pwd`/`cd ..` then match Linux physical semantics.
Shell prompt prints the cwd (`fakeos:~$` / `fakeos:~/sub$` / `fakeos:/path$`).
Builtin `realpath`; `ls -l` shows `name -> target` via readlink. Init smoke:
`[U][PASS] getcwd physical` (Ramfs + Ext2 when mounted).

## shell env / export / unset / which — landed

Shell environment table (default `PATH=/bin:/usr/bin:.`, `HOME=/home`,
`USER=root`, `FAKECC_PKG=/src`). Builtins: `env`, `export NAME=val`,
`unset NAME`, `which CMD` (PATH lookup or `shell builtin`). Whole-token
`$NAME` / `$?` expansion before run; bare `cd` uses `$HOME`.

## execve envp + live /proc/self/environ — landed

Kernel accepts up to 8 envp strings (128B each), builds SysV stack envp, and
stores NUL-separated `NAME=val` in the TCB for `/proc/self/environ` (snapshot
on open). Empty/NULL envp seeds `FAKECC_PKG=/src` so kbuild/fakecc keep working.
Shell passes its env table through `execve`. Focused virtio login smoke covers
`export` + `cat /proc/self/environ` alongside login/`uname`/`ls -l`/`udptest`.

## chmod / umask + Ext2 getdents / ls file operands — landed

`chmod(2)` (nr 90) updates Ramfs inode `mode` and Ext2 `i_mode` permission bits
(keeps `S_IFMT`; no ACL model). Per-TCB `umask` (default `0022`, fork/clone
inherits) is applied on Ramfs/Ext2 create/`mkdir`/`symlink` and on
`open(O_CREAT)` via the creat mode argument. Shell builtins: `chmod MODE FILE...`,
`umask` / `umask MODE` (octal). Ext2 `getdents` now matches the Ramfs 48-byte
record layout (`d_type@18`, `d_name@19`) so `ls` works on `/home`. `/bin/ls`
accepts file operands (`AT_EMPTY_PATH` fstat). Focused virtio login smoke covers
chmod/umask/`ls -l` on Ramfs+Ext2 plus login/`uname`/`udptest`/`env` (smp 1/2).

## Suggested next steps

1. Optional shell polish: tab completion, timestamps on `ls -l` (wire Ext2/Ramfs mtime into `stat`).
2. `chown`/`getuid` stubs if ownership display is wanted.
3. Tiny CFS nesting polish (roots must stay NONE, not BSS 0) — low priority.
4. Deep jbd — avoid unless needed.
