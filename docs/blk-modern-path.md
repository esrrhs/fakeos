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
Shell builtins: `ln` / `ln -s`, `readlink`. `ls -l` uses NOFOLLOW and shows `l`.
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

## ls -l mtime + getuid/chown stubs — landed

`struct stat` now carries `st_uid`/`st_gid` and atime/mtime/ctime (seconds;
boot-relative like `clock_gettime(REALTIME)`). Ramfs inodes store uid/gid/mtime;
Ext2 create/write stamps `i_uid`/`i_gid`/`i_mtime` (and `i_ctime` on chown).
Syscalls: `getuid`/`geteuid`/`getgid`/`getegid` (nr 102/107/104/108) and
`chown` (nr 92; `(u32)-1` leaves a field unchanged). Shell builtins: `id`,
`chown UID[:GID] FILE...`. `/bin/ls -l` prints `mode uid gid size mtime name`.
Init smoke: `[U][PASS] getuid/chown/mtime`. Focused virtio login smoke covers
id/chown/`ls -l` on Ramfs+Ext2 plus login/`uname`/`udptest`/`env` (smp 1/2).

## TTY Tab completion — landed

Canonical TTY line edit handles Tab: complete the token under the cursor
against cwd (relative) or the absolute directory prefix; command-position
tokens (start of line, no `/`) also search `/bin` and a builtin name table
kept in sync with shell help. Unique directory matches append `/`. No match
or no common-prefix progress rings BEL. Skipped when ECHO is off. Focused
virtio login smoke covers Tab (`ec`→`echo`, `/tm`→`/tmp/`, `/bin/l`→`/bin/ls`
or shared `l` prefix) plus login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).


## setuid/seteuid + groups — landed

Credential mutators on the TCB: `setuid`/`setgid` (nr 105/106), `setreuid`/`setregid`
(nr 113/114; `(u32)-1` unchanged — `seteuid` is `setreuid(-1, e)`), and
`getgroups`/`setgroups` (nr 115/116; up to 8 supplementary gids). Privilege is
`euid == 0`; otherwise uid/gid targets must match real or effective. New files
take `euid`/`egid`. `/etc/group` is published; `/bin/login` parses passwd uid/gid,
loads supplementary groups, then `setgroups`/`setgid`/`setuid` before exec (and
seeds `USER`/`HOME`/`FAKECC_PKG`). Shell `id` prints `groups=`; thin `setuid` /
`seteuid` builtins for interactive checks. Passwd ships `root` and `user` (1000).
Init smoke: `[U][PASS] setuid/seteuid/groups`. Focused virtio login smoke covers
id/groups/`seteuid`/`setuid` plus login/`uname`/`ls -l`/`udptest`/`env` (smp 1/2).

## TTY Double-Tab match listing — landed

Ambiguous Tab with no common-prefix progress still rings BEL on the first press
and sets a pending flag; a second Tab prints the match list (directories marked
with a trailing `/`, wrapped near 72 columns) then redisplays the edit buffer
(shell prompt is userspace-owned and not reprinted). Any other line edit clears
pending. Unique / progressing completions unchanged. Focused virtio login smoke
covers Double-Tab on `/bin/l` (lists `ldfake`/`login`/`ls`) plus prior Tab bites
and login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/tail + /bin/grep + /bin/printf — landed

Thin coreutils: `tail [-n N]|-N` (ring of last N lines, stdin OK), fixed-string
`grep [-ivnF]` (no BRE), and `printf` with `\n/\t/\r/...` escapes plus
`%s/%d/%u/%x/%c/%%`. Focused virtio login smoke covers the three plus
login/`uname`/`ls -l`/`udptest`/`id`/`head`/`wc` (smp 1/2).

## TTY Ctrl-W/K + quoted Tab — landed

Canonical line edit: Ctrl-W werases the word left of the cursor, Ctrl-K kills
to EOL, and Ctrl-V quotes the next key so Tab inserts a literal TAB (echoed as
one blank cell for 1:1 cursor math). Focused virtio login smoke covers the
three plus login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/tee + /bin/basename + /bin/dirname — landed

Thin coreutils: `tee [-a] [FILE...]` copies stdin to stdout and up to 8 files
(`-a` appends with `O_APPEND`); `basename PATH [SUFFIX]` and `dirname PATH`
strip path components (trailing-slash aware; `/` stays `/`). Focused virtio
login smoke covers the three plus login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/true + /bin/false + /bin/seq — landed

Thin coreutils: `true` exits 0, `false` exits 1; `seq [-s SEP] LAST` /
`FIRST LAST` / `FIRST INCREMENT LAST` prints integer sequences (empty range
OK; always trailing newline). Focused virtio login smoke covers the three
plus login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/cut + /bin/tr — landed

Thin coreutils: `cut -c LIST` / `cut -f LIST [-d DELIM] [-s]` selects
characters or fields (1-based `N`, `N-M`, `N-`, `-M`, comma lists); `tr SET1
SET2` translates and `tr -d SET` deletes (`a-z` ranges; `\n`/`\t`/`\r`/`\0`/
`\\` escapes). Focused virtio login smoke covers both plus login/`uname`/
`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/test + /bin/[ — landed

Thin POSIX `test`/`[` (exit 0 true / 1 false / 2 error). Unary file/string
ops (`-e/-f/-d/-h/-L/-s/-r/-w/-x/-z/-n` plus `-b/-c/-p/-S`), string `=`/`!=`,
integer `-eq/-ne/-lt/-le/-gt/-ge`, and a single leading `!`. The `[` form
requires a closing `]` (same ELF published as `/bin/test` and `/bin/[`).
Focused virtio login smoke covers file/string/int forms with `&&`/`||` plus
prior interactive greens (smp 1/2).


## /bin/uniq + /bin/sort — landed

Thin text-pipeline pair. `uniq [-c] [-d] [-u] [FILE...]` collapses consecutive
duplicate lines (`-c` count prefix, `-d` duplicates only, `-u` unique only).
`sort [-r] [-n] [FILE...]` loads lines and sorts lexicographically (`-r`
reverse, `-n` leading-integer numeric). Focused virtio login smoke covers
`sort | uniq`, `-c`/`-n`/`-r` bites plus prior interactive greens (smp 1/2).


## /bin/yes + /bin/od — landed

Thin coreutils pair. `yes [STRING...]` repeatedly writes a line (default `y`,
args joined by spaces) until a write fails (pipe close). `od [-A n|o|x|d]
[-t x1|o1|c] [-N COUNT] [FILE...]` dumps bytes 16-wide (default `-A o -t x1`;
`-An`/`-tx1` sticky forms accepted). Focused virtio login smoke covers
`yes | head`, `od -An -tx1 -N`, plus prior interactive greens (smp 1/2).

## shell echo -n/-e + /bin/xargs — landed

Shell builtin `echo [-neE] [ARGS...]`: `-n` suppresses the trailing newline,
`-e` enables backslash escapes (`\n`/`\t`/`\r`/`\\`/`\c`/octal; `\c`
stops output and implies `-n`), `-E` forces literal (default). Leading sticky
flag clusters (`-ne`) accepted; first non-flag operand ends option parse.
Thin `/bin/xargs [-n N] [-r] [COMMAND [ARGS...]]` whitespace-splits stdin into
batches (default COMMAND prints args; no `/bin/echo` required). Batches honor
`execve` `EXEC_MAX_ARGC=8`. `-r` skips empty stdin. Focused virtio login smoke
covers `echo -n/-e`, `xargs`/`-n`/`-r`, plus prior interactive greens (smp 1/2).

## shell mkdir -p + /bin/rm -r — landed

FS polish for daily-driver trees. Shell builtin `mkdir [-p] DIR...` creates
multiple operands; `-p` walks path components and treats an existing directory
as success (re-stat after mkdir; no errno). `/bin/rm [-r|-R] FILE...` removes
files with unlink and directories recursively via getdents name collection then
unlink/rmdir (skips `.`/`..`; refuses directories without `-r`). Focused virtio
login smoke covers nested `mkdir -p` / `rm -r` plus prior interactive greens
(smp 1/2).

## hard link(2) + shell ln — landed

Linux nr `link(86)`. Ramfs tracks `inode.nlink` (files/symlinks); Ext2 bumps
`i_links_count`. Same-FS only (`EXDEV` across Ramfs↔Ext2). Directories and
special nodes refused; `oldpath` is resolved nofollow (hard-link a symlink
inode like Linux). `unlink`/`rename`-replace drop one link and free only at
zero. Shell builtin: `ln TARGET LINKPATH` (hard) and `ln -s` (symlink). Init
smoke: `[U][PASS] hard link(2)` (Ramfs + Ext2 when mounted). Focused virtio
login smoke covers `ln` hard + prior interactive greens (smp 1/2).

## TTY Ctrl-L redraw/clear — landed

ICANON line edit: Ctrl-L clears the host serial terminal (CSI `2J` + `H`) and
VGA (`vga_clear`), then redisplays the edit buffer at the saved cursor
(shell prompt is userspace-owned, same as Double-Tab listing). With ECHO off,
clear only — do not paint the hidden buffer. Focused virtio login smoke covers
Ctrl-L bites + prior interactive greens (smp 1/2).

## /bin/cmp + /bin/dd — landed

Thin coreutils pair for scripting. `cmp [-l|-s] FILE1 FILE2` compares two files
byte-by-byte (exit 0 identical / 1 differ / 2 error; `-s` silent; `-l` lists
each differing byte as `POS OCT1 OCT2`; `-` = stdin). `dd [if=FILE] [of=FILE]
[bs=N] [count=N] [skip=N] [seek=N] [status=none]` copies blocks (default
`bs=512`, stdin→stdout; `N` accepts decimal with optional `k`/`b`/`M` suffix;
`skip`/`seek` are in blocks; `status=none` suppresses the records summary).
Focused virtio login smoke covers `cmp`/`-s`/`-l`, `dd` if/of/bs/count/skip +
prior interactive greens (smp 1/2).


## /bin/expr + /bin/hostname — landed

Thin coreutils pair for scripting. `expr INTEGER OP INTEGER` supports
`+ - * / %` and `= != < > <= >=` (integers; string `=`/`!=` when a side is
non-numeric); `expr length STRING`; `expr ARG` prints ARG. Exit 0 if the
result is non-zero/non-empty, 1 if zero/empty, 2 on error (`*` must be quoted
in the shell). `/bin/hostname` prints the `uname(2)` nodename field (no set).
Focused virtio login smoke covers `expr` arith/length/cmp + `hostname` plus
prior interactive greens (smp 1/2).


## /bin/echo — landed

Thin external `echo [-neE] [ARGS...]` mirroring the shell builtin: `-n`
suppresses the trailing newline, `-e` enables backslash escapes
(`\n`/`\t`/`\r`/`\\`/`\c`/octal; `\c` stops and implies `-n`), `-E`
forces literal (default). Sticky clusters (`-ne`) accepted. Lets
`xargs echo` / `/bin/echo` paths work without relying on the shell builtin.
Focused virtio login smoke covers `/bin/echo -n/-e/-E/-ne/\c`,
`xargs /bin/echo`, plus prior interactive greens (smp 1/2).

## /bin/sleep — landed

Thin external `sleep SECONDS` (non-negative decimal integer) via `nanosleep(2)`,
mirroring the shell builtin. Lets `xargs sleep` / script `exec` paths work
without relying on the builtin. Focused virtio login smoke covers
`/bin/sleep 0`/`1`, usage/invalid, `xargs /bin/sleep`, plus prior interactive
greens (smp 1/2).

## /bin/truncate — landed

Thin `truncate -s SIZE FILE...` via `open(2)` + `ftruncate(2)` (Ext2/Ramfs).
SIZE is a non-negative decimal integer with optional `k`/`K`/`b`/`B`/`M`
suffixes (same as dd). Missing files are created (0644). Focused virtio login
smoke covers shrink/grow/zero/create, usage/invalid, plus prior interactive
greens (smp 1/2).

## /bin/mkdir + /bin/pwd + /bin/ln — landed

Thin external shell mirrors for scripts/`xargs` that cannot use builtins:
`mkdir [-p] DIR...` (parents; existing dirs OK), `pwd` via `getcwd(2)`, and
`ln [-s] TARGET LINKPATH` via `link(2)`/`symlink(2)`. Focused virtio login
smoke covers `/bin/mkdir -p`, `/bin/pwd`, `/bin/ln` hard+`-s`, `xargs
/bin/mkdir`, plus prior interactive greens (smp 1/2).

## /bin/chmod + /bin/uname + /bin/env + /bin/sync — landed

Thin external shell mirrors: `chmod MODE FILE...` via `chmod(2)` (octal mode),
`uname [-a]` via `uname(2)` (sysname or five utsname fields), `env` dumping
`/proc/self/environ` (NUL-separated `NAME=val` from execve envp), and `sync`
via `sync(2)`. Focused virtio login smoke covers `/bin/chmod` + `ls -l`,
`/bin/uname`/`-a`, `/bin/env` (PATH/FAKECC_PKG), `/bin/sync`, plus prior
interactive greens (smp 1/2).

## /bin/readlink + /bin/realpath + /bin/printenv — landed

Thin external shell mirrors: `readlink PATH` via `readlink(2)`, `realpath PATH`
(physical resolve matching the shell builtin walk), and `/bin/printenv` as an
alias of `/bin/env` (same ELF published twice). Focused virtio login smoke
covers `/bin/readlink`, `/bin/realpath`, `/bin/printenv`, `xargs /bin/readlink`,
plus prior interactive greens (smp 1/2).

## /bin/date + /bin/stat — landed

Thin utils (not shell builtins): `date [+%s]` prints `clock_gettime(CLOCK_REALTIME)`
seconds (boot-relative; no RTC yet), and `stat [-L] FILE...` prints
type/mode/ino/nlink/uid/gid/size/mtime via `lstat(2)` (default) or `stat(2)`
with `-L`. Focused virtio login smoke covers `/bin/date`, `/bin/stat` on file
and symlink (+ `-L`), `xargs /bin/stat`, plus prior interactive greens (smp 1/2).

## /bin/nl + /bin/fold — landed

Thin text-pipeline pair. `nl [-b a|t|-ba|-bt] [-v START|-vN] [-w WIDTH|-wN] [-s SEP|-sSEP] [FILE...]`
numbers body lines (default `-b t` non-empty, `-v 1`, `-w 6`, tab sep).
Compact glued forms keep argv under `EXEC_MAX_ARGC=8`.
`fold [-w WIDTH] [-s] [FILE...]` wraps to WIDTH columns (default 80; `-s`
breaks at the last blank before WIDTH when possible). Focused virtio login
smoke covers `seq | nl`, `nl -ba -v10 -w2 -s:`, `fold -w`, `fold -s`,
pipeline `nl | fold`, plus prior interactive greens (smp 1/2).

## /bin/mktemp + /bin/rev + /bin/paste — landed

Thin script/text-pipeline trio. `mktemp [-d][-u|-du] [TEMPLATE]` creates a
unique file (default) or directory (`-d`) under a template ending in `XXXXXX`
(default `/tmp/tmp.XXXXXX`); `-u` dry-runs (create then unlink/rmdir). Uniqueness
via `getpid` + retry with `O_CREAT|O_EXCL` / `mkdir`.
`rev [FILE...]` reverses characters of each line.
`paste [-s] [-d LIST|-dLIST] [FILE...]` merges lines (default tab delim; `-s`
serial; max 6 files; one `-` stdin). Focused virtio login smoke covers
`mktemp`/`-d`/`-u`, `printf|rev`, `paste`/`-s`/`-d:`, plus prior interactive
greens (smp 1/2).

## /bin/expand + /bin/unexpand — landed

Thin text-pipeline pair. `expand [-t N|-tN] [FILE...]` converts tabs to spaces
(default stops every 8 columns). `unexpand [-a] [-t N|-tN] [FILE...]` converts
spaces to tabs (default leading blanks only; `-a` or `-t` converts all blank
runs that hit a stop). Compact glued `-tN` keeps argv under `EXEC_MAX_ARGC=8`.
Focused virtio login smoke covers `printf|expand`, `expand -t4`,
`printf|unexpand`/`-a`/`-t4`, round-trip `expand|unexpand`, plus prior
interactive greens (smp 1/2).

## /bin/comm + /bin/join — landed

Sorted-file pair. `comm [-123] FILE1 FILE2` prints three columns (unique to
FILE1 / unique to FILE2 / common); suppress with `-1`/`-2`/`-3` (combinable);
leading tabs only between columns that are printed; one `-` stdin allowed.
`join [-t C|-tC] [-1 N|-1N] [-2 N|-2N] FILE1 FILE2` joins on field 1 by default
(blank-run fields, or single-char `-t`); glued forms keep argv under
`EXEC_MAX_ARGC=8`. Thin join advances one pair per equal key (unique-key
smokes). Focused virtio login smoke covers `comm`/`-12`/`-13`, `join`/`-t:`/
`-12`, plus prior interactive greens (smp 1/2).

## Suggested next steps

1. Tiny CFS nesting polish (roots must stay NONE, not BSS 0) — low priority.
2. Harden simultaneous Ext2-redirect duplex / multi-nc SMP races — if a clear
   coherent fix exists; otherwise leave known flakes.
3. Deep jbd — avoid unless needed.
4. More daily-driver gaps: thin utils still missing (`which`/`type` external,
   `dirname` already present; consider `split`/`csplit`, `fmt`, external
   `/bin/which`) — prefer small coherent pairs.
   Avoid deep jbd / Ext2-redirect duplex races / CFS nesting unless a clear
   bite-sized fix.
