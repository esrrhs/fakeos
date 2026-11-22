# Fork admission limit (P0-9): deterministic process-count EAGAIN

Local acceptance, 2026-11-21, QEMU TCG SMP1/2. kernel/syscall.c fork path +
kernel/sched.c live-process counter. LOCAL result; the reference-oracle gate
stays Linux-CI-scoped. No commit/push claimed.

## What was implemented

* `sched_live_process_count()`: owner records with a live task, under
  runlock. 24-process admission limit checked in `fork_exclusive` BEFORE
  any allocation: exhaustion fails with clean EAGAIN (-11) instead of the
  old implicit first-resource-outage failure. Distinguishes ENOMEM (-12)
  for AS/clone failures and slot failures, EAGAIN for the process limit.
* P0-3 step 3 decision record (docs/evidence/rl-instrument-20261121/):
  per-CPU runqueue sharding is blocked on the process-model split (P0-10)
  because pick_next must stay mutually exclusive with global state
  transitions (sched_signal_send broadcasts to arbitrary pids under the
  same run_lock the pick needs). Recorded in the arch doc's sharding path;
  the step is deferred behind P0-10, per the documented discipline.
* P0-4 audit correction: kernel-task preemption ALREADY EXISTS —
  test_preemptive_spinners ("Timer preemption: never-yielding threads all
  scheduled") drives sched_tick's charge+pick+switch through kernel
  threads. The review's P0-4 was a misjudgment and is removed from the
  plan.

## Deterministic proof (scripts/test_rlimit.py, SMP1 and SMP2)

* fork until EAGAIN: the limit hits deterministically (children hold the
  process count with a bounded churn); the failing fork returns -11.
* every spawned child reaped with exact status; after the reaps, four new
  forks succeed (slot recovery) — no permanent slot loss.
* runs embedded in make test; zero [FAIL] across the full regression
  (native objects/caps/net-raw/vdso/rlimit/lifetime/fixture/ext2/vfs/
  bootstrap transfer units).

## Known issues carried

* ~1 page per fork+release leaks in the table teardown path (see
  docs/evidence/vm-shootdown-20261121/RESULT.md); the rlimit probe's
  accounting shows the same drift and tolerates it as [WARN].
* The admission limit is a global count (single-user kernel); per-uid
  limits need the credential model (P1).
