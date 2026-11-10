# Daily command-line status (fakeos)

This is a short note of what a login shell can actually do today, not a
roadmap and not a claim that fakeos is a daily driver.

## Landed in this increment

Live `/proc/<pid>/{status,maps,cmdline,environ}` for every visible user
process (including unreaped zombies), `/proc/self` (unchanged contract),
and `/proc/meminfo` built from real PMM page counters (`MemTotal`,
`MemFree`, `MemAvailable`, `MemUsed` in kB — no invented Cached/Buffers
figures). `/bin/ps` and `/bin/free` read those nodes; writes, creates,
unlinks and renames on the synthetic tree are refused. Numeric pid
directories are bound to a reserved inode slot on lookup because
`next_pid` grows past the thread-table size.

A QEMU login workflow is `tools/m5_daily_proc_smoke.py`: `ps` shows the
shell as pid 1, `/bin/sleep 20 &` appears, `kill` plus `wait` removes it,
and `free` / `cat /proc/meminfo` show a non-zero `MemTotal`.

Init selftest: `[U][PASS] /proc pids meminfo`.

Userland ELFs are published from a packed `user_blob_table` in
`user/blob.S` so `rootfs.c` does not take a GOT entry per `*_start`/`*_end`.
That keeps the kernel link under fakecc `--kernel-multiboot`'s current
256-entry GOT cap (which fails silently if exceeded).

## Limits

`ps` lists user process leaders only (no kernel idle threads, no `-L`
thread rows, no tty/time/RSS columns). `/proc/<pid>/fd` and a richer
`/proc/stat` are not present. `free` is kB-only (`-k` accepted).
After `init` execve's into `login`/`sh`, pid 1 is the shell, not
`/bin/init`.

## Bounded daily CLI acceptance

These are executable acceptance checks, not a daily-driver certification:

| Area | Evidence / status |
| --- | --- |
| File workspace | `tools/m5_persistent_files_smoke.py --smp 2`: fresh private 4 MiB ext2 image, two boots; create nested dirs, append with piped `ed`, copy/rename/delete, recursive copy, byte comparisons, >12-block file, source saved script, mode/size/mtime preserved across relaunch; RAM `/tmp` not preserved |
| Relaunch contract | Guest `sync`, then shell `exit 0`, then QEMU process shutdown/relaunch on that same disposable image; **not** guest reboot(2), power-loss safety, or a full journal guarantee |
| Memory under SMP | Post-AP buddy/slab worker canaries, allocation/free interleaving; allocator locks preserve local IRQ state, lock order heap -> PMM |
| Programs / compiler | `tools/m5_guest_smoke.py --require-process-selftests --smp 1` and repeated `--smp 2`: seven pipe/signal/orphan regressions, AP SYSRET/NXE/current-AS, guest C exec, provenance, ELF64/flat binary |
| Build | `FCC=/tmp/fakecc-pr116-merge/build/fakecc FAKECC_SRC=/tmp/fakecc-pr116-merge tools/m5_no_host_cc_smoke.sh`: clean deny-tools build and compiler fixed point |
| Inspection | `tools/m5_daily_proc_smoke.py`: real processes/memory via ps/free |
| Scripts | `source` / `. FILE` tested; `/bin/sh FILE` and shebang execution are **not implemented** |
| Desktop/network | GUI, browser, general desktop and network robustness are **not proven** by any file or compiler smoke |

The persistence test accepts **no existing disk path**; each invocation makes a
new `/tmp/fakeos-persistent-*/disposable.img` and leaves both boot logs beside it.
It never uses `build/disk.img`. Every command checks exit status, byte comparisons
must succeed on both boots, and any boot `[FAIL]` is an error.

### Discovered regression

Before the allocator fix, SMP2 piped `printf ... | /bin/ed /home/...` crashed in
`release_pt` reading a bogus frame index (RIP `0xffffffff8011d0d8`, invalid PTE).
The same workflow passed on SMP1. The shared PMM buddy lists and heap slab heads
had no cross-CPU exclusion. Both now use IRQ-saving spinlocks; neither locked
path sleeps. The repeated workflow and the new allocator canary test must pass;
this does not claim all address-space/refcount races have been exhaustively tested.

The longer SMP2 workflow also exposed input corruption (`seq` arrived as `eq`):
UART IRQ and polling consumers shared an unprotected hardware/ring path and
TTY IRQ producers did not share the reader's kmutex. UART reads now serialize;
TTY ingestion and popping share an IRQ-safe spinlock, including an atomic
condition-lock release after sleep is armed under the scheduler lock. Raw input
also uses this path instead of leaking its re-acquired mutex on a sleep retry.
Each boot now verifies 32 immediately successive input/output token commands,
with no retries or timing sleeps to conceal lost/reordered bytes.

### Process lifetime follow-up

Expanded SMP2 ps/free coverage caught a second real fault: `/proc/<pid>/status`
read `VmSize` through a borrowed address-space pointer while the other CPU
exec'd or freed it. Zombies also retained that freed pointer. `/proc` now formats
VMA data under the scheduler lock shared with AS replacement; exit detaches its
TCB's AS before freeing it. The formatters do not allocate or sleep under this
lock. This protects lifetime, not a transactional snapshot of every proc field.

`wait4` could consume its exit record before the child reached DEAD, so reaping
missed the slot permanently. Exit records are now serialized, and consumption
requires successful DEAD-slot reaping under the scheduler lock. Pending teardown
is yielded to (or returns 0 for WNOHANG), not reported as already reaped.
`m5_daily_proc_smoke.py` now races ps against 20 background exec/exits, checks
child maps, and requires no zombies after each wait. It also waits for the whole
UART job notification before parsing the PID (reads may split mid-line).

Review also found a stack handoff hazard: DEAD/FREE publication and stack
freeing preceded the actual RSP swap, and run_lock was dropped before saving
RSP. Another CPU could select/reap/reuse the outgoing context too soon. The
assembly switch now completes ownership and stack reclamation on the **new**
stack under run_lock; creators atomically reserve FREE slots. `wait4` arms its
sleep before releasing the exit-record condition lock (local CLI alone cannot
exclude remote exit producers).

An expanded run failed the old spinner assertion (`spinner 1 never got
scheduled`). Its deadline began before workers had even entered. Workers now
sleep until the last arrival starts the original 30-tick never-yielding window.
The last worker, not idle-only bootstrap slot 0, releases the barrier; all
original progress/completion assertions remain. Failed earlier runs remain
failures, not acceptance evidence.

Foreground launch probes also exposed silent stdout loss: console writes
incorrectly applied SIGTTOU even with default TOSTOP disabled, returning EIO
when that signal was ignored. Background writes now proceed unless TOSTOP is
set and SIGTTOU is not ignored. The daily smoke requires actual output from an
external background printf, not just its exit status.

## Next concrete gaps

1. Safe file copying: inspect/test copying onto the same inode (including hard
   links and aliases) before `O_TRUNC`; current `/bin/cp` lacks that guard. Also
   check read failures and partial writes before reporting successful copying.
2. Script launching: shell currently ignores script argv; only source/. is
   tested. Add non-interactive script execution with meaningful exit status.
3. Metadata UX (`cp -p` / `touch -d`) and pager/named pipes remain missing.
   Historical full in-OS kbuild restage pin is still unresolved.
