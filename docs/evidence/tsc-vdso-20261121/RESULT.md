# TSC high-resolution clock + vDSO-lite clock page

Local acceptance, 2026-11-21, macOS arm64 host, homebrew qemu-system-x86_64
(TCG), kernel built by the staged self-hosted fakecc toolchain. LOCAL result
only; the reference-oracle gate remains Linux-CI-scoped (see
docs/evidence/native-wait-cancel-20261118/RESULT.md). No commit/push claimed.

## What was implemented (review item P0-7/8)

The whole kernel read time only through the 100 Hz LAPIC tick counter
(`clock_ticks`, BSP-incremented): clock_gettime resolution was 10 ms and
every read was a syscall. kernel/tsc.c (new module, boot manifest 42
modules) adds:

* **TSC monotonic clock, ns resolution**: `tsc_read()` (rdtsc; shift; or;
  ret as machine code — see the fakecc constraint below), calibrated ONCE
  at boot against the PIT (`pit_busy_wait_ms(100)` — real-time hardware
  pacing, independent of the LAPIC tick and of which CPU runs it):
  `tsc_hz = delta * 10`. A fixed-point multiplier (mult = (1e9 << 20) / hz,
  shift 20) keeps `ns = (cycles - base) * mult >> 20` free of 64-bit
  overflow for millennia of uptime. Plausibility gate: rates outside
  500 MHz..20 GHz fall back to the tick clock (all callers stay correct).
  Measured under QEMU TCG: 1,000,510,000 Hz.
* **clock_gettime(2)**: CLOCK_REALTIME/MONOTONIC/BOOTTIME serve
  tsc-ns (10 ms -> ns) when calibrated, tick fallback otherwise. No RTC
  yet: REALTIME stays boot-relative (unchanged, documented).
* **nanosleep(2)**: exact TSC deadline — the tick sleep can overshoot by
  up to one tick, so the total wait is never short and precise to the
  calibrated TSC; POSIX remainder-on-EINTR is now exact ns (was ticks).
* **vDSO-lite clock page** (`tsc_vdso_init` + `as_vdso_attach`): one
  kernel-owned page (magic/version/mult/shift/base/tsc_base/flags) mapped
  into EVERY address space at a fixed VA (0x50000000) as a pinned
  `VMA_PHYS` mapping — inherited through fork, present after exec.
  User code reads its own TSC and the page and computes wall/monotonic ns
  WITHOUT a syscall. Layout: u32 indexes 0 magic, 1 ver, 2-3 mult (u64),
  4 shift, 5 flags, 6-7 base_ns (u64), 8-9 tsc_base (u64).

## fakecc constraints hit in this slice (both documented, worked around)

* `.S` assembler silently DROPS `rdtsc` (same class as `lock cmpxchg`):
  the instruction vanished from the assembled output. Machine code goes
  through `__attribute__((naked))` + a SINGLE `.byte` directive with
  comma-separated decimal bytes — fakecc keeps only the FIRST .byte
  directive of a statement (multi-directive statements lose everything
  after the first), and the wire-test format (decimal, one directive)
  assembles byte-exact.
* The naked function works in the KERNEL and in USER code only in this
  single-directive form; earlier multi-directive and 3-argument forms were
  silently mangled (dropped instructions, injected mov) or corrupted the
  caller's frame.

## Deterministic proof (scripts/test_vdso.py, in make test)

* `SMP2 PASS: vDSO clock monotonic + syscall agreement + sleep precision`
* `SMP1 PASS: ...` (calibration is CPU-independent — PIT window)
* Assertions per run: calibration present and plausible (0.5..20 GHz);
  64 vDSO reads non-decreasing; vDSO-computed time within 100 ms of the
  syscall clock_gettime (they share one calibration — agreement proves the
  page math and the mapping); nanosleep(50 ms) measured 45..130 ms by the
  vDSO clock itself; no [FAIL] markers in the boot.
* Full `make test` regression on the same build: zero [FAIL], zero [WARN]
  (the ~1-page/fork teardown leak from the VMCC slice did not manifest in
  this run — it is timing-dependent and remains tracked in
  docs/evidence/vm-shootdown-20261121/RESULT.md).

## Bugs found and fixed during this slice

* vDSO publish used BYTE offsets as u32 indexes (the RAWO pattern is
  indexes; the constants switched mid-slice) — magic was overwritten by
  version. Indexes are now consistent on both sides, asserted by the probe.
* The user probe read mult/base from the wrong slots after the layout
  change, computing ~0 elapsed time (drift == uptime). Field indexes now
  mirror VDSO_OFFSET_* exactly.

## Remaining limitations (deliberate)

* No RTC/CMOS read: REALTIME stays boot-relative (all three clock IDs move
  together). A CMOS read would anchor REALTIME to wall time.
* Under TCG the TSC pace is emulated (~1 GHz here, not host-accurate);
  monotonicity and agreement hold, absolute wall precision does not apply.
  Under KVM the calibration is host-accurate.
* Cross-CPU TSC skew is not synchronised (Linux does per-CPU corrections):
  reads on different CPUs may disagree slightly; monotonicity is per-CPU.
  The vDSO page is a shared calibration, not per-CPU state.
* The vDSO page is read-only to user code and static after calibration —
  no settimeofday/adjtime (no clock discipline), no timers based on it yet.
