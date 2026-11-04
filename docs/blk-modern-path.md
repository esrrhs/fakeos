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
`grep [-ivnFE]` (thin `-E` ERE subset; default fixed), and `printf` with `\n/\t/\r/...` escapes plus
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

Thin coreutils: `true` exits 0, `false` exits 1; `seq [-w] [-s SEP] LAST` /
`FIRST LAST` / `FIRST INCREMENT LAST` prints integer sequences (`-w`
equal-width zero pad; empty range OK; always trailing newline). Focused virtio login smoke covers the three
plus login/`uname`/`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/cut + /bin/tr — landed

Thin coreutils: `cut -c LIST` / `cut -f LIST [-d DELIM] [-s]` selects
characters or fields (1-based `N`, `N-M`, `N-`, `-M`, comma lists); `tr SET1
SET2` translates and `tr -d SET` deletes (`a-z` ranges; `\n`/`\t`/`\r`/`\0`/
`\\` escapes). Focused virtio login smoke covers both plus login/`uname`/
`ls -l`/`udptest`/`id` (smp 1/2).

## /bin/test + /bin/[ — landed

Thin POSIX `test`/`[` (exit 0 true / 1 false / 2 error). Unary file/string
ops (`-e/-f/-d/-h/-L/-s/-r/-w/-x/-z/-n` plus `-b/-c/-p/-S`), string
`=`/`!=`/`</>` (byte-order lex), integer `-eq/-ne/-lt/-le/-gt/-ge`, `-ot`/
`-nt`, and a single leading `!`. The `[` form requires a closing `]` (same
ELF published as `/bin/test` and `/bin/[`). Focused virtio login smoke
covers file/string/int/lex forms with `&&`/`||` plus prior interactive
greens (smp 1/2).


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
Thin `/bin/xargs [-n N] [-r] [-I REPLACE] [COMMAND [ARGS...]]` whitespace-splits stdin into
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

Thin utils (not shell builtins): `date [+FORMAT]` prints `clock_gettime(CLOCK_REALTIME)`
(boot-relative; no RTC yet; bare/`+%s` seconds; FORMAT supports
`%s/%N/%Y/%m/%d/%H/%M/%S/%y/%%`), and `stat [-L] [-c FORMAT] FILE...` prints
type/mode/ino/nlink/uid/gid/size/mtime via `lstat(2)` (default) or `stat(2)`
with `-L` (`-c` thin GNU field formats). Focused virtio login smoke covers
`/bin/date`, `/bin/stat` on file and symlink (+ `-L`/`-c`), `xargs /bin/stat`,
plus prior interactive greens (smp 1/2).

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

## /bin/split + /bin/csplit — landed

File-splitting pair. `split [-l LINES|-lLINES] [-a N|-aN] [FILE [PREFIX]]`
writes PREFIX+alphabetic suffix pieces (default 1000 lines, PREFIX `x`,
suffix length 2 → `xaa`/`xab`/…); `-`/omitted FILE is stdin. `csplit
[-f PREFIX|-fPREFIX] [-n DIGITS|-nDIGITS] FILE LINE...` splits before each
1-based ascending LINE into PREFIX+zero-padded pieces (default `xx00`…),
printing each created path; numeric LINE args only (no regex). Glued forms
keep argv under `EXEC_MAX_ARGC=8`. Focused virtio login smoke covers
`split -l2`/`-a1`, `csplit -f`/`-n`, plus prior interactive greens (smp 1/2).

## /bin/fmt + /bin/which — landed

Ramfs `MAX_DENTS` raised 64→128 so `/bin` can hold the growing
daily-driver set (was exactly full at `csplit`). Thin text util + shell-mirror pair. `fmt [-w WIDTH|-wWIDTH] [FILE...]` fills
whitespace-separated words into lines of at most WIDTH columns (default 75;
historic `-WIDTH` accepted); blank lines separate paragraphs; words longer
than WIDTH stand alone. `which CMD...` searches `PATH` from
`/proc/self/environ` (default `/bin:/usr/bin:.`) and prints the first hit per
CMD (exit 0 iff all resolve); does not know shell builtins — the shell builtin
`which` still reports those. Glued `-wN` keeps argv under `EXEC_MAX_ARGC=8`.
Focused virtio login smoke covers `fmt`/`-w`/`-wN`, `/bin/which`, shell
`which` builtin still, plus prior interactive greens (smp 1/2).

## /bin/cksum + /bin/sum — landed

Checksum pair. `cksum [FILE...]` prints POSIX CRC and byte length (poly
`0x04C11DB7`, length folded as little-endian bytes, final complement) as
`CRC LENGTH [FILE]`; stdin/`-` omit the name. `sum [-r|-s] [FILE...]`
defaults to BSD (`-r`) 16-bit rotating checksum with 1024-byte blocks;
`-s` is System V (byte-sum folded to 16 bits, 512-byte blocks). Compact
argv-friendly; no glued forms needed. Focused virtio login smoke covers
`printf|cksum`, file `cksum`, `sum`/`-r`/`-s`, plus prior interactive
greens (smp 1/2).

## /bin/strings — landed

Thin printable-run extractor. `strings [-a] [-n N|-nN] [FILE...]` emits
runs of bytes in `0x20..0x7e` of length at least N (default 4); `-a` is
accepted as a no-op (whole-stream scan is the only mode). Glued `-nN`
keeps argv under `EXEC_MAX_ARGC=8`. stdin if no FILE. Focused virtio
login smoke covers stdin/`-n`/`-nN`, SOH-separated runs, ELF
`strings /bin/hello`, plus prior interactive greens (smp 1/2).


## /bin/pr + /bin/column — landed

Thin text-layout pair. `pr [-n] [-t] [-w WIDTH|-wWIDTH] [-l LENGTH|-lLENGTH]
[-h HEADER|-hHEADER] [FILE...]` paginates with a 5-line header (title + Page N)
and form feeds; body lines per page are LENGTH-5 (default LENGTH 66, WIDTH 72).
`-t` omits headers/form feeds; `-n` prefixes 5-digit line numbers + tab. `column
-t [-s SEP|-sSEP] [FILE...]` sizes columns from input and pads fields (default
whitespace-run split; `-s` treats each SEP char as a separator). Glued
`-wN`/`-lN`/`-hTEXT`/`-sSEP`/`-tsSEP` keep argv under `EXEC_MAX_ARGC=8`.
Focused virtio login smoke covers `pr -t`/`-n`/`-w`/`-l`/`-h`, `column -t`/
`-s`/`-ts:`, plus prior interactive greens (smp 1/2).

## /bin/md5sum + /bin/sha1sum — landed

Checksum follow-on with real digests (not zero stubs). `md5sum [FILE...]`
prints RFC 1321 MD5 as `DIGEST  FILE` (two spaces; `-` for stdin/`-`).
`sha1sum [FILE...]` prints FIPS 180-1 SHA-1 the same way. Streaming 64-byte
blocks; compact argv (no flags). Focused virtio login smoke covers empty/
`abc`/`hello` stdin digests, file operand, plus prior interactive greens
(smp 1/2). Ramfs `/bin` dents 77/128.

## /bin/sha256sum — landed

Checksum follow-on with real FIPS 180-4 SHA-256 (not a stub).
`sha256sum [FILE...]` prints `DIGEST  FILE` (two spaces; `-` for stdin/`-`).
Streaming 64-byte blocks; compact argv (no flags). Focused virtio login smoke
covers empty/`abc`/`hello` stdin digests, file operand, plus prior interactive
greens (smp 1/2). Ramfs `/bin` dents 78/128.

## /bin/base64 — landed

Thin encode/decode. `base64 [FILE...]` encodes with 76-column wrap and a
trailing newline (empty input stays empty). `base64 -d [FILE...]` decodes,
ignoring whitespace and rejecting other non-alphabet. stdin if no FILE;
compact `-d` argv. Focused virtio login smoke covers encode/decode round-trip
(`hello`/`abc`/pad cases), file operand, plus prior interactive greens
(smp 1/2). Ramfs `/bin` dents 79/128.

## /bin/factor + /bin/clear + /bin/tty + /bin/whoami — landed

Thin daily-driver set. `factor [NUMBER...]` prints ascending prime factors
(with multiplicity; stdin ints if no operands). `/bin/clear` writes ANSI
`ESC[2J ESC[H` (shell-mirror of builtin). `tty [-s]` uses ioctl TCGETS on
stdin and prints `/dev/tty` or `not a tty`. `whoami` maps geteuid() via
`/etc/passwd` (numeric fallback). Focused virtio login smoke covers factor
12/17/1/100, tty, whoami=root, clear escape, prior interactive greens
(smp 1/2). Ramfs `/bin` dents 83/128.

## /bin/id + /bin/groups — landed

External shell-mirrors. `/bin/id` prints `uid= euid= gid= egid=` and optional
`groups=` (comma-separated getgroups numbers), matching the shell builtin —
no USERNAME/`-n`/`-G` flags (compact argv). `/bin/groups` prints primary egid
then supplementary getgroups names via `/etc/group` (numeric fallback; skips
duplicate egid). Focused virtio login smoke covers `/bin/id` (root + wheel
supp), `groups` (`root wheel`), plus prior interactive greens (smp 1/2).
Ramfs `/bin` dents 85/128.

## /bin/cal — landed

External thin calendar. Forms: `cal MONTH YEAR` and `cal YEAR` (twelve months
stacked). Week starts Sunday; Gregorian. No bare `cal` — CLOCK_REALTIME is
boot-relative (no RTC), so current month is not inferred. Focused virtio login
smoke covers Nov 2026 / Mar 2024 / year 2024 markers plus prior interactive
greens (smp 1/2). Ramfs `/bin` dents 86/128.

## /bin/install — landed

Thin POSIX/GNU subset. Forms: `install [-m MODE|-mMODE] SOURCE DEST`,
`install [-m MODE|-mMODE] SOURCE... DIRECTORY`, and `install -d [-m MODE|-mMODE]
DIR...` (`-d` is mkdir -p style). Default MODE 0755; copy reuses open/read/write
then chmod; glued `-mMODE`/`-dmMODE` keep argv under `EXEC_MAX_ARGC=8`. Focused
virtio login smoke covers `-d`/`-m`/single copy/multi-into-dir plus prior
interactive greens (smp 1/2). Ramfs `/bin` dents 87/128.

## /bin/chown + /bin/chgrp — landed

External shell/POSIX mirrors. `chown UID[:GID] FILE...` (decimal; missing GID
leaves group unchanged via `(u32)-1`); `chgrp GID FILE...` changes group only
(`chown(2)` uid=`(u32)-1`). Compact argv for `EXEC_MAX_ARGC=8`. Focused virtio
login smoke covers `/bin/chown` uid:gid / uid-only, `/bin/chgrp`, Ramfs+Ext2
`ls -l` owners, plus prior interactive greens (smp 1/2). Ramfs `/bin` dents
89/128.

## /bin/tac + /bin/nproc + /bin/timeout — landed

Thin POSIX/GNU subset. `tac [FILE...]` prints lines in reverse (RAM-buffered
like sort). `nproc [-a|--all]` counts `processor` lines in boot-published
`/proc/cpuinfo` (filled from `smp_cpu_count()`; online==all, no affinity).
`timeout [-s SIG] SECONDS COMMAND [ARG...]` forks+execs, polls with 50ms
nanosleep + `wait4(WNOHANG)`, signal on deadline (default SIGKILL=9; `-s` /
`--signal`), exit 124; COMMAND without `/` resolves as `/bin/COMMAND`. Compact
argv for `EXEC_MAX_ARGC=8`. Focused virtio login smoke covers tac reverse,
nproc on smp 1/2, timeout success + timed-out sleep, plus prior interactive
greens. Ramfs `/bin` dents 92/128.

## /bin/rmdir + /bin/unlink — landed

Thin shell/POSIX mirrors for scripts/`xargs`. `rmdir [-p] DIR...` removes empty
directories via `rmdir(2)`; `-p` also strips empty parent components (mkdir -p
inverse; stops before `/`). Shell builtin `rmdir` matches (`DIR...` + `-p`).
`unlink FILE...` removes directory entries via `unlink(2)` (directories refused
— use rmdir). Completes the external mkdir/rm family. Focused virtio login smoke
covers Ramfs+Ext2 empty rmdir, unlink file, refuse unlink-on-dir / rmdir-on-file,
plus prior interactive greens (smp 1/2). Ramfs `/bin` dents 94/128.

## /bin/tsort + /bin/pathchk — landed

Thin POSIX pair for scripts/Make-ish graphs and pathname checks. `tsort
[FILE...]` reads whitespace-separated predecessor/successor pairs (stdin if
no FILE; odd trailing token is a lone node) and prints a topological order;
cycles diagnose on stderr. `pathchk [-p] PATH...` rejects empty paths; default
limits are PATH<=4095 / component<=255; `-p` uses portable charset
`[A-Za-z0-9._-]` with `_POSIX_PATH_MAX=256` / `_POSIX_NAME_MAX=14` (no
pathconf yet). Focused virtio login smoke covers tsort diamond + cycle,
pathchk ok/`-p` reject, plus prior interactive greens (smp 1/2). Ramfs `/bin`
dents 96/128.

## /bin/logname + /bin/nohup — landed

Thin POSIX pair. `logname` prints the login name: `LOGNAME` then `USER` from
`/proc/self/environ`, else `/etc/passwd` for `getuid()`, else numeric (no utmp).
Shell seeds `LOGNAME=root` alongside `USER`. `nohup COMMAND [ARG...]` ignores
SIGHUP; if stdout is a TTY appends to `./nohup.out`, mirrors stderr onto stdout
when stderr is a TTY, and redirects tty stdin from `/dev/null`. COMMAND without
`/` resolves as `/bin/COMMAND`. Compact argv for `EXEC_MAX_ARGC=8`. Focused
virtio login smoke covers logname, nohup→nohup.out, plus prior interactive
greens (smp 1/2). Ramfs `/bin` dents 98/128.

## /bin/cp + /bin/mv directory targets + rmdir -p — landed

`cp`/`mv` now accept `SRC DST` and `SRC... DIRECTORY`: when the destination
exists as a directory, each source lands at `DIR/basename(SRC)` (same join
helpers as `/bin/install`). Multi-source requires a directory last operand.
Still regular-file only (no `-r`). `/bin/rmdir -p` and shell `rmdir [-p]
DIR...` remove empty parents after the leaf. Also raises Ramfs `MAX_INODES`
256→512: after login, alloc was already up near inode ~252, so creating files
inside new dirs failed under modest smoke load. Focused virtio login smoke
covers cp/mv into dir + multi, rmdir -p nested, plus prior interactive greens
(smp 1/2). Ramfs `/bin` dents unchanged (98/128; no new publishes).

## /bin/cp -r recursive directory copy — landed

`/bin/cp` gains `-r`/`-R` recursive directory trees (getdents collect then
copy children; mkdir leaf dest; merge into existing dest dir). Still supports
`SRC DST` and `SRC... DIRECTORY` (`DIR/basename`). Without `-r`, directory
sources are refused. Cycle guard rejects DST under/equal to SRC. No new
`/bin` publish (dent count unchanged). Shell has no builtin `cp` — external
only. Focused virtio login smoke covers recursive tree copy (Ramfs + Ext2),
refuse-without-`-r`, self-copy refuse, prior interactive greens (smp 1/2).
Ramfs `/bin` dents 98/128.

## /bin/mv recursive directory trees — landed

`/bin/mv` moves directory trees: prefer `rename(2)` same-FS; on EXDEV/fail,
copy-tree (getdents collect + mkdir/merge) then recursive remove source.
Still supports `SRC DST` and `SRC... DIRECTORY` (`DIR/basename`). Cycle guard
rejects DST under/equal to SRC. No new `/bin` publish (dent count unchanged).
Shell has no builtin `mv` — external only. Focused virtio login smoke covers
Ramfs rename tree, cross-FS Ramfs→Ext2 copy+rm, into-dir, cycle refuse, prior
interactive greens (smp 1/2). Ramfs `/bin` dents 98/128.



## /bin/cp -n + /bin/mv -n noclobber — landed

Thin POSIX polish. `/bin/cp` gains `-n` (noclobber; sticky with `-r`/`-R` as
`-rn`/`-nr`): skip existing destinations quietly (exit 0). `/bin/mv` gains
`-n` the same way before `rename(2)` or cross-FS copy+rm. Recursive `cp -rn`
still creates missing dirs/files and only skips paths that already exist.
No new `/bin` publish (dent count unchanged). Shell has no builtin cp/mv —
external only. Focused virtio login smoke covers Ramfs + Ext2 noclobber for
cp/mv (file, into-dir, sticky `-rn`), prior interactive greens (smp 1/2).
Ramfs `/bin` dents ~106/128.


## /bin/find + /bin/du — landed

Thin tree-walk pair for daily-driver scripting (no kernel work). `find
[PATH...] [-name GLOB] [-type f|d] [-print]` walks with `getdents`+`lstat`;
paths are argv before the first primary (default `.`); `-name` matches the
basename with `*`/`?` only; predicates AND; default action `-print`. `du
[-a] [-s] [-h] [FILE...]` sums apparent `st_size` (default 1024-byte units,
ceil; `-h` human K/M/G); default prints each subdirectory total; `-a` includes
files; `-s` summary only.
Focused virtio login smoke covers find name/type, du -s/-a, Ext2 walk, prior
interactive greens (smp 1/2). Ramfs `/bin` dents 100/128.

## /bin/shuf + head/tail -c — landed

Thin POSIX polish. `/bin/shuf [-n N] [-i LO-HI] [-e ARG...] [FILE...]` loads
lines (or an inclusive decimal range / `-e` argv lines), Fisher-Yates shuffles
with a clock_gettime+getpid xorshift seed, then prints (`-n` caps output).
`head`/`tail` gain `-c N`/`-cN` byte modes alongside existing `-n`/`-N` line
modes; `tail -c` prefers `fstat`+`lseek` on seekable files and falls back to a
byte ring for stdin. No kernel work. Focused virtio login smoke covers shuf
range/file/ `-n`/`-e`, head/tail `-c`, prior greens (smp 1/2). Ramfs `/bin`
dents 101/128.

## wc -L + dirname multi-arg — landed

Thin POSIX polish (no new `/bin` dent; no kernel work). Inventory: no
`statfs(2)` yet, so thin `df` deferred. `wc` gains `-L` (and glued `-lwcL`)
for max line length in bytes (unterminated final line counts); default with
no flags remains lines/words/bytes. `dirname PATH...` accepts multiple
operands and prints one dirname per line (GNU multi-arg; single-path
behavior unchanged). Focused virtio login smoke covers `wc -L`/`-lL`,
`dirname` multi, Ext2 file, prior greens (smp 1/2). Ramfs `/bin` dents
101/128.

## touch -c + /bin/kill — landed

Thin POSIX / shell-mirror pair. Inventory: still no `statfs(2)`, so thin `df`
deferred. `touch` gains `-c` (do not create missing files; missing paths are
silent success). New `/bin/kill [-SIG] PID...` mirrors the shell builtin for
`xargs`/scripts (default SIGTERM=15; numeric signal 0..31; negative PID targets
process group per `kill(2)`). Compact argv for `EXEC_MAX_ARGC=8`. No kernel
work. Focused virtio login smoke covers touch `-c`, `/bin/kill -0` probe /
SIGTERM of background sleep, Ext2 touch, prior greens (smp 1/2). Ramfs `/bin`
dents 102/128.

## /bin/diff — landed

Thin line-oriented compare (companion to byte-wise `/bin/cmp`). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred). New `/bin/diff [-uq] FILE1 FILE2`: classic a/c/d hunks by default,
`-u` unified (3 lines context), `-q` brief; exit 0 identical / 1 differ / 2
error; `-` stdin at most once. LCS over loaded lines (MAX_LINES=1024). No
kernel work. Focused virtio login smoke covers identical/`-q`/classic/`-u` on
Ramfs+Ext2 plus prior greens (smp 1/2). Ramfs `/bin` dents 103/128.


## basename multi-arg + head -q/-v — landed

Thin POSIX/GNU polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred). `basename` gains `-a PATH...` and `-s SUFFIX PATH...` (traditional
`PATH [SUFFIX]` unchanged). `head` gains `-q` (never headers) / `-v` (always
headers); default still headers when more than one FILE. Focused virtio login
smoke covers basename `-a`/`-s`, head `-q`/`-v`, Ext2, prior greens (smp 1/2).
Ramfs `/bin` dents 103/128.

## printf %b/\c + pwd -L/-P — landed

Thin POSIX polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred). `printf` gains `%b` (expand the same backslash escapes in the
ARGUMENT) and `\c` (stop all further output) in FORMAT and `%b` args.
`/bin/pwd` and the shell `pwd` builtin accept `-L`/`-P`; kernel `chdir`
already stores a physical cwd, so both modes use `getcwd(2)`. Focused virtio
login smoke covers `printf %b`/`\c`, `pwd -L`/`-P`, Ext2, prior greens
(smp 1/2). Ramfs `/bin` dents 103/128.

## /bin/nice + /bin/arch + /bin/time — landed

Thin POSIX process/machine utilities. Inventory: still no `statfs(2)` (thin
`df` deferred) and no `utimensat(2)` (touch `-r`/`-d` deferred). New
`/bin/nice` wires `u_nice` to existing `sys_nice` (fakeos nr 503): bare
`nice` prints current niceness; `nice [-n ADJ] COMMAND...` / GNU short
`nice -ADJ COMMAND` adjust then exec (default +10; PATH-lite `/bin/`).
`/bin/arch` prints `uname` machine (`x86_64`). `/bin/time [-p] COMMAND...`
times with `clock_gettime(MONOTONIC)` around fork/wait; reports real on
stderr (`-p` adds stub user/sys 0.00). Focused virtio login smoke covers
nice/arch/time, Ext2, prior greens (smp 1/2). Ramfs `/bin` dents 106/128.


## cat -n/-b + tail -q/-v — landed

Thin POSIX polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred). `cat` gains `-n` (number all lines) / `-b` (number non-blank;
overrides `-n`); width-6 + tab like GNU. `tail` gains `-q` (never headers) /
`-v` (always headers); default still headers when more than one FILE (mirrors
`head`). Focused virtio login smoke covers `cat -n`/`-b`, `tail -q`/`-v`,
Ext2, prior greens (smp 1/2). Ramfs `/bin` dents 106/128.


## /bin/env -i/-u — landed

Thin POSIX env polish (no new `/bin` dent; `printenv` already aliases `/bin/env`;
no kernel work). Inventory: still no `statfs(2)` (thin `df` deferred) and no
`utimensat(2)` (touch `-r`/`-d` deferred). `/bin/env` gains `-i` / bare `-`
(clear inherited environment), `-u NAME` (unset; also `-uNAME`), `NAME=VALUE`
assignments, and optional `COMMAND [ARG...]` (exec with the resulting envp;
PATH-lite `/bin/` when COMMAND has no `/`). No COMMAND prints the resulting
environment. Inherited start state is `/proc/self/environ`. Empty envp still
seeds `FAKECC_PKG=/src` in the kernel (`EXEC_MAX_ENVC=8`). Shell builtin `env`
remains print-only — smoke uses `/bin/env`. Focused virtio login smoke covers
`/bin/env`, `-i`, `-i A=1`, `-i A=1 /bin/env`, `-u USER /bin/env`, Ext2, prior
greens (smp 1/2). Ramfs `/bin` dents 106/128.

## /bin/ls -l polish — landed

Thin POSIX ls polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred). `/bin/ls` gains `-a` / `-A` / `-d`, glued short options (`-la`,
`-ald`, …), and multi-PATH operands (directory `path:` headers when more than
one PATH). Default hides names starting with `.` (POSIX). `-a` synthesizes
`.` / `..` because Ramfs/Ext2 `getdents` omit them; `-A` shows other hidden
names without `.`/`..`. Long format adds `nlink` (`mode nlink uid gid size
mtime name`). Focused virtio login smoke covers default hide, `-a`/`-A`/`-ld`/
`-la`/nlink/multi-PATH, Ext2, prior greens (smp 1/2). Ramfs `/bin` dents
106/128.

## /bin/readlink -n/-f + /bin/which -a — landed

Thin path/shell-mirror polish (no new `/bin` dent; no kernel work). Inventory:
still no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/readlink` gains `-n`
(omit trailing newline) and `-f` (physical resolve via the same readlink-walk
algorithm as `/bin/realpath`; works for non-symlinks too). Glued short options
(`-nf`/`-fn`) accepted. `/bin/which -a` prints every PATH match (default still
first only). Shell builtins remain thin (single-operand `readlink` /
first-match `which`). Focused virtio login smoke covers `readlink`/`-n`/`-f`/
`-nf`, `which`/`-a`, Ext2 symlink, prior greens (smp 1/2). Ramfs `/bin` dents
106/128.

## grep -c + tr -c — landed

Thin filter polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `grep` gains `-c` (count
matching lines; multi-FILE prints `name:count`; suppresses line output; glued
with `-ivnF`). `tr` gains `-c`/`-C` (complement SET1) and glued `-cd`/`-dc`
(`tr -cd SET` keeps only SET). Focused virtio login smoke covers `grep -c`,
`tr -c`/`-cd`, Ext2, prior greens (smp 1/2). Ramfs `/bin` dents 106/128.



## date +FORMAT + stat -c — landed

Thin format-string polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/date` gains `+FORMAT`
beyond bare / `+%s`: `%s` `%N` `%Y` `%m` `%d` `%H` `%M` `%S` `%y` `%%` (calendar
fields treat boot-relative `CLOCK_REALTIME` seconds as Unix epoch — no RTC).
`/bin/stat` gains `-c FORMAT` / glued `-cFORMAT` / `-Lc`; thin GNU conversions
`%n` `%s` `%i` `%a` `%f` `%F` `%h` `%u` `%g` `%Y` `%%` (default multi-field line
unchanged). Focused virtio login smoke covers `date`/`+%s`/`+%Y-%m-%d`,
`stat -c`/`-Lc` symlink follow, Ext2, prior greens (smp 1/2). Ramfs `/bin`
dents 106/128.

## seq -w + du -h — landed

Thin display polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/seq` gains `-w`
(equal-width zero pad; width = max decimal width of endpoints including `-`).
`/bin/du` gains `-h` (human-readable integer 1024 powers: bare bytes / `K` /
`M` / `G`; default KiB units unchanged). Focused virtio login smoke covers
`seq -w`, `du -h`/`-sh`, Ext2, prior greens (smp 1/2). Ramfs `/bin` dents
106/128.

## xargs -I + sort -k + timeout --signal — landed

Thin POSIX polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/xargs` gains `-I REPLACE`
/ glued `-IREPLACE` (line-oriented; substitute REPLACE in initial ARGS once per
non-empty stdin line). `/bin/sort` gains `-k N` / `-kN` (blank-separated 1-based
field; combines with `-n`/`-r`; missing field sorts empty/0). `/bin/timeout`
gains `-s SIG` / `-sSIG` / `--signal SIG` / `--signal=SIG` (numeric 0..31;
default remains SIGKILL=9; fallback SIGKILL if chosen signal does not reap).
Focused virtio login smoke covers `xargs -I`, `sort -k`/`-n -k`, `timeout -s`/
`--signal`, Ext2, prior greens (smp 1/2). Ramfs `/bin` dents 106/128.

## base64 -w + split -b — landed

Thin POSIX/GNU polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/base64` gains `-w COLS`
/ glued `-wCOLS` (default 76; `0` disables wrapping entirely, including the
trailing newline). `/bin/split` gains `-b SIZE` / glued `-bSIZE` (byte mode;
decimal with optional `k`/`K`=1024, `m`/`M`=1MiB, `b`/`B`=512); `-l`/`-b` last
wins. Focused virtio login smoke covers wrap/decode, byte-split pieces, Ext2,
prior greens (smp 1/2). Ramfs `/bin` dents ~106/128.

## cal -y + uname -s/-n/-r/-m — landed

Thin display polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/cal` gains `-y YEAR`
(no RTC, so bare `-y` is rejected; existing `cal YEAR` / `MONTH YEAR`
unchanged). `/bin/uname` and the shell builtin gain `-s`/`-n`/`-r`/`-v`/`-m`/`-a`
(glued short options; multi-field POSIX snrvm order). Focused virtio login
smoke covers both plus prior greens (smp 1/2). Ramfs `/bin` dents ~106/128.

## chmod -R thin — landed

Thin recursive mode polish (no new `/bin` dent; no kernel work). Inventory:
still no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/bin/chmod` and the shell
builtin accept `chmod [-R|-r] MODE FILE...` (glued `-R`/`-r` ok). Depth-limited
tree walk (max 16) via `lstat`+`getdents`; post-order so a restrictive MODE
still reaches leaves; does not descend through symlink directories; entry cap
64/dir. Focused virtio login smoke covers builtin + `/bin/chmod -R` on Ramfs
trees plus prior greens (smp 1/2). Ramfs `/bin` dents ~106/128.

## find -maxdepth/-mindepth — landed

Thin tree-depth polish (no new `/bin` dent; no kernel work). Inventory: still no
`statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`/bin/find` gains `-maxdepth N` / `-mindepth N` (start points are depth 0;
GNU-ish: maxdepth gates descent, mindepth gates emit; AND with existing
`-name`/`-type`/`-print`). Focused virtio login smoke covers depth filters on
Ramfs trees plus prior greens (smp 1/2). Ramfs `/bin` dents ~106/128.

## shell set -e/-u — landed

Thin shell option polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
Shell builtin `set [-eu]|[+eu]` (glued ok); bare `set` prints `$-`-style flags.
`-e` exits after a finished command-list fails (mid `&&`/`||` short-circuit
returns do not errexit). `-u` errors on whole-token `$NAME` unbound and skips
the line (interactive: does not exit even with `-e`). Focused virtio login smoke covers nounset /
errexit short-circuit / `set +e` survival plus prior greens (smp 1/2). Ramfs
`/bin` dents ~106/128. Jobs/`fg`/`bg` already present — left alone.

## test -ot/-nt + expr substr/index — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`test`/`[` gain `-ot`/`-nt` against `st_mtime` (boot-relative secs; missing-file
rules match common POSIX/bash). `expr` gains `index STRING CHARS` (1-based; 0
if none) and `substr STRING POS LEN` (1-based POS; empty on invalid POS/LEN).
Focused virtio login smoke covers mtime order / missing-file / index / substr
plus prior greens (smp 1/2). Ramfs `/bin` dents ~106/128. Jobs/`fg`/`bg`
already present — left alone.

## dd conv= + od -N/-j polish — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`dd` gains `conv=LIST` (comma-separated): `notrunc` (omit `O_TRUNC` on `of=`),
`sync` (pad short reads to `bs` with NUL before write), `fsync` (`fsync(2)` on
`of=` before close; uses existing syscall). `od` gains glued `-NCOUNT` and
`-j SKIP`/`-jSKIP` (prefer `lseek` SEEK_CUR, else read-discard; dump addresses
start at skip). Focused virtio login smoke covers sync pad / notrunc vs trunc /
fsync / `-N4` / `-j2` plus prior greens (smp 1/2). Ramfs `/bin` dents ~106/128.
Jobs/`fg`/`bg` already present — left alone.

## grep -E thin ERE — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`grep` gains `-E` (glued with `-ivncF`): thin ERE subset `|` `.` `*` `+` `?`
`()` `^` `$` and `\` escapes; no `[]` classes, no `{}` bounds. Default remains
fixed-string; `-F` forces fixed and wins over `-E`. Focused virtio login smoke
covers `.`/`*`/`+`/`|`/`^`/`$`/`()`/`?`/escapes/`-Ei`/`-EF`, Ext2, prior greens
(smp 1/2). Ramfs `/bin` dents ~106/128. Jobs/`fg`/`bg` already present — left
alone.

## od -t u1/d1 + dup-line suppress — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred — Ramfs has no block accounting, so
statfs+df not clearly bite-sized) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`od` gains `-t u1`/`d1` (and glued `-tu1`/`-td1`; unsigned width-3 /
signed width-4 space-padded) plus GNU-style duplicate full-width line
collapse to a single `*` (disable with `-v` / glued `-vtx1`/`-vtu1`/…).
Focused virtio login smoke covers u1/d1 / `*` / `-v` / prior `-N`/`-j`, Ext2,
prior greens (smp 1/2). Ramfs `/bin` dents ~106/128. Jobs/`fg`/`bg` already
present — left alone.

## grep -E [] classes — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred — Ramfs has no block accounting, so
statfs+df not clearly bite-sized) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
`grep -E` gains thin `[]` character classes: ranges, leading `^` negate,
leading `]`/`-` literal, `\` escapes next inside `[]`; no `[:class:]`
named classes, no `{}` bounds. `find_alt` / `()` matching skip bracket
spans so `|`/`()` inside classes stay literal. Focused virtio login smoke
covers `[abc]` / `[a-z]` / `[^0-9]` / leading `]` / `-Ei` class / class+`*`
/ `|` inside class / Ext2 / prior greens (smp 1/2). Ramfs `/bin` dents
~106/128. Jobs/`fg`/`bg` already present — left alone.

## /bin/sed thin — landed

Thin new `/bin/sed` (one new Ramfs `/bin` dent; no kernel work). Inventory:
still no `statfs(2)` (thin `df` deferred — Ramfs has no block accounting, so
statfs+df not clearly bite-sized) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
Jobs/`fg`/`bg` already present — left alone. Shell `$(( ))` / for/while and
`stty`/`reset`/`tput`/`watch` deferred (winsize ioctl absent for tput
cols/lines).

`sed [-n] SCRIPT [FILE...]`: semicolon-separated commands, no addresses —
`s/PAT/REPL/[g]` fixed-string substitute (any delimiter; `\`/`\n`/`\t`
escapes), `d` (delete/suppress print), `p` (print), `q` (quit after line).
`-n` quiet. stdin if no FILE. No regex, no hold space. Compact argv for
`EXEC_MAX_ARGC=8`. Focused virtio login smoke covers `s///` / `s///g` /
alt delim / `-n`+`p` / `d` / `q` / pipe+file / Ext2 / prior greens (smp 1/2).
Ramfs `/bin` dents ~107/128.

## shell $(( )) arithmetic expand — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred — Ramfs has no block accounting, so
statfs+df not clearly bite-sized) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
Jobs/`fg`/`bg` already present — left alone. Thin `for`/`while`/`if` deferred
(need per-iteration re-expand of `$NAME`); `stty`/`reset`/`tput`/`watch`
deferred (no `TIOCGWINSZ`).

`expand_token` now scans each argv token for `$?`, `$NAME`, and
`$((...))` (not whole-token-only). Thin integer arith: `+ - * / %`,
parentheses, unary `+/-`, whitespace inside `$(( ))` (or quote the token).
Inside arith, `$NAME` or bare `NAME` loads the shell env (unset → `0`;
`set -u` errors). Division by zero / syntax errors abort the line
(`expand_err`). Focused virtio login smoke covers basic ops / parens /
`$VAR` / bare name / `$?` / inline mix / div0 / Ext2 prior greens (smp 1/2).
Ramfs `/bin` dents ~107/128.

## shell for/while/if + ; — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: still
no `statfs(2)` (thin `df` deferred — Ramfs has no block accounting, so
statfs+df not clearly bite-sized) and no `utimensat(2)` (touch `-r`/`-d`
deferred); `mkfifo` still needs `mknod` (absent). `/proc` is still only
`cpuinfo` + `self/{status,maps,cmdline,environ}` — skip `ps`/`uptime`/`free`.
Jobs/`fg`/`bg` already present — left alone. (`TIOCGWINSZ` + stty/tput/reset
landed later; see below.)

`strtok_sh` now emits `;` as its own token (glued `c;do` works). Top-level
and compound bodies run via `run_seq` (`;`-separated `&&`/`||` lists).
Forms: `for NAME in WORDS; do LIST; done`, `while LIST; do LIST; done`,
`if LIST; then LIST; [else LIST;] fi`. Word list of `for` expands once;
loop/then/else bodies re-expand `$NAME` / `$?` / `$(( ))` each iteration.
`set -e` is suppressed for while/if conditions (POSIX). while iteration
cap 256. No nesting / elif / until. Focused virtio login smoke covers
for / while+test+arith / if/else / `;` seq / Ext2 prior greens (smp 1/2).
Ramfs `/bin` dents ~107/128.

## TIOCGWINSZ + stty/tput/reset — landed

`ioctl(TIOCGWINSZ)` returns fixed VGA 80×25 (no SIGWINCH). `/bin/stty`
(`size|sane|echo|-echo|raw|-raw|[-a]`), `/bin/tput` (`cols|lines|clear|cup`),
`/bin/reset` (sane + ANSI clear). Ramfs `/bin` dents ~110/128. Focused virtio
login smoke green on smp 1/2. Prefer `/bin/watch` next if bite-sized.

## shell until/elif/break/continue — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc SMP races, CFS, deep jbd alone.

Forms: `until LIST; do LIST; done` (invert of while), `if`…`elif`…`then`…
`[else …]` `fi`, builtins `break`/`continue` (error outside loops).
`run_seq` gathers `for`/`while`/`until`/`if` through nesting-aware
`done`/`fi`, and re-expands `$` / `$?` / `$(( ))` per `;`-segment so
mid-body `export` is visible to a later `if test`. `set -e` suppressed on
until/while/if conditions; iteration cap 256. Focused virtio login smoke
covers until / elif chain / break+continue in for|while / Ext2 prior greens
(smp 1/2). Ramfs `/bin` dents ~110/128.

## shell case/esac — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc SMP races, CFS, deep jbd alone.

Forms: `case WORD in PAT) LIST ;; … esac`. Fixed patterns and simple `*`/`?`
globs (same thin matcher as `find -name`). Alternatives via glued `a|b)` or
spaced `a | b)`. WORD expanded once; each pattern alt expands `$` then glob-
matches. Nesting-aware `esac` alongside `done`/`fi`. No match → status 0;
matched clause returns last body status. `;;` is two `;` tokens from
`strtok_sh`. Focused virtio login smoke covers exact / `*` / `?` / `|` /
`$VAR` / default `*` / no-match + prior greens (smp 1/2). Ramfs `/bin`
dents ~110/128.

## /bin/watch — landed

Thin `/bin/watch [-n SEC|-nSEC] COMMAND [ARG...]` (default interval 2s):
ANSI clear + fork/exec/wait + `nanosleep` loop. COMMAND without `/` resolves
as `/bin/COMMAND`. Compact argv for `EXEC_MAX_ARGC=8`. No header line; pair
with `/bin/timeout` in smokes. Also correct ANSI clear write length to 7
(`ESC[2J ESC[H`) in `watch`/`clear`/`reset`/`tput`/shell builtin (was 11,
leaking NUL + trailing rodata into the serial console). Inventory: `/proc` still thin — skip
`ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` — `df`, `touch -r|-d`,
`mkfifo` deferred. Left Ext2-redirect duplex / multi-nc SMP races, CFS, deep
jbd alone. Focused virtio login smoke covers `timeout`+`watch -n1` echo +
prior greens (smp 1/2). Ramfs `/bin` dents ~111/128.

## printf %o + export -n / type / command -v — landed

Thin userland polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc; `hash` /
`read` builtin were next (landed below).

`printf` gains `%o` (octal; `%x` already). Shell: `export -n NAME` clears the
export attribute while keeping `$NAME` (env table gains an `exported` flag;
`env` / bare `export` / `execve` envp only emit exported entries). Builtins
`type NAME` and `command -v NAME` (pathname or builtin name; exit 1 if
missing). Focused virtio login smoke covers `%o` / export -n keep-value /
type builtin+PATH / command -v plus prior greens (smp 1/2). Ramfs `/bin`
dents ~111/128.


## shell read + hash — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc.

Shell builtins: `read [-r] [NAME...]` reads one stdin line (ICANON or `<`
redirect; byte loop to newline). No NAME → `REPLY`. IFS whitespace split;
last NAME gets the remainder; missing fields → empty; EOF → status 1 (vars
cleared). `-r` accepted (always raw). New bindings stay unexported.
`hash` lists `NAME=PATH`; `hash -r` clears; `hash NAME` resolves+remembers
(error on builtin/missing). `resolve_cmd` consults the table, forgets stale
hits, and auto-remembers successful PATH opens (cap 16). Focused virtio
login smoke covers read one/REPLY/fields/`-r`/EOF + hash list/clear/miss/
builtin/auto + prior greens (smp 1/2). Ramfs `/bin` dents ~111/128.

## shell pushd/popd/dirs + cd - — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc; shell
functions / alias / `[[ ]]` were still deferred then.

Directory stack cap 8 (`dirstack[0]` = cwd): `pushd DIR` pushes then cds;
bare `pushd` swaps top two; `popd` drops cwd and cds to next; `dirs` prints
space-separated stack. `cd -` uses unexported `OLDPWD` (set on successful
`cd`/`pushd`/`popd`) and prints the new cwd. Tab completion lists the new
builtins. Focused virtio login smoke covers pushd/popd/dirs/swap/`cd -` +
prior greens (smp 1/2). Ramfs `/bin` dents ~111/128.

## shell alias/unalias — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc; shell
functions / `[[ ]]` were still deferred then.

Alias table cap 16 (`ALIAS_NAME`/`ALIAS_VAL` 32/96): `alias` lists
`NAME='VAL'`; `alias NAME=VAL` sets (quote the whole `NAME=multi word` arg
when VAL has spaces); `alias NAME` prints one; `unalias NAME...` /
`unalias -a`. One-shot first-word expand per pipeline stage (no recursion).
`type` / `which` / `command -v` report aliases. Tab completion lists the new
builtins. Focused virtio login smoke covers set/list/expand/unalias/`-a` /
type + prior greens (smp 1/2). Ramfs `/bin` dents ~111/128.

## shell functions/local — landed

Thin shell polish (no new `/bin` dent; no kernel work beyond Tab builtin names).
Inventory: `/proc` still thin — skip `ps`/`uptime`/`free`. No `statfs`/
`utimensat`/`mknod` — `df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect
duplex / multi-nc SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc;
`[[ ]]` still deferred.

Function table cap 8 (`FUNC_NAME`/`FUNC_BODY` 32/160): `name () { body; }` or
`name() { body; }` (first `}` closes; body re-tokenized on call). Cap depth 4.
`return [N]` exits the current function; `local NAME[=VAL]...` saves/restores
env entries for the frame. Positional `$0`..`$9` and `$#` expand inside
functions. Lookup order after alias: function → builtin → PATH. `type` /
`which` / `command -v` report functions; Tab lists `return`/`local`. Focused
virtio login smoke covers define/call/`$1`/`$#`/`return`/`local`/type + prior
greens (smp 1/2). Ramfs `/bin` dents ~111/128.

Thin `/bin` polish (3 new dents). Inventory unchanged on deferred kernel gaps
(`/proc` thin → skip `ps`/`uptime`/`free`; no `statfs`/`utimensat`/`mknod` →
`df`/`touch -r|-d`/`mkfifo` deferred). Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc; `[[ ]]`
still deferred.

`/bin/hexdump [-Cv] [-n LEN|-nLEN] [-s OFF|-sOFF] [FILE...]`: canonical
`-C` hex+ASCII (default); duplicate full-width lines collapse to `*` (`-v`
disables). `/bin/xxd [-pr] [-l LEN|-lLEN] [-s OFF|-sOFF] [-c COLS|-cCOLS]
[IN [OUT]]`: address dump (default), `-p` plain hex, `-r`/`-rp` reverse to
binary (OUT only with `-r`). `/bin/file FILE...`: thin magic via `lstat` +
prefix read (empty / ELF / `#!` / gzip / PNG / ASCII text / directory /
symlink / data). Glued `-n`/`-s`/`-l`/`-c` keep argv under `EXEC_MAX_ARGC=8`.
Focused virtio login smoke covers hexdump/xxd round-trip/`file` + prior
greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell [[ ]] — landed

Thin shell polish (no new `/bin` dent; no kernel work). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc.

`[[ EXPR ]]` builtin: string `==` / `=` / `!=`, `-z` / `-n`, file tests
mirroring thin `test(1)` (`-e/-f/-d/-h/-L/-b/-c/-p/-S/-s/-r/-w/-x`), unary
`!`, and left-to-right inner `&&` / `||` (no regex, no `( )`). `run_list`
gathers from `[[` through closing `]]` so inner `&&`/`||` are not shell list
ops; shell-level `[[ … ]] && cmd` still works. Focused virtio login smoke
covers string/file/`&&`/`||`/`!`/if + prior greens (smp 1/2). Ramfs `/bin`
dents ~114/128.

## shell shift + $@/$* — landed

Thin shell polish (no new `/bin` dent; Tab lists `shift`). Inventory: `/proc`
still thin — skip `ps`/`uptime`/`free`. No `statfs`/`utimensat`/`mknod` —
`df`, `touch -r|-d`, `mkfifo` deferred. Left Ext2-redirect duplex / multi-nc
SMP races, CFS, deep jbd alone. Skip large `getopts` / here-doc.

`shift [N]` (default 1) left-shifts function positionals; fails if `N > $#`
or non-numeric. `$@` / `$*` expand to space-joined `$1..$#` (thin; same;
no IFS). Bare `$@` / `$*` in `for x in …` expand to one word per positional.
Focused virtio login smoke covers shift/`$@`/`$*`/for + prior greens
(smp 1/2). Ramfs `/bin` dents ~114/128.

## shell :/true/false — landed

Thin shell polish (no new `/bin` dent; Tab lists `true`/`false`/`:`).
Inventory unchanged on deferred kernel gaps. `/bin/true` and `/bin/false`
already published; shell builtins shadow them for no-fork use and enable
`while :` (no `/bin/:`).

`:` / `true` always exit 0; `false` exits 1; arguments ignored (POSIX).
Focused virtio login smoke covers `:`/`true`/`false`/type/`while :` + prior
greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell eval + source/. — landed

Thin shell polish (no new `/bin` dent; Tab lists `eval`/`source`). Inventory
unchanged on deferred kernel gaps.

`eval [ARGS…]` joins args with spaces and re-tokenizes via `run_seq` (empty
→ 0). `source FILE` / `. FILE` reads up to 512 bytes, maps newlines to `;`,
then same path; no-slash names use thin PATH lookup (`resolve_cmd`). Shared
nesting cap 4. Focused virtio login smoke covers eval/source/`.` + prior
greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell readonly — landed

Thin shell polish (no new `/bin` dent; Tab lists `readonly`). Inventory
unchanged on deferred kernel gaps.

`readonly` lists `readonly NAME=val`; `readonly NAME[=VAL]` sets/marks.
`sh_setenv` / `sh_unsetenv` reject later writes (export/read/for/local paths
included); missing `unset` still OK. Focused virtio login smoke covers
set/list/reject-assign/reject-unset + prior greens (smp 1/2). Ramfs `/bin`
dents ~114/128.

## shell wait — landed

Thin shell polish (no new `/bin` dent; Tab lists `wait`). Inventory: `unset`
already lists names and respects readonly — left alone. Deferred kernel gaps
unchanged.

`wait` waits for all active jobs; `wait PID` / `wait %N` for one. Blocking
`wait4` until exit; stopped jobs get `SIGCONT` first. Returns last child's
status (127 if unknown). Focused virtio login smoke covers
`true&wait`/`false&wait`/`wait %1` + prior greens (smp 1/2). Ramfs `/bin`
dents ~114/128.

## shell command [-vV] + run — landed

Thin shell polish (no new `/bin` dent). Inventory: `command -v` already
existed — extended carefully. Deferred kernel gaps unchanged.

`command NAME [ARGS…]` runs NAME skipping alias and function lookup
(builtin via nested `run_builtin`, else fork+PATH exec). `-v` keeps short
identify; `-V` verbose (type-like). Focused virtio login smoke covers
alias/function bypass + `-v`/`-V` + prior greens (smp 1/2). Ramfs `/bin`
dents ~114/128.

## shell [[ ]] numeric — landed

Thin shell polish (no new `/bin` dent). Inventory: `/bin/printf` already has
`%x/%X/%u/%s/%d/%o/%b` (width still absent); `test` already has numeric ops;
`[[` had only string/`=`/`!=` + file tests — extended carefully. `umask`
exists; no `ulimit`/`trap` yet (trap deferred as larger). Deferred kernel
gaps unchanged.

`[[ A -eq|-ne|-lt|-le|-gt|-ge B ]]` parses signed decimals (same rules as
thin `test`). Focused virtio login smoke covers numeric true/false/`!` +
prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell jobs -l — landed

Thin shell polish (no new `/bin` dent). Inventory: jobs/fg/bg/wait already
present — `-l` was the bite-sized gap. Printf width / ulimit / trap left for
later (trap not clearly bite-sized). Deferred kernel gaps unchanged.

`jobs [-l]` lists active jobs; `-l` inserts PID after the `+/-` marker
(`[N]+ PID Running NAME`). Focused virtio login smoke covers `true &` +
`jobs -l` + `wait` + prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## /bin/printf width — landed

Thin `/bin/printf` polish (no new `/bin` dent; no shell printf builtin).
Inventory: specs `%s/%c/%d/%i/%u/%x/%X/%o/%b/%%` already present — added
width/flags only. `ulimit`/`trap` deferred. Deferred kernel gaps unchanged.

Flags `-` (left) and `0` (zero-pad; `-` wins); decimal width before the
conversion. Applies to `d/i/u/x/X/o/s` (and thin `c`/`b`). No precision /
`*` / `#`. Focused virtio login smoke covers `%5d`/`%04d`/`%-5s`/`%08x` +
prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell builtin — landed

Thin shell `builtin NAME [ARGS…]` (no new `/bin` dent). Inventory: kernel has
no `getrlimit`/`setrlimit`/`RLIMIT_*` — skip `ulimit` rather than invent a
fake kernel. `trap`/`getopts`/here-doc still deferred.

`builtin` runs NAME only via `run_builtin` (skip alias/function/PATH); errors
with `not a shell builtin` when NAME is unknown to `is_builtin`. Mirrors the
run half of `command` without PATH fallback. Tab-completion + help/banner
updated. Focused virtio login smoke covers `builtin echo` / non-builtin
reject / function shadow bypass + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## shell dirs -v/-p — landed

Thin `dirs [-v|-p]` polish on existing pushd/popd/dirs (no new `/bin` dent).
`-p`: one path per line; `-v`: bash-ish indexed (` N  PATH`) one-per-line;
glued `-vp`/`-pv` ok (`-v` wins). Default remains space-joined. Focused
virtio login smoke covers default/`-p`/`-v` after pushd + prior greens
(smp 1/2). Ramfs `/bin` dents ~114/128.

## shell set -o — landed

Thin `set -o`/`+o` on existing `shell_opt_e`/`shell_opt_u` (no new `/bin`
dent). Bare `set -o`/`+o` lists `errexit`/`nounset` on|off; `set -o NAME` /
`set +o NAME` toggles `errexit`/`nounset`. Existing `set -e`/`-u`/`+e`/`+u`
(and glued) unchanged. Focused virtio login smoke covers list / -o errexit /
+o nounset / legacy -eu + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## shell type -t — landed

Thin `type [-t] NAME…` on existing type (no new `/bin` dent). Inventory:
`hash`/`hash -r`/`hash NAME` already present — left alone. `-t` prints one
word: `alias`/`keyword`/`function`/`builtin`/`file` (silent fail if missing).
Verbose form also reports shell keywords. Keyword set is thin reserved words
(`if`/`for`/`[[`/…). Focused virtio login smoke covers `-t` cases + prior
greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell export/readonly -p — landed

Thin `export -p` / `readonly -p` portable list forms (no new `/bin` dent).
Bare `export`/`readonly` already printed `export NAME=val` /
`readonly NAME=val`; `-p` is now accepted (glued `-np` ok with export `-n`).
`-p` with no names lists; with names, still sets/marks as before. Inventory
skipped shell printf (larger), `cd -L/-P` (cwd already physical), and
`printenv` (env binary already aliases). Focused virtio login smoke covers
`-p` lists + prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell cd -L/-P — landed

Thin `cd [-L|-P] [DIR|-]` (no new `/bin` dent). Kernel cwd is already
physical (same as `pwd -L/-P`); flags accepted as no-ops. Bare `-` remains
OLDPWD and is not treated as a flag. Glued `-LP`/`-PL` ok. Focused virtio
login smoke covers `cd -P /tmp`, `cd -L`, `cd -` + prior greens (smp 1/2).
Ramfs `/bin` dents ~114/128. Shell printf mirror still larger — defer unless
clearly bite-sized.

## shell printf — landed

Thin shell `printf` builtin mirroring `/bin/printf` (no new `/bin` dent).
Careful copy of width/flags helpers into `sh.c` (`spf_*` / `sh_do_printf`) to
avoid clashing with C `printf`. Specs: `%s/%c/%d/%i/%u/%x/%X/%o/%b/%%` with
`-`/`0`/decimal width. Format escapes match `/bin/printf`. Focused virtio
login smoke covers builtin width cases + prior greens (smp 1/2). Ramfs
`/bin` dents ~114/128.

## shell alias -p — landed

Thin `alias [-p]` portable list form (no new `/bin` dent). Inventory:
`unalias -a` already present — left alone. `set --` deferred (positionals
are function-scoped; no top-level argv frame yet). `-p` lists/prints
`alias NAME='VAL'`; bare `alias` keeps `NAME='VAL'`. Focused virtio login
smoke covers `-p` list + `unalias -a` + prior greens (smp 1/2). Ramfs
`/bin` dents ~114/128.

## shell help NAME — landed

Thin `help` / `help NAME` (no new `/bin` dent). Bare `help` keeps the
existing banner and adds a pointer to `help NAME`. Named form uses a small
synopsis table; unknown-but-builtin falls back to `NAME: shell builtin`;
non-builtins error. Focused virtio login smoke covers bare/`help printf`/
`help nosuch` + prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell [[ ==/!= glob — landed

Thin `[[ str == pat ]]` / `!=` pattern match with `*` and `?` only (no
`[…]` classes; no new `/bin` dent). Reuses existing `case_glob`. RHS is
the pattern (bash-ish). Exact equality still works when pat has no meta. Inventory: no `getrusage`/
`times` syscall → skip `times`; classic `/bin` utils mostly present;
`set --` still deferred. Focused virtio login smoke covers `*`/`?`/`!=`
+ prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell help table coverage — landed

Expanded `sh_help_tab` so remaining builtins that fell through to
`NAME: shell builtin` get one-line synopses (`mkdir`/`rmdir`/`ln`/
`readlink`/`realpath`/`uname`/`pid`/`ppid`/`sleep`/`sync`/`clear`/
`chown`/`id`/`setuid`/`seteuid`). Preferred names (wait/export/[[/…)
were already covered. No new `/bin` dent. Focused virtio login smoke
covers new synopses + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## shell [[ </> lexicographic — landed

Thin `[[ a < b ]]` / `[[ a > b ]]` string compare via `strcmp` (byte order;
no locale). `extract_redirects` skips `<`/`>`/`>>` inside `[[…]]` so lex
ops are not stolen as redirects. Inventory: `&&`/`||` cover logical and/or
(`-a`/`-o` not added); `read -r` present (`-p` deferred); echo `-neE`
already there. No new `/bin` dent. Focused virtio login smoke covers `<`/`>` + prior
greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell read -p — landed

Thin `read [-r] [-p PROMPT] [NAME…]` (no new `/bin` dent). `-p` writes
PROMPT to stderr (bash-ish) before reading; glued `-pPROMPT` ok; `-r`
unchanged (always raw). Focused virtio login smoke uses file redirect
input so QEMU stays non-interactive (`read -p … < file`) + prior greens
(smp 1/2). Ramfs `/bin` dents ~114/128.

## shell let — landed

Thin `let NAME=EXPR…` builtin (no new `/bin` dent). Reuses `arith_eval` /
`$(( ))` and `fmt_long_dec`/`sh_setenv`. Exit status: 0 if last result
nonzero, 1 if zero, 2 on error. Inventory: `tee -a` / `xargs -n` already
present; `declare`/`typeset`/`=~`/`%q` deferred. Focused virtio login
smoke covers assign/status + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## shell declare/typeset — landed

Thin `declare`/`typeset` (alias) with `-r`/`-x`/`-p` and `NAME[=VAL]` (no
arrays; no new `/bin` dent). Reuses `sh_setenv` / `sh_mark_export` /
`sh_mark_readonly`. New bindings without `-x` clear the default export bit
`sh_setenv` sets (bash-like shell-local). `-p` prints
`declare -x|-r|-rx|-- NAME=val`. Focused virtio login smoke covers
assign/export/readonly/print + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## shell hash -p — landed

Thin `hash -p PATH NAME` remembers NAME→PATH without a PATH walk (bash-like;
no existence check). Keeps `hash -r`, bare list (`NAME=PATH`), and
`hash NAME` resolve+remember. No new `/bin` dent. Focused virtio login
smoke covers `-p`/list/`-r` + prior greens including declare (smp 1/2).
Ramfs `/bin` dents ~114/128.

## shell hash -t/-l — landed

Thin `hash -t NAME…` prints remembered path(s) (single name → path only;
multiple → `NAME\tPATH`; missing → error, no PATH walk). `hash -l [NAME…]`
lists reusable `hash -p PATH NAME` lines; `-lt` prints tab form for the
table/filter. No new `/bin` dent. Focused virtio login smoke covers
`-t`/`-l`/`-lt` + prior greens including `hash -p` (smp 1/2). Ramfs `/bin`
dents ~114/128.

## test/[ </> lex — landed

`/bin/test` and `/bin/[` gain binary string `<` / `>` (byte-order
`strcmp`, bash-like; mirrors shell `[[ a < b ]]`). No new `/bin` dent.
Focused virtio login smoke covers `test`/`[` lex + prior greens including
hash -t/-l (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell local -r/-x — landed

Thin `local [-rx] NAME[=VAL]…` (glued `-rx` ok) inside functions. Reuses
`sh_mark_export` / `sh_mark_readonly`; local frame save/restore now tracks
`was_readonly` and unwinds with `sh_ro_override`. No new `/bin` dent.
Focused virtio login smoke covers `-r`/`-x`/restore + prior greens
including test lex (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell break/continue [N] — landed

`break [N]` / `continue [N]` (default 1). `loop_ctrl_n` carries remaining
levels; `run_loop_body` decrements on the way out (continue N>1 breaks the
inner loop and continues the outer). N above `loop_depth` clamps to the
outermost (POSIX). Existing `run_seq`/`run_list` early-return on
`loop_ctrl` propagates across nesting. No new `/bin` dent. Focused virtio
login smoke covers nested `break 2` / `continue 2` + prior greens (smp 1/2).
Ramfs `/bin` dents ~114/128.

## grep -q — landed

`/bin/grep` gains `-q` (quiet: no stdout; exit 0/1 by match; glued with
`-ivncFE`; suppresses `-c` counts; may stop at first hit). No new `/bin`
dent. Inventory: shell mostly saturated (`exec`/`select`/`coproc` deferred
or larger); picked clear util flag gap. Focused virtio login smoke covers
`grep -q` + prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## shell exec — landed

Thin `exec [CMD [ARG…]]` builtin (no new `/bin` dent). Reuses existing
redirect push (`<`/`>`/`>>` on stdin/stdout — not `N>file`) and
`resolve_cmd`/`u_execve`/`sh_build_envp`. Bare `exec` (or redirects only)
keeps the redirected fds. `exec CMD` replaces the shell image; rejects
builtins/functions. Failed `execve` keeps redirects and returns 127.
Focused virtio login smoke uses pipeline/child forms so the login shell
survives (`exec /bin/echo … | cat`) plus redirect-stick/`/dev/tty` restore
and prior greens (smp 1/2). Ramfs `/bin` dents ~114/128.

## sort -u — landed

`/bin/sort` gains `-u` (unique: after sort, skip consecutive equal keys).
Key equality mirrors sort rules (`-n` / `-k`; no `-r`; no full-line
tiebreak under `-k`). Glued with `-rnu`. No new `/bin` dent. Inventory:
sed `-n`, uniq `-c`, head `-n`, cut `-f/-d`, tr `-d`, sort `-r/-n/-k`
already present — `-u` was the clear gap. Focused virtio login smoke
covers `sort -u` / `-nu` + prior greens (smp 1/2). Ramfs `/bin` dents
~114/128.

## grep -l/-L — landed

`/bin/grep` gains `-l` (print matching path only) and `-L` (print
non-matching path only). Last of `-l`/`-L` wins when glued. Scans quietly
(reuses `-q` early-exit for `-l`); exit 0 if at least one name printed.
Glued with `-ivncqFE`. No new `/bin` dent. Focused virtio login smoke
covers `-l`/`-L` + prior greens including sort -u (smp 1/2). Ramfs `/bin`
dents ~114/128.

## cat -s — landed

`/bin/cat` gains `-s` (squeeze consecutive blank lines to one; state
carries across FILE operands). Keeps `-n`/`-b` (line-oriented path when
any of `-nbs`). Glued `-nbs` ok. No new `/bin` dent. Focused virtio login
smoke covers `cat -s` / `-ns` + prior greens including grep -l/-L
(smp 1/2). Ramfs `/bin` dents ~114/128.

## kill -l — landed

`/bin/kill -l` lists signal names 1..31 (thin table: HUP..SYS) or resolves
`SIGNUM`↔`NAME`/`SIGNAME` (case-insensitive). Existing `kill [-SIG] PID…`
unchanged. Shell builtin `kill -l` forks/`execve`s `/bin/kill` so the table
stays in one place. No new `/bin` dent. Focused virtio login smoke covers
`kill -l` / `-l 15` / `-l TERM` + prior greens (smp 1/2). Ramfs `/bin`
dents ~114/128.

## tr -s — landed

`/bin/tr` gains `-s` squeeze: collapse consecutive repeats of chars in the
last SET (`tr -s SET`, `tr -s SET1 SET2` translate-then-squeeze SET2,
`tr -ds SET1 SET2` delete then squeeze). Glued `-cs`/`-ds`/`-cds` ok with
existing `-c`/`-d`. No new `/bin` dent. Focused virtio login smoke covers
`tr -s` / `-s` translate / `-ds` + prior greens including kill -l
(smp 1/2). Ramfs `/bin` dents ~114/128.

## join -a — landed

`/bin/join` gains `-a N` / `-aN` (N=1|2): also emit unpairable lines from
file N (join field + remaining fields of that file). Keeps `-t`/`-1`/`-2`.
Drains leftovers after the merge walk. No new `/bin` dent. Focused virtio
login smoke covers `join -a1`/`-a2` + prior greens including tr -s
(smp 1/2). Ramfs `/bin` dents ~114/128.

## grep -w/-x — landed

`/bin/grep` gains `-w` (match only at word boundaries: alnum/_) and `-x`
(match whole line). Works for fixed and thin `-E` paths; glues with
existing `-ivncqlLFE`. No new `/bin` dent. Focused virtio login smoke
covers `grep -w`/`-x`/`-iw` + prior greens including join -a (smp 1/2).
Ramfs `/bin` dents ~114/128.

## sort -f + uniq -i — landed

`/bin/sort -f` folds A–Z→a–z for string compares, field keys (`-k`), and
`-u` key equality (numeric `-n` still primary). `/bin/uniq -i` compares
consecutive lines case-insensitively (emits the first spelling). Both
glue with existing `-rnu` / `-cdu`. No new `/bin` dent. Focused virtio
login smoke covers `sort -f`/`-fu` and `uniq -i` + prior greens including
grep -w/-x (smp 1/2). Ramfs `/bin` dents ~114/128.

## ls -1/-F — landed

`/bin/ls` accepts `-1` (one name per line; already the short default,
ignored under `-l`) and `-F` (classify short names: `/` dir, `*` exec,
`@` symlink, `|` fifo, `=` sock; uses fstatat). Short listing no longer
always appends `/` (only with `-F`). Keeps `-l/-a/-A/-d` and glued
clusters. No new `/bin` dent. Focused virtio login smoke covers `ls -1` /
`ls -F` + prior greens including sort -f / uniq -i (smp 1/2). Ramfs
`/bin` dents ~114/128.

## md5sum/sha256sum -c — landed

`/bin/md5sum -c` and `/bin/sha256sum -c` verify GNU checksum lines
(`DIGEST  FILE` or `DIGEST *FILE`), print `PATH: OK` / `PATH: FAILED`,
exit nonzero on any failure. Generate mode unchanged. No new `/bin` dent.
Focused virtio login smoke covers `-c` OK/FAILED + prior greens including
ls -1/-F (smp 1/2). Ramfs `/bin` dents ~114/128.

## sha1sum -c — landed

`/bin/sha1sum -c` mirrors md5/sha256 check mode (40-hex DIGEST lines;
`PATH: OK` / `FAILED`). Generate mode unchanged. No new `/bin` dent.
Focused virtio login smoke covers `sha1sum -c` + prior greens including
md5/sha256 -c (smp 1/2). Ramfs `/bin` dents ~114/128.

## find -iname — landed

`/bin/find -iname GLOB` is case-insensitive `-name` (fold A–Z in thin
`*`/`?` glob on basename). Last of `-name`/`-iname` wins. Keeps
`-type`/`-print`/`-maxdepth`/`-mindepth`. No new `/bin` dent. Focused
virtio login smoke covers `-iname` + prior greens including sha1sum -c
(smp 1/2). Ramfs `/bin` dents ~114/128.

## join -v — landed

`/bin/join -v N` / `-vN` (N=1|2): emit unpairable lines from file N and
suppress pairable (joined) lines. Implies `-a N`. Keeps `-a`/`-t`/`-1`/`-2`.
No new `/bin` dent. Focused virtio login smoke covers `join -v1`/`-v2` +
prior greens including find -iname (smp 1/2). Ramfs `/bin` dents ~114/128.

## diff -b/-w — landed

`/bin/diff -b` ignores changes in whitespace amount (runs match; trailing
ws-only OK). `-w` ignores all whitespace. Glues with existing `-uq`. Output
still shows original lines. No new `/bin` dent. Focused virtio login smoke
covers `diff -b`/`-w`/`-qb` + prior greens including join -v (smp 1/2).
Ramfs `/bin` dents ~114/128.

## uniq -f/-s — landed

`/bin/uniq -f N` skips N blank-separated fields before comparing; `-s N`
skips N characters after that. Forms `-f N`/`-fN` and `-s N`/`-sN`. Keeps
`-cdui`. Output still emits the full original line. No new `/bin` dent.
Focused virtio login smoke covers `uniq -f`/`-s`/`-f1s1` + prior greens
including diff -b/-w (smp 1/2). Ramfs `/bin` dents ~114/128.

## cksum -c — landed

`/bin/cksum -c` verifies `CRC LENGTH FILE` lines (POSIX/GNU untagged).
Keeps generate mode; open failures name the path; `--` supported. No new
`/bin` dent. Focused virtio login smoke covers `cksum -c` OK/FAILED + prior
greens including uniq -f/-s (smp 1/2). Ramfs `/bin` dents ~114/128.

## sort -b — landed

`/bin/sort -b` ignores leading blanks on the sort key (whole-line or `-k`
field). Glues with existing `-rnuf`/`-k`. Output still emits original lines.
No new `/bin` dent. Focused virtio login smoke covers `sort -b` + prior
greens including cksum -c (smp 1/2). Ramfs `/bin` dents ~114/128.

## find -path — landed

`/bin/find -path GLOB` matches the full path with the same thin `*`/`?`
glob as `-name` (no casefold). ANDs with `-name`/`-iname`/`-type`/depth.
No new `/bin` dent. Focused virtio login smoke covers `find -path` + prior
greens including sort -b (smp 1/2). Ramfs `/bin` dents ~114/128.

## find -prune — landed

`/bin/find -prune`: when predicates match a directory, do not descend
(thin AND model; no `-o`/parens). Matched dirs still print under default
`-print`. No new `/bin` dent. Focused virtio login smoke covers
`find -name DIR -prune` + prior greens including find -path (smp 1/2).
Ramfs `/bin` dents ~114/128.

## ls -R — landed

`/bin/ls -R` recurses into subdirectories (`-d` wins). Subdir blocks get a
blank line and `path:` header. Thin caps: depth 8, 32 subdirs per dir.
Keeps `-a/-A/-l/-1/-F`. No new `/bin` dent. Focused virtio login smoke
covers `ls -R` + prior greens including find -prune (smp 1/2). Ramfs
`/bin` dents ~114/128.

## du -d — landed

`/bin/du -d N` / `-dN` prints totals only for depth ≤ N below the operand
(GNU-ish; still walks full tree for sizes). `-s` remains summary (= depth 0);
last of `-s`/`-d` wins. Keeps `-a`/`-h`. No new `/bin` dent. Focused virtio
login smoke covers `du -d` + prior greens including ls -R (smp 1/2). Ramfs
`/bin` dents ~114/128.

## sed -i — landed

`/bin/sed -i` edits FILE in place (same-dir `.sedtmp` + `rename(2)`; no
backup extension). Refuses stdin/`-`. Glues with `-n`. Keeps thin `s///g`
`d`/`p`/`q`. No new `/bin` dent. Focused virtio login smoke covers `sed -i`
+ prior greens including du -d (smp 1/2). Ramfs `/bin` dents ~114/128.

## find -empty — landed

`/bin/find -empty` matches empty regular files (`st_size == 0`) or empty
directories (no getdents entries beyond `.`/`..`). ANDs with other
primaries. No new `/bin` dent. Focused virtio login smoke covers
`find -empty` + prior greens including sed -i (smp 1/2). Ramfs `/bin`
dents ~114/128.

## cmp -n — landed

`/bin/cmp -n N` / `-nN` compares at most N bytes (GNU-ish). Within the
limit, equal prefixes exit 0 even if the rest differs. Keeps `-l`/`-s`.
No new `/bin` dent. Focused virtio login smoke covers `cmp -n` + prior
greens including find -empty (smp 1/2). Ramfs `/bin` dents ~114/128.

## date +%F/%T/%R — landed

`/bin/date` gains thin strftime composites: `%F` (`%Y-%m-%d`), `%T`
(`%H:%M:%S`), `%R` (`%H:%M`). Keeps existing `%s/%N/%Y/%m/%d/%H/%M/%S/%y/%%`.
No new `/bin` dent. Focused virtio login smoke covers `date +%F`/`+%T`/`+%R`
+ prior greens including cmp -n (smp 1/2). Ramfs `/bin` dents ~114/128.

## realpath multi-PATH — landed

`/bin/realpath PATH...` prints one resolved path per operand (GNU multi-arg).
Continues after failures; exit non-zero if any PATH fails. No new `/bin`
dent. Focused virtio login smoke covers multi-arg `realpath` + prior greens
including date +%F/%T/%R (smp 1/2). Ramfs `/bin` dents ~114/128.

## Suggested next steps

1. Leave Ext2-redirect duplex / multi-nc SMP races and CFS nesting unless a
   clear bite-sized fix (CFS roots must stay NONE, not BSS 0).
2. Deep jbd — avoid unless needed.
3. More daily-driver gaps after inventory: prefer small coherent chunks. Keep
   `df`/`touch -r|-d`/`mkfifo`/`ps`/`uptime`/`free`/`tar`/`gzip`/`awk`
   deferred. Next thin polish: `grep -o` / `od -w` / `xargs -0` / other util
   flags (`cp -p` waits on utimensat; `set --` deferred; skip large shell).
   Leave Ext2 duplex / multi-nc / CFS / jbd alone.
