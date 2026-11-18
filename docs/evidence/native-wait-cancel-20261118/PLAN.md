# Interruptible wait correctness slice (2026-11-18 Shanghai)

Bounded contract: condition -> run_lock order; pending interruption/group-exiting
check and sleep publication are ONE run_lock transaction. Signal send uses that
same lock, so it either precedes the check or observes the published sleeper.
Return interruption to callers; never terminate a task while an object/condition
lock is held. Existing kernel mutex/semaphore/driver cleanup sleeps remain
uninterruptible. Compatibility restartable policy retains caught SA_RESTART
behavior; poll/select use nonrestartable policy. Native kernel object waits share
the publication primitive, not a file wrapper.

For predicates checked outside a condition lock (futex user-word/readiness scans),
a bounded per-TCB prepare/commit registration is preferable to loading user memory
under run_lock: producers can latch notification while the waiter remains on-CPU.
No VM dereference/spin under run_lock, no wake polling or allocation. Commit checks
latched notification and cancellation under run_lock, clears preparation, then
publishes actual sleep atomically. All early returns/exit must clear preparation.
The existing cross-process futex VA-key identity/shared-mapping model is a separate
known limitation; this slice must not claim a secure native user capability ABI.

Deterministic proof uses real isolated /bin/init --park-probe children, armed only
by a trusted boot driver before publication (no user arm syscall). Exact futex
post-value-check/pre-park injection sets SIGKILL or caught SIGTERM. A stranded
pending-signal SLEEPING state is observed under run_lock and rescued explicitly,
counting FAILURE rather than hiding it. Group rendezvous holds a worker in kernel
before park until leader really calls group-exit42 and detaches; only then release
the worker. Legacy code should strand that final AS borrower. Additional cases
cover already-blocked fatal delivery, normal wake with fatal in either order,
caught EINTR vs SA_RESTART, normal wake, repeated process/object-slot reclamation.
Baseline and fixed probes must be the same strengthened tests, SMP1/2/4 sequential.
No prohibited blocking pipe/socket diagnostic, network setup or new user IPC API.
