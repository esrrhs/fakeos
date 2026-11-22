# run_lock instrumentation (P0-3 step 1): measured contention + hold times

Local acceptance, 2026-11-21, QEMU TCG SMP1/2/4. Every direct
spin_lock/spin_unlock of `sched_run_lock` in kernel/sched.c (64 lock sites,
61 unlock sites — the asymmetry is the early-init assignment and comment
mentions) now goes through `runlock_lock()`/`runlock_unlock()`, which count
per-CPU acquisitions, contended acquisitions (test-and-set failed before
the spin) and hold time via the PIT-calibrated TSC. Exposed read-only as
`rl_holds/rl_hold_ns/rl_hold_max/rl_contends` and printed by
`vm_concurrent_selftest` under load.

## Measured evidence (VMCC cross-CPU churn, per boot)

* SMP2: holds=3,752,126, contended=367, max hold=13.9 ms
* SMP4: holds=1,584,368, contended=1,153, max hold=19.5 ms
* CPU0 holds ~400x more acquisitions than the APs (the BSP performs all
  kernel-side work); contention is real and grows with CPU count.
* Long holds (>1 ms) measured per-site: the context switch (sched_yield
  site) dominates; allocation sites recorded none.

## Sharding decision inputs (architecture-doc step 1 + step 2 complete)

1. Contention is measurable and grows with CPU count -> the global lock is
   a real serialization point under fork-heavy workloads.
2. **Per-site long-hold attribution (step 2)**: with site-tagged
   acquisitions, the >1 ms holds attribute to SITE 2 = sched_yield — the
   context switch runs INSIDE the critical section (charge + pick +
   switch_to until the finish_switch release on the new stack). The
   allocation sites (alloc_slot, process_attach, nat_acquire) recorded
   ZERO >1 ms holds — the earlier guess that fork allocations dominated
   was WRONG; the measurement corrected it.
3. Therefore: there is nothing to "move out" for a cheap win — the long
   hold IS the context switch, which only per-CPU runqueue locks (step 3:
   the handoff release moving to the target-CPU lock, per the
   architecture doc) can shrink. Step 3 is the sharding work itself.
4. Instrumentation overhead: two TSC reads per acquisition; the counters
   are plain per-CPU stores. VMCC and every boot-level test pass with the
   instrumentation active.

## Constraints recorded

* The wrapper replaces 125 call sites mechanically; a self-recursive
  acquisition (the wrapper calling itself) was the one bug introduced and
  fixed — the wrapper's own primitive stays the raw spin_lock/spin_unlock.
* stats writes are plain stores (per-CPU indexed, no atomics needed).
* The wrapper adds two TSC reads per acquisition — bounded slice overhead,
  removed when the lock is split.

Known issue carried: ~1 page per fork+release leaks in the table teardown
path (tracked in docs/evidence/vm-shootdown-20261121/RESULT.md).
