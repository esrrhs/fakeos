# Pipe / signal / orphan regression status

## Upstream dependency (2026-11-10, Asia/Shanghai)

The seven failures below reproduce on fakeos `ca6a92aba9fec8ecd58441e107596cfd9325b5c5`
built with released fakecc `027e78655763651849220929430bae3a8b5cb0ce`.
The implementation fix landed on fakecc master as
[PR #116](https://github.com/esrrhs/fakecc/pull/116)
(`dfb1c7b649480e2acfef2a0a02e8168b5b28ee38`, local fast-forward). No kernel test was
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
## Follow-up: complete SMP2 compiler workload

The earlier AP SYSRET fix (`9f659809`) was real but incomplete: it removed
`sysretq` #DF without fixing all CPU-local execution state. On that commit,
a diagnostic SMP2 boot reproduced the later failure and recorded:

- CPU1 after `fork`: RIP `0x400215` is `ret` in the shell's `u_fork` stub;
  CR2=RSP=`0x7f7ff0e8`, error `0xc`. EFER=`0x501` (NXE missing), hardware
  CR3 and software root both `0x3a33000`. The hardware walk had present
  ancestors `0x3a38027`, `0x3a3b027`, `0x3a3c027` and a present NX leaf
  `0x8000000003a32065`. This is a reserved NX-bit fault, **not** an absent
  stack page inferred from the printed error label. BSP EFER was `0xd01`.
- CPU0 subsequently faulted writing the shell's COW data at `0x44e7d8`
  (RIP `0x424feb`, error `7`): hardware CR3=`0x3a01000`, while the global
  software active-AS pointer reported the kernel root `0x102000`. The AP's
  idle switch had overwritten that pointer, so the BSP skipped its actual
  process's COW fault handler and killed pid 1.

Two minimal kernel corrections address the separate causes:

1. Call the existing CPU-local `mem.vmm_init()` on every AP before enabling
   its timer/user scheduling, setting EFER.NXE for the existing NX mappings.
2. Track the installed address space per CPU, matching CR3; update the pointer
   and CR3 together with IRQs disabled. Fault dispatch and COW TLB invalidation
   select the calling CPU's address space, not another CPU's last activation.

Regression coverage extends the AP worker check to EFER.SCE+NXE and matching
software/hardware CR3, using independent result slots instead of a racy shared
completion increment. The guest smoke now explicitly waits for AP startup and
all final boot tests, requires that AP check, and still runs the actual embedded
compiler compile/execute plus ELF64/bin assembly. It does not pin users to the
BSP or disable SMP, skip the workload, or mask faults.

Validation commands (from the fakeos checkout, preexisting released seed):

```sh
FCC=/tmp/fakecc-pr116-merge/build/fakecc \
FAKECC_SRC=/tmp/fakecc-pr116-merge bash tools/m5_no_host_cc_smoke.sh
python3 tools/m5_guest_smoke.py --require-process-selftests --smp 1
python3 tools/m5_guest_smoke.py --require-process-selftests --smp 2
```

`/tmp/fakecc-pr116-merge` is a clean source tree at released fakecc
`dfb1c7b649480e2acfef2a0a02e8168b5b28ee38`; the rebuilt seed and embedded
compiler hash is `147b2dd2339e142d74830f6bc4086c519bfd0c6230577a639ba20af11c39e9bb`.
Detailed diagnostics/logs are retained outside the build directory under
`/tmp/fakeos-smp2-pf-investigation/`.

Validation results on the fresh clean image (not cached incremental objects):

- Clean deny-tools smoke passed: zero denied invocations, all 34 embedded
  modules/provenance verified, second selfhost byte-identical, actual boot
  ELF64 and AP flat-bin assembly/seed parity passed.
- SMP1: 1/1 full strict guest smoke passed.
- SMP2: 3/3 independent fresh-VM full strict guest smokes passed (plus one
  earlier incremental-image confirmation). Each completed ping compile/run,
  provenance and ELF64/bin assembly after AP startup; all seven original
  regressions and the AP check passed. No FAIL lines or post-boot unhandled
  exceptions appeared in those four clean-image runs.
- Saved clean logs: `clean-smp1.log`, `clean-smp2-run1.log` through
  `clean-smp2-run3.log` under `/tmp/fakeos-smp2-pf-investigation/`.

The historical full in-OS kbuild restage pin remains outside this work.
