# PASS — native SMP diagnosis and full clean acceptance

Finished 2026-11-17 23:09:04 Asia/Shanghai. Job444781 actual EXIT0;
`NATIVE_CI_PIN_GATE_EXIT=0`; frozen source/test input changes=0. No job/VM remains.

## Cause and correction
The prior a2158f translated compiler removed the indexed-width RMW guard that
exists in CI-pinned dfb1c7b. Actual failed-image cfs_rb_insert machine code writes
cfs_nr through a stale TCB-offset register, omitting the valid counter-index/load/
increment computation. The executable counter reproduction returns42 with the
retained prior compiler and0 with the exact pinned compiler; both machine codes
are retained. This is scheduler-state corruption caused by compiler misgeneration,
not merely a test having too little parallel work. Before changing any scheduler
or workload source, an exact-pinned CLEAN build passed the unchanged SMP2 case:
mask3, remote2/IPI2, worker hits2/2. No scheduler algorithm rewrite was justified.

Existing exact-pinned source was audited against EVERY tracked Git blob (zero
mismatches). The initial seed was rebuilt/installed privately with the existing
CI CMake Release procedure; all fakeos and staged compiler work then used fakecc.
Neither workflow/default build settings nor shared/isolated compiler sources were
edited. This is local acceptance using the actual CI pin, NOT a remote CI run.
The old PATH seed f74449... and a2158f recovery are not dependencies of this run.
See REPRODUCE.md and toolchain.env for exact commands and explicit identities.

## Regression strengthening (not relaxed assertions)
New task edits are limited to kernel/native.c, kernel/proctest.c,
scripts/test_native_objects.py and this evidence directory. Original assertion39
is retained. The bounded barrier publishes4 runnable workers, released by the
last worker. First controller-based test failed on SMP1 because kmain is the
BSP idle TCB and cannot run while workers stay runnable; that failure and its
machine code remain under controller-barrier-failure*. Last-worker release fixes
that TEST controller inversion, without arbitrary sleeps or CPU affinity.
Test shared counters now have proper locks; per-core successful send/receive
counts, exact-once512, real mask, CFS tree-count checks, and remote enqueue + IPI +
AP execution are required. No latency/scaling/performance claims are made.

## Actual acceptance
* Three repeats of SMP1/2/4:9/9 pass, payload-operation masks1/3/f EVERY time.
  Per-core send totals512 and receive totals512 each; close/revocation/pin drain,
  stale/rights/FIFO/backpressure/EOF/exhaustion/rollback/no-leak assertions active.
* One subsequent FULL CLEAN make test: fresh pure-fakecc build; its SMP1/2/4 pass;
  fixture5 + transfer11 tests; Ext2 exact reclaim/reuse/survivor two-boot and VFS
  synced namespace two-boot workflows pass.
* Fresh clean host/staged reference parity40/40, PLUS **actual guest** kbuild
  recompiles40 distinct modules and compares every object byte-for-byte (not just
  a host reference assertion); native.o and proctest.o included.
* Actual guest link and framed dump: 21713824 bytes;
  SHA256 `40695ab663c9217d157f45971bb6f6deb7f561d33c5cb959bfcedb58a30a89ac` equals the independently trusted reference. ELF/loadable-range,
  large-image and truncated-object rejection checks passed.
* Actual reconstructed-kernel second boot: all31 strengthened checks passed,
  including login/hello/exit, signal/pipe/orphan and native-channel regression.

Main proof: full-clean-acceptance.log (guest object lines3764–3843; final checks
and true exit at end), ACCEPTANCE-RESULT.json, repeated-results.json,
full-artifacts/test-qemu-rebuilt.serial.log, clean-gate-host-stage-parity.json.
The actual reconstructed ELF remains build/fakeos-rebuilt.elf.
No process lifetime refactor, native user handle exposure, commit/push, shared
fakecc modification, blocked network diagnostic, or unrelated feature work.
All accepted FS patches remain untouched. Remaining32-task/global-lock limits
and kernel-only channel scope remain: this is not user-process IPC acceptance.
