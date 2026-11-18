# Process-owner candidate: covered full acceptance PASS

Final gate job444792 (outer1349575/gate1349582), **actual EXIT0**, ended
2026-11-18 00:10:19 Asia/Shanghai. `MAKE_TEST_EXIT=0`,
`PROCESS_OWNER_GATE_EXIT=0`, frozen input changes0. No builds/VMs remain active.
This result supersedes historical gate-in-progress checkpoint notes; no further
implementation is started. HEAD remains9ac709f; no commit/push.

## What was actually accepted

* Exact existing CI pin dfb1c7b649480e2acfef2a0a02e8168b5b28ee38, not old PATH
  or a2158f. Independent private-source audit:4657 tracked blobs match;128 embedded
  source hashes match. Archive provenance revision is null, so the explicit blob
  audit, not an invented Git HEAD, establishes source identity. Seed SHA0f214a...,
  staged SHA147b2d... unchanged. Normal build remains pure fakecc; no compiler
  source/workflow-pin/shared-fakecc changes. This is LOCAL acceptance, not a claim
  that GitHub CI ran. See current-toolchain-audit.json and prior exact-pin
  REPRODUCE.md/toolchain.env.
* Native channel SMP1/2/4 passed twice targeted, plus once in the full gate:
  actual512 transfer masks1/3/f. Strict remote placement/AP execution/IPI tests
  remain asserted (full SMP2 remote2/IPI1; SMP4 remote3/IPI3).
* Process SMP1/2/4 all pass: leader raw task-exit17 does not complete the process;
  held final worker29 does. Explicit group42 cancels the peer body and fixes42.
  Parent never returns early. Fork/exec BUSY/rollback/success,64 unique
  completions/second-wait failure, six paired independent-process teardown rounds.
* Bounded task exhaustion recovers identical capacity across two rounds:
  29/28/26 on SMP1/2/4. Each fills30 child zombie owner slots, reaches the actual
  owner-table rejection, reaps/reuses, and proves inherited writer retains from
  failed admission were undone through immediate pipe poll/EOF. Actual task-exit
  CPU masks include3 on SMP2 andf on SMP4. This is not a PMM leak proof.
* Existing Ext2 exact reclaim/reuse/count/survivor/reboot tests, VFS two-boot
  persistence, five fixture and11 transfer unit tests all pass.
* **Actual guest** recompiled40 DISTINCT kernel objects, all40 byte-identical;
  actual guest link,1769-frame full dump,21730208 bytes. Full reconstructed ELF
  and trusted reference BOTH SHA256:
  `6805baaf18e50a52a74506db90b732de46c4baaac16db66023e2363f09ca5744`.
  All31 strengthened reconstructed-kernel second-boot checks pass (default SMP1).
  Host/staged40 parity is separately verified, not substituted for guest proof.

Durable data: ACCEPTANCE.json (object names/hashes/CPU counts),
full-clean-acceptance.log, full-artifacts/ (serial/provenance plus compressed actual
reconstructed ELF), validated-inputs.tar.gz and frozen-input-sha256.json (all218
accepted inputs). The accepted filesystem source was unchanged throughout.

## Minimal owned slice / preserved behavior

* kernel/sched.c: process owner with one AS reference, live vs off-stack resident
  references, nonwrapping locked PIDs, admission/rollback, guarded AS image swap,
  owner-based process observation/reap. Existing global run_lock retained.
* kernel/syscall.c: task60 vs group231 termination, one finalizer/completion,
  guarded fork, no per-thread AS refcount increments/decrements.
* kernel/uproc.c: exclusive exec guard and ordered ownership/CR3 swap/release.
* kernel/proc.c: authoritative parent lookup under exit_lock, preserving
  exit_lock -> run_lock ordering and unique wait consumption.
* kernel/proctest.c + scheduler batch helper: real idle-publication opportunity
  using all-or-none preparation and one normal-policy enqueue transaction;
  production selector/IPI policy and strict assertions are not relaxed.
* user/init.c lifecycle tests, user/usys.S group-exit wrapper,
  scripts/test_process_lifetime.py, one Makefile test entry, architecture/review.
  See the four *-owner.diff files against preserved actual baseline snapshots.
  Other existing dirty FS/docs/native/channel changes are not newly attributed
  to this slice. mem/as.c itself was NOT refactored; no fd-table sharing added.

Old initial gate444789 failed2 (real AP execution but no idle-IPI opportunity).
Target444790 passed0 after bounded batch-publication fixture correction.
Retry444791 failed2 because a strict user PASS marker was split by the concurrent
final kmain banner, despite guest status0. Preserved as full-clean-retry1-failed.log
and full-retry1-failed-artifacts. Fix waits for the real boot-complete idle banner,
not a sleep, and keeps every assertion. Stock kernel SHA is identical across that
fixture correction (b3c1917f...). Only the final clean retry is reported PASS.

## Native semantics and remaining consequential risks

Status is deliberate: absent explicit group exit, the final AS-membership
**detacher** supplies the process status (29), not the last assembly handoff.
First explicit group transition fixes42. Resource isolation does NOT require this
status policy. A designated main result17 with delayed resource drain is a valid
alternative. Existing CRT main-return still uses raw60 and can therefore lose its
result to a later worker; no blanket POSIX/Go lifetime equivalence is claimed.
Recommend explicit process_exit(status) for deterministic program results, and
settle program-main return vs generic-task return before releasing a native ABI.
See SEMANTICS-REVIEW.md; completion-record/reap uniqueness does not imply one
legacy SIGCHLD handler invocation (legacy signal fanout remains an adapter).

**Important source-review finding:** already-sleeping peers wake correctly, but
fatal cancellation can arrive just BEFORE old interruptible park admission and
be followed by sleep after the wake was attempted. See KNOWN-WAIT-ADMISSION-GAP.md.
This exact interleaving is NOT yet an executed deterministic guest reproduction.
The current gate covers runnable/current-peer group cancellation, not complete
blocked-peer cancellation. Recommend explicit interruptible park admission +
deterministic pre-park cancellation tests next; do not blindly make all kernel
cleanup sleeps interruptible. It is a progress/cancellation blocker even though
live/resident references continue to prevent premature AS/stack reclamation.

Concurrent VM mutation/shared physical-frame accounting/TLB shootdowns remain
unsolved. FD/cwd/credential/signal metadata remains copied TCB compatibility state;
sharing needs backend lifetime pins. Native user capabilities/IPC are NOT exposed.
PID1/supervisor death and retained native task-join results remain later policy.
Measured ELF storage:32 TCB slots=62976B (1968 each), owner table1536B (48 each);
dynamic kernel stacks16KiB, fixed32 limit and global lock unchanged. No cheap-task,
scalability, latency, total PMM-leak or complete SMP VM safety claim.
