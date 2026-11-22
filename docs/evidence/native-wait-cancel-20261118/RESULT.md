# Interruptible park admission: implemented, local acceptance PASS

Local acceptance, 2026-11-21, macOS arm64 host (darwin 27.0.0), homebrew
qemu-system-x86_64, kernel built by the staged self-hosted fakecc toolchain
(hint @ 2c1f4525, seed /Users/mingming/project/fakecc build v0). This is a
LOCAL result, not a claim that the Linux CI reference-oracle gate ran. The
x86-64 Linux ELF oracle (scripts/bootstrap_transfer.py prepare_reference_image,
used by scripts/test_qemu.py:206) cannot execute on this host by design — its
docstring scopes it to Linux CI. No commit/push is claimed.

## What changed (interruptible park admission, Go gopark/sema discipline)

The KNOWN-WAIT-ADMISSION-GAP.md interleaving is closed: a fatal or caught
nonrestartable signal arriving while the target is still RUNNABLE no longer
strands a later park, and predicates checked outside a condition lock (futex
user word, select/poll readiness) no longer lose wakeups that land while the
waiter is on-CPU between registration and commit.

* kernel/sched.c: TCB gains `park_state`/`park_notify`; new
  `sched_park_prepare` / `sched_park_abort` / `sched_park_commit(chan,
  nonrestartable)`. Commit decides park-vs-refuse in ONE run_lock
  transaction; every producer (futex WAKE, readiness wakeups,
  sched_signal_send) holds that same run_lock, so it either precedes the
  commit check or observes the published sleeper. Wake paths
  (`sched_wakeup`, `sched_wakeup_n`, `wake_chan_locked`) latch
  `park_notify` on PARK_PREPARING waiters instead of losing the wakeup (Go
  netpollblock-style latch). New `sched_sleep_unlock_spin_intr` gives wait4
  the same admission under its exit condition lock; refusal releases the
  condition lock with IRQs still off and returns 0. Slot reclamation clears
  the registration. Kernel-internal sleeps (native channel, net worker,
  kmutex/ksem/waitqueue, probe rendezvous gate) remain uninterruptible and
  never register.
* kernel/sysproc.c: FUTEX_WAIT = prepare BEFORE the trusted probe injection
  point, then commit; PARK_INTERRUPTED or post-wake pending signal returns
  EINTR. Restartable policy.
* kernel/proc.c: wait4 parks via `sched_sleep_unlock_spin_intr`; refusal
  returns EINTR without consuming the exit record. Restartable policy.
* kernel/sysfile.c: select/poll infinite blocks use prepare/commit with the
  nonrestartable policy (sched_should_eintr_timer); a readiness producer
  latching during the window yields PARK_WOKEN and a fresh readiness scan.
* kernel/sched.c probe gate: `sched_park_abort` before the trusted
  rendezvous park so the gate is the only claimed channel.

## Deterministic proof (strengthened in-tree suite, unchanged tests)

The 18-case wait-cancel suite (kmain boot selftest, trusted boot driver
injection at the exact post-predicate/pre-publication boundary, real
/bin/init --park-probe children) passed on SMP1, SMP2 and SMP4:

* SMP1: `[WAIT] PASS interruptible admission cases=18 failures=0 CPU-mask=0x1`
* SMP2: `[WAIT] PASS interruptible admission cases=18 failures=0 CPU-mask=0x3`
* SMP4: `[WAIT] PASS interruptible admission cases=18 failures=0 CPU-mask=0x7`

Every case ran twice per SMP level with rescues=0 on all 54 lines. The
CHECKPOINT.md baseline (same corrected fixture, original production waits)
had exactly cases 0/3/6/7 failing twice each, each strand needing one
explicit rescue recorded as FAIL. Those four now pass:

* case 0 pre-park fatal: commit refuses, EINTR, exit 137
* case 3 pre-park caught nonrestartable TERM: refusal, handler runs, exit 0
* case 6 group-exit42 rendezvous: released AS borrower's later futex park is
  refused on the pending SIGKILL, exits, process completes 42 without a
  stranded borrower
* case 7 pre-park normal notification: driver wake at the injection point
  latches the preparing waiter; commit returns PARK_WOKEN, no sleep, exit 0

Full boot-time kernel selftests across the same three boots: zero [FAIL]
markers, 90/91/91 [PASS] lines (SMP1/2/4).

## Local make test coverage and CI deferral

* scripts/test_native_objects.py SMP1/2/4 PASS (MPMC masks 0x1/0x3/0xf,
  teardown, barrier, fork/thread compatibility).
* scripts/test_process_lifetime.py SMP1/2/4 PASS (owner admission/rollback,
  64 completions, fork/exec BUSY, multi-CPU teardown evidence).
* scripts/test_qemu_fixture.py, test_ext2_reclaim.py, test_vfs_mounts.py,
  test_bootstrap_transfer.py (11 unit tests) PASS.
* `make test` stops at test_qemu.py:206 prepare_reference_image on this
  host: `OSError: [Errno 8] Exec format error ... build/user/ldfake.elf`.
  This is the Linux-CI-only host linker oracle, not a guest regression.
  Two harness lines were fixed for Python 3.9 (int.bit_count ->
  bin(x).count("1")) in test_native_objects.py and test_process_lifetime.py;
  both had already PASSED their guest runs before the checker crashed.

Deferred to the Linux CI gate: the full test_qemu.py run (guest self-host
recompile/link of 40 modules against the trusted size/SHA reference, serial
transfer, second-boot acceptance). The wait-cancel suite itself is a boot
selftest and was covered locally on SMP1/2/4 above; the reference-oracle
independence argument is unchanged and belongs to CI.

## Remaining limitations (unchanged, deliberate)

* Futex identity remains the VA-key model (shared-mapping identity is the
  documented separate limitation); no native user capability ABI is exposed.
* Fixed 32 tasks, 16 KiB stacks, global run_lock unchanged.
* nanosleep/timeouts on futex are not wired; timeout paths busy-yield by
  deadline and keep their existing behavior.
* No claim that every interruptible wrapper is audited beyond futex, wait4,
  select and poll — those are the four user-interruptible park sites.
