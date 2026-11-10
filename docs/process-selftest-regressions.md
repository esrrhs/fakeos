# Pipe / signal / orphan regression status

## Upstream dependency (2026-11-10, Asia/Shanghai)

The seven failures below reproduce on fakeos `ca6a92aba9fec8ecd58441e107596cfd9325b5c5`
built with released fakecc `027e78655763651849220929430bae3a8b5cb0ce`.
The implementation fix is **unmerged** fakecc [PR #116](https://github.com/esrrhs/fakecc/pull/116),
head `dfb1c7b649480e2acfef2a0a02e8168b5b28ee38`.
Default released fakecc is therefore not yet claimed fixed. No kernel test was
removed, weakened, or bypassed, and no speculative pipe/reparent kernel change
is needed for these seven regressions.

The compiler's abs32 indexed read-modify-write matcher removed load/ALU/address
producers for dword operations, but its indexed STORE_PTR emitter only handled
bytes. The generic fallback consumed dead/reused register homes. A hardware
watchpoint caught a CFS counter store using a thread-stride-scaled index and
writing into pipe data byte 444. Indexed pending-signal updates were also
miscompiled. Failed signal tests left children behind, making the later orphan
wait reap an unexpected child. The upstream guard preserves ordinary scalar
load/compute/store code for unsupported indexed widths; its matching checked-in
`v0/codegen.c` is regenerated, with a dedicated indexed mask/counter regression.

## Reproduction / validation

Use a seed compiler built from the PR head, and the matching source snapshot;
merely pointing FAKECC_SRC at the fix while using a broken compiler to compile
the kernel is not sufficient. With an existing seed, build the corrected seed:

```sh
FCC=/absolute/path/to/existing-seed \
FAKECC_SRC=/absolute/path/to/fakecc-at-dfb1c7b6 \
  bash tools/m5_build_fakecc.sh /absolute/path/to/corrected-seed
FCC=/absolute/path/to/corrected-seed \
FAKECC_SRC=/absolute/path/to/fakecc-at-dfb1c7b6 \
  bash tools/m5_no_host_cc_smoke.sh
python3 tools/m5_guest_smoke.py --require-process-selftests --smp 1
python3 tools/m5_guest_smoke.py --require-process-selftests --smp 2
```

The strict flag requires every PASS below, rejects targeted FAIL lines, and
requires an immediate external `/bin/true` to exit 0 after login (rather than
waiting out signal tests to mask a spurious exit 130). It still runs the full
M5 guest compiler smoke afterwards; a later failure is not hidden.

| Original failure | Corrected compiler, SMP1 and SMP2 boot tests |
| --- | --- |
| pipe data mismatch at 444 | pipe kmutex + wait_queue_sleep_unlock blocking PASS |
| kill/wait status=0 reaped=0 | kill SIGKILL + wait status PASS |
| kill(SIGTERM) for handler | SIGTERM handler + sigreturn PASS |
| kill(SIGSTOP) | SIGSTOP/CONT + WUNTRACED/WCONTINUED PASS |
| kill for pipe EINTR | pipe read EINTR + handler PASS |
| kill for SA_RESTART | pipe SA_RESTART keeps read PASS |
| orphan reparented to pid 1 | orphan reparented to pid 1 PASS |

Validation with the PR compiler:

- New compiler regression: old compiler exits 41 at O0/O1; corrected compiler
  exits 0 at both. All eight codegen e2e cases at O0/O1 pass (16/16).
- Clean default fakeos build: zero denied host compiler/assembler/linker tools;
  second selfhost byte-identical; actual boot/ap_tramp bin/ELF64 parity passes.
- SMP1 strict guest smoke passes all seven, immediate external exec, embedded
  `/bin/fakecc` ping compile/execute, provenance, ELF64 and flat-bin assembly.
- SMP2 passes all seven targeted boot tests and immediate external exec, but
  the subsequent guest compiler workload **fails** in two runs: #DF at
  `RIP=0xffffffff801055d5`, with user-range `RSP=0x7f7ff0e8` (one run also
  reports a user stack write #PF). The helper exits nonzero waiting for
  `M5_C_END`. Its cause is not established here; complete SMP2 compiler smoke
  is **not green**, and this is not presented as a fully completed default
  release fix.

The historical full in-OS kbuild restage pin remains outside this fix. No
unrelated PR, CI repair, or compiler master merge is included.
