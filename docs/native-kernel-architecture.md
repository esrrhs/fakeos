# Native multicore execution and objects — decision record

Status: architectural direction + first **kernel-private** channel/object core;
not a completed process/thread redesign or a released native userspace ABI.
2026-11-17, Asia/Shanghai. Baseline: `9ac709fca90554b7ea4e024c5bde079505a38518`
with previously accepted, uncommitted filesystem fixes preserved.

## 1. Direction and evidence

The unit of **isolation/resource ownership is a process**; the unit of CPU
scheduling is a **task**. Threads in one process share its address space and
capability namespace. Processes communicate using explicit channel endpoints,
not descriptor-shaped pipe/socket wrappers. POSIX descriptors remain a separate
compatibility namespace; compatibility need not dictate native kernel objects.

This is not an argument that Linux mutexes require files: Linux futexes already
provide non-file synchronization, and goroutines are not OS threads. The goal
is a coherent native object and task model with convenient language bindings.

Baseline implementation before the slices below, and the gaps motivating them:

| Current source / mechanism | Keep / change |
| --- | --- |
| `kernel/sched.c: struct tcb`: `pid`, `tid`, `as_h`, signals, credentials, `fd[16]`, cwd, environment all embedded | Separate process resources from the execution context; do not duplicate a new native handle table in each TCB. |
| `sched_clone_thread`: shared `pid`/AS but copied and retained fd entries, copied cwd/credentials | Existing threads do not have genuinely process-shared descriptor namespace semantics. Do not use this as a model for native handles. |
| `MAX_THREADS=32`, order-2 16-KiB contiguous kernel stacks | Fixed bounded prototype, not goroutine-scale; avoid claiming otherwise. TCB metadata includes two 32-element u64 signal arrays as well as saved state. |
| `cfs_root[cpu]`, `rq_steal`, load-aware enqueue, idle-CPU IPI wakeups | Real SMP foundations exist; evolve them instead of replacing working context switch and interrupt entry. |
| `sched_run_lock` protects all runqueues and handoff | Per-CPU queues are **not independent concurrent queue locks**. Global serialization is an explicit initial limitation. |
| `schedule_tail` / context-switch handoff, RETIRED vs DEAD slots | Retain deferred stack reclamation: no other CPU may reap a stack still executing on its old CPU. |
| `sched_sleep_unlock_spin` | Usable atomic condition-lock -> run-lock -> sleep handoff; preferred foundation for native wait objects. |
| `sched_sleep`, `sys_futex` in `kernel/sysproc.c` | Current futex checks the value then separately sleeps; disabling local IRQs cannot close a cross-CPU lost wakeup. It keys by VA, ignores timeouts and cannot identify unrelated ASs or shared mappings at different VAs correctly. Leave compatibility present but do not copy its design. |
| `kmutex_lock/unlock`, `ksem_*`, `wait_queue_sleep_unlock_spin` | Existing kernel-private blocking primitives, not capability-protected user mutexes. `kmutex_unlock` does not validate its recorded owner. |
| `mem/as.c`: address spaces, COW, `as_retain_h` / `as_release_h` | Keep isolation and reference counting, add explicit process ownership and VM lifecycle/locking audit. Frame-refcount mutations are now serialised by `ref_lock` (fakecc drops `lock cmpxchg`, so no CAS primitive), and VM mutations shoot down remote TLBs via the vector-35 per-CPU mailbox path (see docs/evidence/vm-shootdown-20261121/). Do not use recyclable AS pointer as process capability identity. |
| `kernel/uproc.c: sys_execve`, `kernel/syscall.c: proc_terminate`, `kernel/proc.c`: exit records/wait4 | Leader exit currently publishes process exit/reparents even with peers alive; secondary exits skip process notification. Refcounting AS is not a process lifecycle. Exec/group-exit and shared ownership must be specified together before exposing process-scoped native handles. |

## 2. Execution choice: 1:1 first, optional M:N above it

**Choose preemptible kernel tasks, 1:1 with native OS threads**, sharing one
process object where requested. Each task is schedulable on at most one CPU.
Processes need not have dedicated scheduler entities: tasks carry a process
reference, and process/group CPU budgets prevent many-thread unfairness.
A new task must not be constrained to its creator CPU; idle CPUs must receive
remote reschedule IPIs after publication. Sleeping is not spinning.

Why not put an entire Go-like M:N runtime in the kernel now? It introduces
separate worker/task identities, stack growth/relocation and TLS semantics,
blocking-syscall worker replacement, safe asynchronous preemption and a language
stack-map/GC contract. Growable user stacks cannot simply be copied while raw
pointers into them exist. None follows automatically from naming a thread
lightweight. Keep language-specific user M:N runtimes optional: an async
completion/wait API and CPU-budget feedback should permit them later without
changing process isolation. A runtime parks only its logical task for local
work; a blocking kernel call otherwise parks its OS worker, not all workers.

Cost boundary: current kernel task stack alone reserves 16 KiB, excluding its
TCB, user stack, page tables and process resources. First reduce duplicated
resource metadata and make limits observable. Later use virtual, guard-paged
kernel stacks with bounded physical commitment after auditing interrupt depth;
do not shrink stacks by guesswork or promise millions of tasks. No throughput,
latency or memory scalability numbers are claimed by the first slice.

Scheduling invariants: exactly one CPU owner; runnable queue membership matches
state; migration only after the running-context handoff; consistent lock order
for two runqueues (CPU index order); remote publication precedes IPI; no sleeping
under a spinlock except the explicit atomic-release transition. Keep CFS-lite
weights/vruntime initially, including migration normalization. Wire process ownership into the existing `task_group`/`tg` budget boundary
before treating thousands of tasks as independent competitors; define fork
budget inheritance rather than accidentally creating an unlimited fresh budget.

Do not shard `sched_run_lock` in the same patch as process lifecycle changes.
First instrument hold time and runnable-to-running delay; then split local queue
locks plus separate lifecycle locking with explicit handoff references. Existing
idle utilization/steal/IPI tests remain, with sustained imbalance and runnable
latency tests added; no claim that sharding alone guarantees fairness.

## 3. Target owned resources and typed objects

Proposed object graph (names are design sketches, not existing public structs):

```
process { stable_id+generation, refs, state, address_space,
          credentials, native_cap_table, compatibility_fd_table, task_set,
          resource_budget, process_exit_completion }
task    { stable_id+generation, process_ref, saved_context, kernel_stack,
          user_TLS, scheduling_entity, active_wait, cancellation_state }
cap     { slot_generation, object_ref, object_type, rights, inherit_policy }
```

The process owns a locked table shared by all its tasks. A user handle is a
process-relative slot+generation, never an object address, fd, global ID or
security token accepted from another process. Type/rights checks and temporary
references are taken atomically at lookup. Non-reused generations prevent ABA;
exhausted generations retire slots. Bounds apply per process and globally.

Channel endpoint rights: SEND, RECEIVE, DUPLICATE, TRANSFER, WAIT; mutex rights:
LOCK, UNLOCK, WAIT, DUPLICATE, TRANSFER; task/process handles separately control
JOIN, CANCEL, INSPECT. Rights only attenuate. A bootstrap process builder receives
an explicit list of inherited capabilities; fork's compatibility adapter clones
only handles marked inheritable with references under the process-table lock.
Spawn failures roll everything back before publishing the child runnable.
No ambient global object lookup by guessed integer. The parent can create a
channel and explicitly give one endpoint to each child without shared pointers.

Transfer: send a copied bounded byte message plus a bounded list of rights-
attenuated capabilities. Initially DUP/COPY only. MOVE requires two-phase
reservation and all-or-nothing commit: sender invalidation and receiver install
must not partially happen on queue-full, exit, cancellation, or allocation
failure. A queued capability owns a reference charged to a budget until delivered
or discarded. No arbitrary cross-process pointers. Optional shared-memory
objects later require explicit mapping permissions, pin/unmap ownership and
acknowledged lifetimes; zero-copy is not a default guarantee.

Proposed native calls (sketches; no syscall numbers allocated):

```
process_spawn(image, argv, explicit_caps[], limits) -> process_handle
process_exit(status)                         // reject spawn, cancel group, drain
process_exec(image, argv) -> BUSY if multithreaded in the first native ABI
task_spawn(entry_in_current_AS, stack_desc, arg) -> task_handle
task_exit(result) / task_join(task_handle, deadline, cancel) -> result
channel_create(message_limit, queue_limit) -> send_cap, recv_cap
channel_send(send_cap, copied_bytes, caps[], deadline, cancel) -> status
channel_recv(recv_cap, output_buffers, deadline, cancel) -> message, caps[] | EOF
mutex_create(robust=true) -> mutex_cap
mutex_lock(mutex_cap, deadline, cancel) -> OK | OWNER_DIED
mutex_consistent(mutex_cap) / mutex_unlock(mutex_cap)
cap_dup(cap, attenuated_rights) / cap_close(cap)
wait_any(operations[], absolute_deadline, cancellation_cap) -> winning_index
```

The group leader is not the process resource owner: `task_exit` of that task
must not destroy resources while peers run. Last-task exit finishes process
teardown; explicit process exit first transitions LIVE -> EXITING under the
process lock so concurrent spawn cannot resurrect it. Join results live in
completion metadata after stack reclamation (multiple authorized observers may
wait); holding a task handle must not retain its kernel stack forever. First
native exec rejects multiple live tasks instead of silently destroying their AS.
Compatibility exec needs a later, explicitly synchronized stop-and-replace
adapter; no current working POSIX path is destructively removed in this slice.

## 4. Channel semantics

Bounded FIFO messages with explicit endpoint rights, copied payload and maximum
message/queue byte budgets. FIFO is enqueue order, not an unspecified fairness
promise among concurrent senders. Successful send means accepted into the queue,
not that the receiver has processed it. Full -> WOULD_BLOCK or registered sleep;
empty -> WOULD_BLOCK or registered sleep. Nonblocking operations are mandatory.
A rendezvous (capacity-zero) channel is deferred until pair-match cancellation
and commit semantics are proved; it is not silently emulated as capacity one.

Close drops one capability reference. Last SEND close lets receivers drain the
queue, then returns EOF. Last RECEIVE close makes send fail PEER_CLOSED (no
implicit SIGPIPE); outstanding messages/attached capabilities are discarded when
reclaimed. Duplicated endpoints delay the corresponding final close. Closing a
handle concurrently with a blocked operation revokes that operation; it wakes
and returns BAD_HANDLE/CANCELLED rather than secretly keeping peer liveness.
An operation reference still pins the storage until deregistration completes.

Queue publication is release, delivery acquire; in the initial implementation
spin unlock/lock supplies this ordering. This synchronizes copied messages, not
unsynchronized shared user memory. Faulting user-copy must not happen while an
IRQ-off object lock is held: validate/copy into reserved kernel buffers first,
and commit receive removal only after successful copyout or use an explicit
peek/reserve transaction with documented ordering and rollback.

## 5. One wait contract: timeout, cancellation, select

Target wait record: `{ task_ref, object_refs[], generation, registrations[],
state=PENDING|COMMITTED|CANCELLED|TIMED_OUT, result }`. Exactly one transition
from PENDING wins under agreed lock ordering. Condition test + registration +
release must be atomic w.r.t. producers; waking is only a hint, predicates are
rechecked. A running task is not put on two CPUs when the wake races sleep.

Timeouts use monotonic **absolute** deadlines, with explicit infinite/nonblocking
modes and overflow validation. Already-ready work wins if it committed before
cancellation; otherwise no side effect on a timed-out/cancelled operation.
Signal adapters map results without consuming a message twice on restart.
Cancellation is cooperative at safe boundaries; never free a CPU's live stack
or abandon a held internal spinlock. Exit cancels and deregisters all waits,
wait references drain, then the task/process is reaped after CPU handoff.

`wait_any`/select uses one shared completion token over multiple registrations,
locks objects in stable identity order for atomic recheck/arm, commits at most
one arm, and removes losing registrations before freeing the token. Fairness
requires rotating/randomized scan starts, plus measured starvation tests.
Do not implement select as polling or independent blocking syscalls. These
operations are **not implemented by the first channel slice**; a nonzero timeout
must later be implemented or explicitly rejected, never silently ignored.

## 6. Native robust mutex contract

Mutex is a typed kernel object, independent of shared-memory location. Owner is
a stable task identity+generation, not PID or a recycled TCB index. Nonrecursive
lock initially; self-lock -> DEADLOCK, nonowner unlock -> NOT_OWNER. Waiters
register via the common wait contract, with defined cancellation of a pending
handoff. Use a wait queue that prevents unbounded newcomer barging. Native
mutexes permit direct cross-process ownership only after explicit capability
transfer; protecting shared user data also requires an explicit shared mapping.

Owner death produces OWNER_DIED on next acquisition; the protected data is not
magically repaired. New owner must call consistent/repaired before normal
unlock; abandoning recovery poisons the mutex as NOT_RECOVERABLE. Last-cap close
while locked does not destroy the object: an owner reference survives until
unlock/death, then wakes waiters and drains references. Group exit walks owned
mutexes only outside scheduler spinlocks. Priority inheritance is a later,
separate decision; initial API documents no PI, tests bounded basic scheduling
without pretending that prevents real-time priority inversion.

## 7. Implemented first slice and deliberately closed boundary

`kernel/native.c` now supplies a separate native channel/object core: no VFS,
pipe, socket, path or fd routing. Fixed pools: 8 channel objects, 32 handles,
8 u64 messages/channel. Handle records carry rights and 56-bit non-wrapping
generations. Duplicates are independent records; creation rolls back partial
allocation. A blocked operation pins storage; close revokes the original handle,
wakes both directions and reclaims only after endpoints and pins drain.
The object-lock -> scheduler-lock handoff uses `sched_sleep_unlock_spin`.

These IDs are **kernel-private trusted references, not secure process-relative
capabilities**. Since this first slice, the process-relative capability
namespace (gate 2, kernel/natcap.c, syscalls 504-508) and the native robust
mutex (gate 3, section 6; syscalls 513-517) have been implemented ON TOP of
this core — see docs/evidence/native-caps-20261121/RESULT.md and
docs/evidence/native-mutex-20261122/RESULT.md. Still unimplemented: deadlines,
select, endpoint transfer, per-process cleanup beyond the exit walk,
goroutine-scale tasks. The single pool spinlock is bounded but serializes
independent channels; split registry/pin and per-object locks only after the
model is tested.
Run `python3 scripts/test_native_objects.py` (also included by `make test`);
serial evidence is retained per SMP count in `build/native-object-tests/`.
The test runs one QEMU at a time with `-nic none` and a fresh disposable disk.

Kernel callers must close owned handles before normal exit: no implicit kernel
thread-death cleanup is provided for CHANNEL handles. The robust-mutex exit
walk (owner death -> OWNER_DIED) IS wired for user tasks (proc_exit_task) and
kernel threads (kthread_exit), outside scheduler spinlocks. The user-side
asymmetry is deliberate and implemented: the process-relative capability table
is closed by the final task exit (kernel/natcap.c), which kernel threads do not
have — they own no table.

## 8. Migration and acceptance gates

1. Kernel-private object core: pure fakecc compile + complete guest self-bootstrap
   must include the new module; exact object/image equality remains required.
   SMP 1/2/4 sequential, NIC disabled: FIFO/backpressure/EOF, stale handles and
   wrong rights, generation reuse, allocation rollback/exhaustion, blocked close,
   operation-pin lifetime, 2 producers/2 consumers exact-once/no-leak. Print actual
   observed CPU mask; SMP2/4 require at least two execution CPUs, not a latency or
   sustained all-core utilization claim. No host-only mocks substitute for these.
2. Process object + capability namespace: convert clone sharing and resource
   teardown in one bounded series; preserve descriptors with a compatibility
   table. Test child setup rollback, parent/last-thread death, exec/group exit,
   races with lookup/close, descriptor compatibility and slot/AS reuse. Add real
   concurrent user-thread AND separate-process channel exchange on SMP1/2/4,
   isolation (forged/foreign handles), rights attenuation/inheritance and copied
   payloads. This gate is still pending, not met by kernel worker tests.
3. Common cancellation/deadline wait token, then robust mutex and select:
   deterministic wake-before-park/close-after-arm, timeout-vs-commit, kill while
   blocked/owning, owner-dead/poison recovery, multiwait cancellation and retained
   message references. Counters return to pretest baseline after repeated cycles.
4. Scheduler/stack scaling, after correctness: independent queues, fair process
   budgets, bounded stack use and observable resource failures. Benchmarks record
   task/stack/queue memory and p50/p99 wake latency on stated hardware/QEMU mode,
   CPU count, load and version; no fabricated scale claims.
5. Language/runtime layer: convenient spawn/join/mutex/channel syntax and optional
   M:N user task scheduler. Only then evaluate default runtime semantics. Existing
   filesystem, ELF, pthread and self-hosting paths remain regression gates.

Open consequential decisions: initial native userspace payload cap and accounting
policy; default inherit vs explicit-spawn-only policy (recommend explicit spawn);
robust mutex mandatory vs optional mode (recommend mandatory native mode);
process CPU budget granularity and optional runtime worker-count feedback. The
above conservative defaults allow kernel-core work without inventing user ABI.
Graphics and expanded filesystem design are paused; prior accepted FS patches
are untouched. The previously safety-blocked network diagnostic is not resumed.

## Acceptance checkpoint after the initial implementation

The first full gate matched all 40 guest objects and the exact linked/dumped
21,709,728-byte image, but FAILED reconstructed-kernel login/shell acceptance.
It was not an accepted complete self-bootstrap. Investigation reproduced signal
corruption: embedded fakecc indexed-struct-member compound updates store the
array index. Cached stock and staged scheduler machine code differed. The same
construct affects pipe/file reference accounting. These are actual executable
compiler reproductions, not evidence that the proposed process model is done.

The current causal repair uses pointer-based updates for 12 scheduler bitset
operations and 19 pipe/file reference/link/offset operations, preserving intended
semantics and prior FS fixes. Indexed self-assignment is not a trusted substitute.
No shared compiler source was changed. The process-object refactor is deliberately
not started until the full gate passes; copied fd, leader lifetime and AS refs
still require focused guest reproductions before changing their ownership model.
See [durable evidence and active gate](evidence/native-process-foundation-20261117/CHECKPOINT.md).


## Process-owner slice decision — 2026-11-17 (implementation gate in progress)

Evidence before modification: `evidence/native-process-owner-20261117/baseline/`.
The deterministic raw-syscall probe prefaults shared coordination/stack pages,
uses atomic handoff and `/proc` state (not timing sleeps), and holds a worker
until its parent actually blocks in wait or incorrectly returns. SMP1/2/4
reproduce leader-only completion with code17 while its worker is alive;
raw exit_group231 also returns42 while its peer survives and executes to completion.
Thus the existing comments promising last-thread teardown do not describe reality.
The current exec path destroys the old AS unconditionally; AS sharer count updates
are plain operations. These are source findings, NOT a reproduced AS UAF claim.

Chosen bounded native contract:
* Process owns ONE address-space reference and a stable nonwrapping PID. Tasks
  borrow that AS while attached. Fixed process slots remain until exactly one
  parent consumes completion; no user pointers/IDs become capabilities.
* `task_exit` (compat raw nr60) removes only the caller. Last attached task claims
  teardown once; group leader has no privileged lifetime role. Completion status
  is the last task's code, unless explicit group exit already fixed the code.
* `process_exit` (compat nr231 and fatal process signals) first marks EXITING,
  refuses new task admission, requests peer cancellation through existing signal/
  wake/IPI paths, then drains. This is cooperative at existing kernel safe points,
  not a promise to kill uninterruptible kernel operations instantly.
* Detached tasks have already closed their copied descriptors, cleared child-TID,
  switched off their AS, and cleared their borrowed AS field UNDER run_lock before
  the last owner releases it OUTSIDE run_lock (file-backed AS teardown may sleep).
  A separate resident-task reference remains until the assembly new-stack handoff;
  parent wait cannot reap the process while any such context/stack is still live.
* Lock order remains exit-condition lock -> sched_run_lock. Never publish an exit
  record while holding run_lock. One claimed finalizer publishes one record; its
  authoritative parent is resolved under the exit lock, avoiding stale reparent
  snapshots. Process slots cannot be reused before all task references drain.
* First fork/exec adapter policy is explicit BUSY unless exactly one attached
  AND resident context remains. An image-transition guard blocks new admission;
  current-ABI remote signals remain pending until the sole caller reaches a safe
  delivery point; fork inherits only the calling task into a new process/AS. Invalid/failing
  image preparation rolls back; successful exec swaps AS ownership before old AS
  release. Full multithreaded stop-and-exec and concurrent COW/TLB-shootdown support
  are later work, not silently assumed Linux semantics.
* Descriptor namespace/cwd/credential/signal metadata remains the existing copied
  TCB adapter in this slice. Sharing fd arrays WITHOUT lookup/close backend pins
  would introduce a separate UAF race, so it is deliberately not done here.

Acceptance: deterministic leader/peer and wait ordering, distinct task/group exit,
failed/successful fork/exec restrictions, repeated create/reap beyond32, bounded
allocation failure rollback, actual user-task teardown on SMP1/2/4 after AP release,
existing channel/FS tests and actual guest40-object self-bootstrap/link/dump/31-check
second boot. Report actual CPU evidence and failures rather than scale claims.
The32-task bound,16KiB kernel stacks and global scheduler lock remain unchanged.
This is lifetime ownership, not a complete SMP VM mutation/TLB or goroutine system.

Status policy is deliberate but independent of resource ownership: raw60's
**last AS-membership detacher**, not the last CPU handoff, provides default status
(leader17/worker29 ->29). First explicit group exit fixes its status (42). Without
explicit group exit, differing task results are schedule-dependent. Keeping a
designated main task's result while waiting for peers is a valid alternative;
recommend explicit `process_exit(status)` for deterministic whole-program native
status, and settle main-return/task-join result policy before releasing an ABI.
Existing CRT main-return uses raw60, so this is not a blanket POSIX-libc claim.
See [completion/locking review and remaining ABI blockers](evidence/native-process-owner-20261117/SEMANTICS-REVIEW.md).

Current owner-slice map superseding the baseline lifecycle rows: one process owns
one AS reference; task `live` membership and off-stack `resident` references
separately guard destruction/reap; leader TCB identity no longer owns completion;
raw60 exits one task, raw231 requests group drain; image transitions require a
single attached/resident task. FD/credential/cwd/signal adapters still live in TCBs.
A further [fatal-cancellation-before-park admission gap](evidence/native-process-owner-20261117/KNOWN-WAIT-ADMISSION-GAP.md)
was found by source review, not a deterministic executed repro. Waking an already
sleeping peer is not proof against a signal arriving just before sleep publication.
That gap is since closed and gated; see [wait-admission gate status](#wait-admission-gate-status--2026-11-22).
The VM/resource-namespace blockers remain before further native user capability
exposure, even if the currently covered lifecycle acceptance passes.

## Wait-admission gate status — 2026-11-22

The 2026-11-17 blocker above was closed by the interruptible park-admission
contract (`docs/evidence/native-wait-cancel-20261118/`): `sched_park_prepare`
registers the intent, and `sched_park_commit` decides park-versus-refuse in the
SAME run_lock transaction that publishes `TCB_SLEEPING`, so a fatal or caught
nonrestartable signal that arrived while the target was still on-CPU refuses the
park instead of stranding it. Call sites carrying that contract: futex
(kernel/sysproc.c), wait4 (`sched_sleep_unlock_spin_intr`, kernel/proc.c),
select/poll (kernel/sysfile.c), epoll (kernel/epoll.c), and the native channel
and mutex waits (kernel/native.c). Kernel-internal sleeps — native core, net
worker, kmutex/ksem/waitqueue, probe rendezvous gate — stay uninterruptible by
decision, because their cleanup obligations differ from a user syscall rewind.

What is NOT claimed: the proof is the 18-case boot suite (9 injection modes x 2
rounds, `rescues` must be zero) at the exact post-predicate/pre-publication
boundary, on SMP1/2/4. It is an injected barrier over enumerated interleavings,
not coverage of every possible one.

One gap this closes is procedural rather than semantic: that suite had been run
by hand since 2026-11-18 but was never listed in `make test`, so no CI run had
ever executed it. It is now stage 8 of the gate, with the same 300-second
boot-complete barrier as its siblings and a default evidence directory, so a
healthy-but-slow guest is not read as a failure.
