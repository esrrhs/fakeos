# Wait/cancel checkpoint — 2026-11-18 00:31 Shanghai

Exact-window singleton fatal/caught/pre-wake races were reproduced and fixed in
initial targets. Group case exposed a TEST fixture bug: its kernel rendezvous
used yield while BSP coordinator is the idle task, starving that coordinator on
SMP1 (and sometimes SMP2/4). Case6 timed out; it was not evidence of the claimed
group race. Initial strict assertions correctly failed; logs are retained.
The baseline-analysis assertion also detected this, but that shell lacked set-e
and proceeded to the fixed run; no PASS was reported. All following pipelines
use explicit fail-fast dependent steps.

Correct fixture blocks on an uninterruptible condition-protected rendezvous,
allowing BSP progress, then waits until leader has actually group-exited42 and
live count is1. Rescue detection excludes this intentional kernel gate and only
counts the actual user futex channel. No affinity, forced mask, sleep or polling
workaround. Rerunning the SAME corrected fixture on original production waits
before applying the correction again. All prior accepted patches preserved.

## 00:38 Shanghai — causal baseline established; final remote fixture live
With the corrected blocking rendezvous all18 cases completed on SMP1/2/4.
Exactly cases0/3/6/7 failed twice each: pre-park fatal, pre-park caught nonrestart
signal, final group peer after leader42 detachment, pre-park normal notification.
Each actual strand needed one explicit rescue (recorded FAIL); group status42
was reaped only after rescue. No production correction is claimed accepted yet.

Final strengthened baseline job444798 PID2126746 is the ONLY active pipeline.
It moves the coordinator from idle BSP into an ordinary schedulable kernel task
so load-aware enqueue has genuine remote CPU opportunity. It also writes the
normal completion predicate before its wake, making that causal proof unambiguous.
No affinity/forced mask; final baseline precedes reapplying the atomic fix.
Baseline log baseline.log; accepted toolchain dfb1c7b unchanged. Worktree is
currently ORIGINAL production wait semantics plus trusted boot test hooks.
The prepared fix is /workspace/apply_fakeos_wait_fix.py, not applied while this
sole baseline VM/build is live. Prior failed fixture results are retained.
