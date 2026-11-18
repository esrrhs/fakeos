# Interruptible park admission remains a native-ABI blocker

Source review during the immutable full gate, 2026-11-17 23:50 Asia/Shanghai.
This is a **source-level interleaving**, NOT a claimed executed UAF/deadlock repro.

`sched_signal_send` holds run_lock, sets pending SIGKILL, wakes tasks already in
SLEEPING/STOPPED, and IPIs remotely running contexts. `sched_sleep`,
`sched_sleep_cli`, and `sched_sleep_unlock_spin` subsequently acquire run_lock
but do not reject park when fatal cancellation is already pending. Example:

1. A user task enters `sys_futex(WAIT)`; its value matches.
2. Its peer enters group termination, marks owner EXITING, sets this task's
   SIGKILL and attempts a wake while this task is still RUNNABLE/on-CPU.
3. Interrupted kernel code resumes and calls sched_sleep, publishing SLEEPING
   after the wake. Ring0 interrupt/scheduler safe-point rules need not deliver
   the pending fatal signal before that publication.
4. With no future futex producer, it can remain asleep, pinning live/resident/AS
   ownership and delaying process completion indefinitely.

The normal futex value-check/park lost-wakeup race is already documented in the
architecture baseline. Fatal cancellation-before-park is a related but distinct
problem: correct object condition-lock handoff alone does not serialize signal
admission unless pending cancellation is checked in the same run_lock transaction.
Wait4/other interruptible wrappers must be audited for the same admission issue;
do not claim that waking an *already* sleeping task proves this race absent.

Current deterministic raw-syscall lifecycle probes hold peers runnable via
atomic gates/yields; actual teardown occurs across multiple CPUs. They prove the
reported task/group results and owner/AS lifetime invariants for those schedules,
not every sleeping-peer interleaving. Generic AS lifetime remains protected by
live/resident references even if termination makes no progress.

Recommendation for the next bounded correctness slice, before native user IPC:
introduce an explicitly interruptible park-admission operation checking fatal/
cancellation state under run_lock, preserving condition-lock -> run_lock order;
return an interruption result and make each caller unwind correctly. Do not
blindly skip all kernel sleeps: AS/file cleanup and uninterruptible kernel mutex
loops have different progress/cleanup obligations. Add deterministic pre-park
signal injection/barrier tests (plus already-sleeping and remote-current peers),
then full SMP1/2/4 and bootstrap acceptance. No such implementation is started in
this gate, as requested. No user-native capability API is exposed meanwhile.
