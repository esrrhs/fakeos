# Native capability namespace: process-relative handles over native channels

Local acceptance, 2026-11-21, macOS arm64 host, homebrew qemu-system-x86_64,
kernel built by the staged self-hosted fakecc toolchain (seed
/Users/mingming/project/fakecc). LOCAL result only — the Linux-CI-only
reference-oracle gate (prepare_reference_image host linker execution) cannot
run on this host; see docs/evidence/native-wait-cancel-20261118/RESULT.md for
the same deferral reasoning. No commit/push claimed.

## What was implemented (migration gate 2, first bounded slice)

Per docs/native-kernel-architecture.md sections 3-4: the process now owns a
**native capability table**, and user processes exchange copied u64 messages
over native channels through process-relative handles — no global object IDs
are published to user space.

* kernel/natcap.c (new): the capability layer. A user handle is
  `(gen << 16) | (slot + 1)` into the owning process's table. Lookup
  validates (slot, gen) under sched_run_lock and copies out the delegated
  kernel-native handle; native.c re-validates under nc_lock, so a concurrent
  close resolves to EBADF, never to a stale object. Each table entry owns
  exactly one native handle; `cap_dup` allocates an independent native
  handle with attenuated rights (subsets only); `cap_close` closes it and
  revokes blocked operations on it. Generations never wrap back into reuse:
  install, close and process exit all bump, capping at 0xFFFF which
  permanently retires the slot — a stale or owner-slot-reused value can
  never validate again. Syscalls 504-508: nat_channel_create, nat_send,
  nat_recv, nat_dup, nat_close (EBADF/EPERM/EAGAIN/EPIPE/ENFILE/EINTR).
* kernel/sched.c: the table lives in the process owner record (arch section
  3: `process { native_cap_table }`), shared by all tasks of a process by
  identity — NOT keyed by pid integer, because PIDs never wrap/recycle.
  `sched_nat_acquire/release` pins the table under run_lock for bounded
  non-sleeping sections; native.c operations run strictly outside that
  window so the nc_lock -> run_lock order is never inverted.
  `nat_fork_install` runs inside sched_clone_user BEFORE the child is
  published: every live parent capability is re-homed **at the same slot
  with the same handle value** (user code addresses inherited endpoints
  through values copied at fork time) backed by an independent
  native_handle_dup; all-or-nothing with whole-child rollback.
  `nat_process_exit` at the final task exit closes every remaining
  capability in two phases (retire under run_lock, close native handles
  outside it). Capabilities survive exec (process-owned, not image-owned).
* kernel/native.c: `nc_transfer_intr` — an interruptible twin of
  nc_transfer for the user syscalls. The park intent is registered while
  nc_lock is held; `sched_park_commit_unlock_spin` (new, in sched.c)
  publishes sleep and refuses park on pending fatal/caught cancellation in
  ONE run_lock transaction, releasing the condition lock during
  publication. Peer enqueue/close hold nc_lock, signals hold run_lock, so
  every producer either precedes the commit check or observes the published
  sleeper — the same admission contract proven by the wait/cancel slice.
  sched_sleep_unlock_spin_intr (wait4) now delegates to the shared commit.
* Deliberate 4-argument limit on new native.c entry points: the current
  fakecc miscompiles 5-argument calls (a boolean 5th param arrived as
  0xfffffff442d31e80 in R8 during debugging), so the two transfer variants
  duplicate the loop body on purpose. Consistent with the documented
  fakecc miscompilation workarounds (indexed-struct-member pointer forms).
* user/usys.S wrappers, user/kbuild.c manifest +1 module (41 total),
  scripts/bootstrap_transfer.py module-count 40 -> 41, Makefile natcap.o.
* user/init.c `--native-caps`: 9-step probe; scripts/test_native_caps.py
  drives SMP1/2/4 and asserts the boot-time wait/cancel suite stayed green.

## Deterministic proof (SMP1/2/4, sequential boots, fresh disk each)

`python3 scripts/test_native_caps.py` — PASS 9/9 steps on SMP1, SMP2, SMP4:

1. same-process user-thread exchange, 256 copied scalar messages in FIFO
   order through the capability layer
2. rights attenuation: RECV-only dup cannot send (EPERM); widening a send
   cap to RECV refused; dup close leaves the value stale (EBADF)
3. drain-then-EOF after the last sender closes
4. forged handles: wrong slot, retired generation, absurd generation,
   handle 0 — all EBADF
5. cross-process echo through fork inheritance; a post-fork parent channel
   value is foreign in the child (EBADF/EPERM, never accepted); parent
   closes its own receive side so the child is the sole reader
6. two children exchanging concurrently on four channels with single-
   reader/single-writer endpoint discipline
7. closing a capability revokes a blocked receive on it (NC_BAD_HANDLE)
8. caught nonrestartable SIGTERM interrupts a blocked receive in a separate
   process with EINTR, handler runs exactly once — the park-admission
   contract applied to channel parks (pre-park and while-parked delivery
   both end EINTR, never a stranded sleeper)
9. a child exiting without touching inherited caps leaves the parent's
   endpoints fully usable

Each SMP run also re-proved `[WAIT] PASS interruptible admission cases=18
failures=0` before the shell stage (asserted by the harness).

## Bugs found and fixed during this slice (kept in the final code)

* nat_install had no live-slot check: the second endpoint of a channel
  overwrote the first table entry (both handles reported slot 1). Install
  now requires rights==0 (not-live) besides not-retired.
* fork-install Phase A collected freed slots (bumped gen, rights==0) as
  live and called native_handle_dup(0, ...) which always failed. It now
  uses the same live definition as install.
* fork-install originally installed duplicates into the child's own free
  slots with fresh generations — inherited handle VALUES did not resolve
  in the child. Re-homing at the same slot with the same generation value
  is required because fork copies user memory verbatim.

## Local make test coverage and CI deferral

* test_native_objects (kernel-only core) SMP1/2/4 PASS — the native core
  semantics are unchanged for kernel callers.
* test_native_caps SMP1/2/4 PASS (this slice).
* Remaining local suite stages (process lifetime, fixture, ext2 reclaim,
  vfs mounts, bootstrap transfer units) run in `make test`; results in
  build/native-caps/ and the suite log. The test_qemu.py full gate remains
  Linux-CI-only on this host (host cannot execute the x86-64 linker ELF
  oracle); no guest regression was observed in any local boot (zero
  [FAIL] markers across all runs).

## Remaining limitations (deliberate, documented)

* Messages are single copied u64 scalars (bounded byte message = 8 bytes);
  no capability-in-message transfer yet — endpoints reach children only
  through fork inheritance. Architecture section 4 transfer semantics
  (MOVE/two-phase commit) and section 5 wait tokens/timeouts are later
  slices; robust mutex is gate 3.
* Foreign-handle guessing surface is comparable to POSIX fd numbers
  (single-user system); the generation discipline removes stale-value
  reuse but is not claimed as a secure cross-user capability ABI.
* NAT_CAPS=8 per process; native.c pools (8 objects/32 handles) bound
  globally; ENFILE on exhaustion with full rollback. Retired slots slowly
  reduce per-owner-slot capacity after heavy reuse (thousands of cycles
  per slot) — bounded-slice tradeoff, documented.
* Blocking channel ops use the restartable EINTR policy; poll/select
  keep their nonrestartable policy via the same commit primitive.
