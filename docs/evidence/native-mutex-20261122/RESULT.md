# Native robust mutex (architecture section 6, migration gate 3)

Local acceptance, 2026-11-22, macOS arm64 host, homebrew qemu-system-x86_64
(TCG), kernel built by the staged self-hosted fakecc toolchain. LOCAL result
only — the reference-oracle gate stays Linux-CI-scoped (see
docs/evidence/native-wait-cancel-20261118/RESULT.md). No commit/push claimed.

## What was implemented

The mutex contract from docs/native-kernel-architecture.md section 6, as a
typed kernel object in the existing native core (kernel/native.c), exposed
through the process-relative capability namespace (kernel/natcap.c):

* **Typed object, not a shared-memory word**: `NC_MUTEX` joins channels in
  the `native_object` pool (8 objects, 32 handles, one `nc_lock`, pin/ref
  lifecycle). Capabilities carry LOCK/UNLOCK/WAIT rights (attenuation via
  `nat_dup`; the CREATE cap holds all three). No futex word, no shared
  mapping required — the owner identity lives in the kernel.
* **Stable owner identity**: `sched_current_owner_key()` (sched.c) returns
  `(pid << 32 | tid)` for user tasks — tid comes from the never-recycling
  pid allocator, so a dead owner can never be impersonated by TCB-slot
  reuse. Kernel threads (pid 0) use their TCB slot and are trusted test
  callers.
* **Nonrecursive + ownership-checked**: self-lock -> EDEADLK (-35),
  nonowner unlock -> EPERM (-1).
* **Bounded FIFO queue, no barging**: unlock with queued waiters transfers
  ownership to the FIFO head AT UNLOCK TIME (state stays LOCKED under the
  new owner), so a newcomer that arrives before the handed-off waiter
  resumes sees the lock held and queues behind it — trylock during an
  in-flight handoff deterministically returns EAGAIN. Queue bound MQ_CAP=8;
  overflow -> EAGAIN (bounded backpressure, no unbounded waiter pileup).
* **Wait admission**: blocking lock follows the exact
  `sched_park_prepare`/`sched_park_commit_unlock_spin` contract the channel
  paths use (nc_lock -> run_lock, never inverted); fatal cancellation
  refuses the park with EINTR.
* **Robustness**: a dying task's exit walk (`native_mutex_owner_died_key`,
  called from `proc_exit_task` for user threads and `kthread_exit` for
  kernel threads, OUTSIDE scheduler spinlocks) marks owned mutexes M_DIED
  and wakes waiters. The next acquirer gets EOWNERDIED (-130) and MUST call
  `consistent()` before a normal unlock; unlocking without consistent
  poisons the mutex ENOTRECOVERABLE (-131, terminal).
* **Kill-while-handed**: when cancellation lands after a handoff already
  transferred ownership to a dying waiter, the lock releases CLEANLY
  (M_FREE) — the task never returned to user code, so the protected data is
  provably untouched; no spurious recovery demand.
* **Last-cap close while LOCKED does not destroy the object**: the owner
  reference survives (reclaim blocked while M_LOCKED with no handles) until
  unlock/death, then waiters drain and the object is freed. Mutex caps
  count as object references (readers), so dup/close bookkeeping reuses the
  channel machinery unchanged; fork inheritance needed NO code changes
  (rights copy verbatim, same-slot same-generation install).
* Syscalls 513-517: `nat_mutex_create/lock/unlock/consistent/stat`. Stat
  publishes (state, waiters, owner-pid) for deterministic test barriers.

## fakecc/scheduler constraints hit in this slice (documented, worked around)

* **kmain is the BSP idle TCB**: `pick_next` prefers real work over idle, so
  spin-YIELD stage waiters starve the controller forever (observed as a
  silent SMP1 hang in the selftest stage machine; the MPMC barrier comment
  documents the same constraint). Stage waits now use the park-admission
  contract.
* **sched_sleep has no lost-wakeup protection**: on SMP2 a wakeup landing
  between a waiter's stage check and its `sched_sleep` publication is LOST
  forever. The selftest gates use prepare-under-condition-lock/commit
  (`nm_wait_stage`) so the controller's publish+wake either precedes the
  check, latches park_notify on the registered PREPARING waiter, or wakes
  the published sleeper. (Product code was already admission-correct; this
  was a test-harness bug, recorded here because it is the exact shape the
  wait-admission contract exists to prevent.)
* User-probe determinism needs kernel-published state (`nat_mutex_stat`)
  barriers, not timing: fork-then-unlock without a waiters barrier races
  the child's queue insertion (unlock into an empty queue releases
  instead of handing off).

## Deterministic proof

Kernel selftest (native_object_selftest, all boot modes, SMP1 and SMP2):

* `native mutex: self-lock DEADLK, nonowner EPERM, stat, attenuated dup` —
  basics, stat fields, UNLOCK-only dup can release but not lock, stale
  handle EBADF after close, object-slot reuse.
* `native mutex: FIFO handoff, no barging, bounded queue drains` — three
  contenders queued in a controlled order (stat barriers); unlock hands to
  the head; main's trylocks during the handoff chain return EAGAIN; the
  acquire ORDER is recorded and asserted FIFO; the drained lock is free
  again.
* `native mutex: owner death -> OWNER_DIED + consistent recovery` — an
  owner exits without unlocking; the waiter acquires with EOWNERDIED,
  consistent() restores, unlock succeeds, the mutex is usable again.
* `native mutex: abandoned recovery poisons NOT_RECOVERABLE` — recovery
  acquired, unlocked WITHOUT consistent; every later lock/consistent
  returns ENOTRECOVERABLE.
* `native mutex: close-while-locked survives, drains at death` — the owner
  closes the ONLY capability while holding the lock, then dies; the object
  survives the close, drains at the death walk, and `nc_empty()` confirms
  the whole pool returned to baseline (counters back to pretest).

User probe `/bin/init --native-mutex` (scripts/test_native_mutex.py, 8
steps, SMP1 and SMP2 PASS, `__NM_RC=0`):

1. basics/self-lock/nonowner/stat/stale-handle.
2. rights attenuation: LOCK/UNLOCK/WAIT dups enforced; a channel SEND
   right is refused on a mutex; MWAIT-only dup stats but cannot lock.
3. same-process threads: FIFO handoff, no barging (trylock EAGAIN during
   the chain), drain.
4. cross-process (fork-inherited caps): both children queue (stat
   barrier), handoffs PROVEN by the published owner pid (parent observes
   owner==child1 then owner==child2), children released via pipes.
5. kill-while-blocked: the queue HEAD is SIGKILLed after both waiters are
   queued and reaped; the dead waiter unwound from the queue (waiters back
   to 1); unlock hands off to the survivor which acquires cleanly.
6. owner death across processes: child acquires via handoff, exits owning;
   parent's blocking lock returns EOWNERDIED; consistent + unlock; lock
   works again.
7. abandoned recovery poisons terminally (cross-process).
8. last-cap close while locked: the child acquires, closes its own cap
   while LOCKED, exits; the parent's cap close drains the object; eight
   fresh create/close cycles prove the object pool returned to baseline.

Every boot also asserts the wait/cancel 18-case suite (interruptible
admission) stays failures=0 and no [FAIL] marker anywhere.

## Remaining limitations (deliberate)

* No deadline/timeout parameter on lock yet: the common cancellation/
  deadline wait token (architecture section 4) is a separate slice; a
  nonzero timeout would need that token, so the API takes only block=0/1
  (trylock is block=0 with EAGAIN).
* No capability TRANSFER yet (arch section 4): cross-process sharing works
  through fork inheritance only, as documented for channels.
* No priority inheritance (explicitly deferred by the architecture).
* Wakeups are channel-granular (wake-all on the mutex's wait channel);
  handed-off ownership makes this correct, and the bounded queue keeps the
  herd bounded.
* No PI futex / robust-list shared-memory shape: this is the native typed
  object mode, which the architecture recommends as mandatory-native.
