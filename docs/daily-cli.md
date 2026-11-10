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
| Ramfs growth | `tools/m5_ramfs_growth_smoke.py --smp 1` and `--smp 2`: real writes beyond 4 MiB, exact 16 MiB, zero extension/full hashes, copy/shrink/regrow, retained data on size-limit and real allocation-pressure failures, recovery after freeing a block |
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

## Safe copy acceptance

A real guest probe reproduced `/bin/cp /tmp/cpself /tmp/cpself` returning 0
while destroying `keep-me` (the resulting file was empty). Copying now opens
both files without truncation, checks **descriptor** device+inode identities,
and only then truncates the open destination. This rejects same paths, `.` /
`..` aliases, hard links, and source/destination symlinks onto the same file.
Ramfs and Ext2 stat/fstat/newfstatat now have distinct device IDs; `stat -c %d`
can inspect them. Ext2 also honors O_CREAT|O_EXCL instead of ignoring it.

`tools/m5_copy_smoke.py --smp 1|2` uses its own new disposable disk: aliases on
both filesystems must return 1 and retain source bytes; ordinary replacement,
-n (including dangling links), cross-filesystem copies, and source/destination
open failures are checked. Two successful 1.5 MiB copies leave too little
space for a third on that 4 MiB test disk: copy must report a write error, return 1, omit the verbose success message,
and allow a later small copy after removing the partial file.

`FCC=... python3 tools/test_cp_io.py` compiles the real copy implementation
with fakecc and injected syscall outcomes: short writes, EINTR, read/stat/
truncate/close errors, failed/zero writes, same inode and cross-device inode
collisions. Resolver/link cases also cover physical descendants, device aliases,
cwd read/change/restore failures, fail-closed lexical symlinks, and conservative
link overwrite/create errors. These checks are not substitutes for guest tests.

Limits: not an atomic copy/rollback facility; failed copies can leave a partial
destination. Concurrent unlink/rename, metadata preservation and power-loss
durability are not certified here.
The first ENOSPC fixture also exposed a separate Ramfs contiguous-allocation
limit: 5 MiB `dd` failed while preparing its source. The fixture now verifies
an exact 1.5 MiB source before filling the disposable disk; larger Ramfs files
were a real daily-use gap before the growth correction below. A separate guest
boundary probe confirmed that exactly 4 MiB succeeds, 5 MiB stops at 4 MiB,
and appending one byte at offset 4 MiB fails without changing retained bytes,
with 191820 kB free. `FILE_MAX` is 16 MiB, but Ramfs doubling calls contiguous
kmalloc whose BIG_ORDER_MAX=10 supports only 4 MiB: this is an architectural
growth/declared-capacity mismatch, not simply exhausted RAM.

### Recursive copy safety

A real guest probe of `cp -r src alias-into-src-subdir/dup` created a growing
`dup/sub/dup/...` tree before failing at a path limit. An ancestor symlink was
also followed repeatedly rather than copied. Directory copies now inspect the
physical destination/parent and compare every ancestor's device+inode against
the source **before mkdir**; this also catches `/mnt` versus `/home` aliases.
Unresolved paths fail closed. `-r` copies symlinks as symlinks, including
ancestor links, dangling links and two-link cycles, without following them.

Expanded copy smoke checks rejection without creating `dup`, retained source
bytes, normal recursive copies and link text on both filesystems, repeated
copies, and refusal to replace existing user data with a different symlink.
The expanded test also exposed `rm` using stat rather than lstat: plain rm
refused a directory link, while recursive rm could walk outside its selected
tree. rm now unlinks links themselves (including dangling/cyclic links),
rejects overlong joined paths rather than truncating them, and propagates
directory read errors. Cleanup tests retain original source/outside-file bytes.

Existing identical symlinks are idempotent; other existing entries for a copied
link are refused (unless -n skips them). This is intentionally conservative,
not full GNU cp -H/-L semantics or adversarial concurrent-rename protection.
Fixed utility bounds remain: 128-byte joined paths and 64 child names per
directory; over-limit joined paths/entry counts fail. Longer filename handling
through the fixed getdents ABI is not certified by this acceptance.

### Group-worker test admission

Strict boot acceptance exposed `sched_tg_attach` failing intermittently.
A deterministic pre-attach yield reproduced the mechanism: both workers were
FREE, done=2, and the original deadline had elapsed before attachment. Workers
now sleep on a setup barrier until their group assignment is complete. Every
group test deliberately yields before attachment and requires zero premature
completions, then releases the original 40/50-tick measured window. All existing
progress, completion, aggregate and ratio assertions are unchanged. Failure
paths release/drain parked workers instead of leaving sleepers behind.

AP MSR acceptance separately exposed a sampling problem: four one-shot
workers all completed on BSP, so the unchanged "must observe an AP" assertion
failed although AP timer, balance and allocator checks passed in that boot.
The BSP now stays IRQ-masked during a bounded remote-work handshake, so the
real workers must execute on APs. All four completion/MSR checks and the AP
observation assertion remain; actual sampled CPU IDs are printed. The original
800-tick deadline also has an 8 s PIT fallback if remote ticks stop advancing.

### Fairness oracle: emulator layout, not scheduler hardening

Earlier uninstrumented boots really failed the original 50-tick/2x nested
aggregate check (including 16184546/5099895). Passing traced reruns did not
resolve them. A failure-only readout subsequently captured 17156908/5442918
with equal parent vruntime 25600/25600, shares 1024/1024, and all workers done
at the original deadline. External host-service measurements on the exact
failed ELF reproduced A/B=2.683 despite near-equal vCPU service (252.96 versus
242.45 ms): the identical B loop had lower throughput per CPU second.

QEMU 10 indexes its direct-mapped software TLB by virtual page number. In
fresh sacrificial VMs, changing **only** the loop's deadline-load address to
a scratch word holding the unchanged deadline isolated the address bias:

| Deadline index | A/B iterations | A/B vCPU service | Original assertion |
| --- | ---: | ---: | --- |
| Matches B's actual stack counter | 2.623 | 1.021 | FAIL |
| Flip one page-index bit; same stack addresses | 1.034 | 1.080 | PASS |
| Matches A1 instead | 0.697 | 1.013 | Slow throughput moves to A1 |

Local counters remained monotonic and equaled the published totals. These
intervened probes are **diagnostics, not acceptance**. The fixture now aligns
its deadline to 16 KiB: existing 16 KiB buddy-backed stacks place counters in
a last page, while the aligned deadline occupies a first page, preventing
that low-page-index collision. The final linked deadline was verified as
`0xffffffff83a3c000` in that earlier image; its tick page had low bits 1,
not stack bits 3.
No scheduler algorithm, hot loop, duration, ratio, progress check, or retry
policy changed. The oldest unobserved failure cannot be retrospectively
measured, and host-service sensitivity still exists.

### Final-image acceptance and UART capture regression

The uninstrumented image SHA256 was
`425995cfa1ab64f043c3d6e0f34d176bdc674371d13efcbbfdb86ad4d9fa6102`.
All twelve selected boot logs had no FAIL/FATAL: copy/removal SMP1/2,
persistence both boots SMP1/2, process inspection/churn SMP1/2, and strict
process + guest compiler SMP1 plus SMP2 three times. SMP2 AP samples were
1/1/1/1. Clean fakecc-only deny build, compiler provenance at released
dfb1c7b, byte-identical self-host fixed point and asm ELF/bin coverage passed.
The 34 real-cp injected scenarios also passed.

The first launcher **did stop** at a host ps-reader failure, not a clean exit:
a job notice split between CR and LF made its second read wait for a fresh
CRLF until the sleeper exited. A lossless forced split reproduced that exact
failure; after correction it consumes the pending LF immediately and passes
unchanged ps/maps/kill/reap and 20-launch churn checks. `test_proc_job_notice.py`
covers every PID/line-ending split and rejects malformed notices. Acceptance
continued with fresh SMP1/2 process runs and all strict compiler runs on the
**same unchanged ELF**; the earlier failed log was retained, not relabeled green.
No full in-OS kbuild or daily-driver readiness is inferred.

### Ramfs growth and buddy reclamation acceptance

A fresh strict guest probe on the preceding validated image verified the full
4 MiB HEAD/zero hash, then failed an actual four-byte append at offset 4 MiB
(`dd: write error`, exit 1). The correction extends buddy orders through 12
(16 MiB) and makes the heap use that same PMM limit instead of a second,
independently capped constant. The contiguous-allocation design is unchanged;
fragmentation or insufficient memory can still reject growth. Review also
reproduced an existing directional buddy-merge bug with the real PMM core:
a private aligned 16 MiB arena split into pages, then freed in ascending
order, reported all 4096 pages free yet could not supply one 16 MiB block.
The correction validates the pair's lower base (either buddy may be freed
last) and marks the absorbed upper head interior. The native fakecc test
covers every leaf order 0..11, ascending/reverse/interleaved frees, reserved
holes, exact accounting and full-block reclamation; its only hardware
adaptations are HHDM backing and single-thread IRQ/lock stubs.

New boot checks hold 8/16 MiB allocations simultaneously, verify every
requested page's boundary canaries and non-overlap, reject an over-limit
request, and require exact page reclamation/reuse. The new
`tools/m5_ramfs_growth_smoke.py --smp 1` (and `--smp 2`) exercises real writes,
zero-extension through 5/8/9/16 MiB, full-byte hashes, retained contents on
size-limit errors, copy, shrink/regrow and deletion. It additionally exhausts
16 MiB blocks in its disposable 256 MiB guest, requires failed truncate and
write growth to retain an existing file's full hash/size, frees one filler,
and requires that same growth to succeed with the prefix retained. This is
allocation-pressure coverage, not a guarantee against arbitrary fragmentation.
The boot bounds test also rejects the maximal unsigned request without wrap.
`FCC=... python3 tools/test_pmm_buddy.py` runs the native PMM cases.
The VFS boot test still requires a failed beyond-limit write, unchanged file
size and correct later O_APPEND; its ceiling now matches FILE_MAX=16 MiB,
not the obsolete 4 MiB heap cap. No fairness windows/ratios were changed.

The final uninstrumented ELF SHA256 is
`d7f5a16ecff878e1000d9b56d7cf714259b27969b885489264c60f969afe73db`.
Its fourteen audited boot logs contain no FAIL/FATAL: growth/copy/ps SMP1/2,
persistence both boots SMP1/2, strict process+compiler SMP1 and SMP2 three
times. All include large canaries, concurrent allocators and the unchanged
nested fairness assertion. Both pressure tests reached 11 live 16 MiB filler
files before failure. Clean fakecc-only deny/fixed-point/provenance and asm
ELF/bin checks passed at released dfb1c7b. Earlier candidates really failed
an obsolete VFS bound and over-cap cleanup argv; they were corrected, not
counted as passing. Contiguous allocation, 16 MiB per-file and resource limits
remain; this is not general daily-driver or full in-OS kbuild acceptance.

## Next concrete gaps

1. Script launching: shell ignores script argv and execve has no shebang
   handling; only source/. is tested. Exec also has an eight-slot argv limit.
2. Long directory-entry names: the fixed 48-byte getdents ABI cannot represent
   all names accepted by the filesystems. Probe prefix collisions and prevent
   silent recursive copy omission before claiming lossless backup workflows.
   Atomic copy replacement/metadata UX also remain incomplete.
3. SMP timekeeping: a controlled SMP2 observation saw `sleep 20` complete in
   about 10 host seconds; global scheduler ticks currently advance on every
   CPU while clock/sleep conversion assumes 100 Hz. Reproduce SMP1/2 timing
   before a separate correction; this increment does not fix clocks.

Pager/named pipes and the historical full in-OS kbuild restage pin remain
unresolved. Desktop/network readiness is not established.
