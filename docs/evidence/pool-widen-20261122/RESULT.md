# P0-10: task pool widened 32 -> 64 slots; P0-9 fork admission limit

Local acceptance, 2026-11-22, QEMU TCG SMP1/2. kernel/sched.c, proc.c,
syscall.c, user/init.c. LOCAL result; the reference-oracle gate stays
Linux-CI-scoped. No commit/push claimed.

## What was implemented

* **Task pool widened: MAX_THREADS 32 -> 64** (kernel/sched.c). The threads
  array, owner table, idle-mark array and every bounded loop scale with the
  single constant; CFS rbtree/pid allocator/exit records are count-agnostic.
  Kernel stacks were ALREADY dynamic (16 KiB buddy blocks per task), so the
  slot widening is the capacity change; per-slot memory cost ~2.1 KB BSS.
* **MAX_EXIT 32 -> 64** (kernel/proc.c): the exit-record table scales with
  the pool so a full cohort of exiting children is reapable.
* **Fork admission limit scales**: `sched_fork_proc_limit()` = MAX_THREADS
  minus 8 kernel-thread headroom (was the hardcoded 24). The rlimit probe
  now hits deterministic EAGAIN at ~54 concurrent children.
* P0-4 audit correction: kernel-task tick preemption ALREADY EXISTS
  (test_preemptive_spinners drives sched_tick's charge+pick+switch through
  kernel threads). Removed from the plan.
* P0-3 step 3 decision recorded (docs/evidence/rl-instrument-20261121/):
  per-CPU runqueue sharding is blocked on the process-model split because
  pick_next must stay mutually exclusive with global state transitions
  (sched_signal_send broadcasts under the same lock the pick needs).

## Deterministic proof

* The lifetime fan (user/init.c, --process-lifetime) now attempts 64
  thread clones: exhaustion reached deterministically at the same count
  across rounds, rollback + reuse verified, `bounded task exhaustion
  round=0 created=61 rollback+reuse OK`.
* The rlimit probe (scripts/test_rlimit.py, SMP1/2): children block on a
  pipe read (proper wait-queue sleep, ZERO scheduler churn); the parent
  forks until deterministic EAGAIN at the scaled limit, wakes the first
  reader with one byte, closes the write end so every blocked reader sees
  EOF, reaps all children with exact status, and verifies slot recovery
  with four new forks. PASS on SMP1 and SMP2.
* Full make test regression: zero [FAIL] across all locally runnable
  stages (native objects, native caps 9/9, net-raw, vDSO, rlimit,
  process lifetime, fixture, ext2 reclaim, vfs mounts, bootstrap transfer
  units). The reference-oracle stage stays Linux-CI-scoped.

## Kernel-behavior findings from this slice

* **nanosleep tick-park reverted**: parking nanosleep callers on a shared
  tick channel (woken per tick by the ISR) caused 50-way wake/park floods
  (5400 context switches/s under load) that starved the serial input path
  completely. Reverted to the busy-yield loop; the proper fix (per-sleeper
  kernel-side deadlines so the tick wakes only expired sleepers) is a
  tracked follow-up.
* **Pipe EOF propagation**: fork-inherited write-end copies keep EOF from
  propagating until EVERY copy closes — the probe children now close the
  inherited write end before reading (the standard pipe discipline).
* A stray QEMU at 100% CPU (left from a debug boot whose script asserted
  before vm.close()) halved TCG throughput and masked itself as test
  flakiness. Scripts now assert the login prompt with retries; stray
  processes must be killed before benchmarking.

## Remaining limitations (deliberate)

* The pool is still a fixed compile-time bound (64) with static TCBs —
  "dynamic allocation" in the architecture sense (guard-paged virtual
  kernel stacks, unbounded tasks) remains the later slice; this widening
  doubles capacity within the existing design.
* The fork limit is a global count (single-user kernel); per-uid limits
  need the credential model (P1).
