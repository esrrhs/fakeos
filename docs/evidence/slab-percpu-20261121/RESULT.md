# Per-CPU slab caches + fine-grained classes (jemalloc/tcmalloc-style)

Local acceptance, 2026-11-21, macOS arm64 host, homebrew qemu-system-x86_64,
kernel built by the staged self-hosted fakecc toolchain. LOCAL result only —
the Linux-CI-only reference-oracle gate (prepare_reference_image host linker
execution) cannot run on this host; the reasoning is in
docs/evidence/native-wait-cancel-20261118/RESULT.md. No commit/push claimed.

## What was implemented

mem/slab.c rewritten from an 8-class, fully heap_lock-serialized single-page
slab heap into a two-tier concurrent allocator:

* **24 size classes** (was 8): jemalloc-style four sub-steps per doubling —
  16, 32, 48, 64 / 80..128 / 160..256 / 320..512 / 640..1024 / 1280..2048 —
  every class a multiple of 16 B so the historical 16-byte kmalloc
  alignment invariant holds and every existing caller keeps working.
  Worst-case rounding waste drops from 46 % (17 B request -> 32 B class) to
  at most 19 % at the sub-step boundaries.
* **8-page (32 KiB) slab runs** (was single pages): the 64-byte run header
  sits at the buddy order-3 block base and objects span the run contiguously.
  The 2048 B class now holds 15 objects per run (was 1 per page); the 16 B
  class holds 2044. Metadata amortises to < 0.2 % of run bytes for large
  classes.
* **Per-CPU caches** (tcmalloc thread-cache / jemalloc tcache analogue):
  kmalloc/kfree fast paths pop/push the calling CPU's cache with NO lock —
  cpu_irq_save is the only mutual exclusion, so concurrent allocators on
  different CPUs never share a lock. The CPU identity comes from an injected
  hook (`kmalloc_cpu_hook`, assigned `&smp_cpuid` by kmain) because the mem
  package does not import the kernel package; unset hook degrades to one
  shared cache row, correct but less concurrent.
* **Bounded batch transfers**: a refill pulls CACHE_BATCH (16) objects per
  heap_lock acquisition when a cache empties; a flush returns down to
  CACHE_BATCH when it hits CACHE_DEPTH (32). A hot cache therefore cannot
  pin an entire class, and the central layer is amortised across batches
  instead of individual objects.
* **O(1) central placement**: each class has a "current" run plus a singly
  linked partial chain. Head insert on full->partial, head take on current
  exhaustion — no back pointers, no scans (the old allocator walked the
  whole slab chain per allocation). Invariant: every partial entry has
  nfree > 0; only the current run is drawn from.
* **Statistics**: per-CPU/per-class fast-alloc, fast-free, refill, flush
  counters (kmalloc_cache_stats), kmalloc_cache_flush_all for exact
  accounting checkpoints, kmalloc_cache_cpu_rows.
* **Preserved guarantees**: page_info O(1) dispatch, big-path double-free
  rejection via INFO_NONE, 16-byte alignment, HHDM pointers, heap_lock ->
  pmm_lock order (cache paths take heap_lock only in refill/flush; the fast
  paths take no shared lock at all).

One fakecc constraint shaped the design: 5-argument calls miscompile (see
docs/evidence/native-caps-20261121/RESULT.md), so every new function stays
within 4 arguments, and the CPU hook is a global data symbol assigned by
kmain rather than a function-pointer parameter (fakecc cannot parse
function-pointer prototypes; the arch-package global-hook pattern is used).

## Deterministic proof

* `test_slab_newclasses` (boot): every one of the 24 classes — at exact
  class size and at the largest size rounding into it — allocates, keeps
  16-byte alignment, byte-exact canaries. PASS on SMP1/2/4.
* `test_slab_concurrent` (boot, after sched_smp_start — the concurrent
  test needs a live scheduler, which is why it is a separate selftest from
  slab_selftest): 4 kernel workers, 300 iterations x 40-allocation
  same-class bursts (forcing refills mid-burst) with deferred frees through
  a cross-worker ring, byte-granular canaries per object. After joining:
  every deferred pointer freed exactly once, kmalloc_cache_flush_all, then
  per-CPU stats must account >= every worker allocation on the lock-free
  path, refills > 0, and on SMP2+ at least two CPUs with lock-free traffic.
  Observed on SMP2: cpu0 fast-alloc 32021, cpu1 20642, refills 75/24 —
  real concurrent lock-free traffic on both CPUs. PASS on SMP1/2/4.
* All pre-existing slab tests pass unchanged: 1024-object class-boundary
  test, 64-block big path with exact buddy accounting, 8/16 MiB large path,
  cached-slab reuse, double-free rejection.
* Full `make test` regression: all locally runnable stages PASS (native
  objects, native caps 9/9, process lifetime, fixture, ext2 reclaim, vfs
  mounts, bootstrap transfer units); the wait/cancel 18-case suite stays
  failures=0 in the same boots; test_qemu.py stops only at the Linux-CI-only
  host-linker oracle as documented.

## Remaining limitations (deliberate)

* Slab runs never shrink back to the buddy (jemalloc-style retention);
  class run counts grow to peak demand. Exact buddy accounting for slab
  objects is therefore not asserted (the big path keeps exact accounting).
* The deferred-free ring in the concurrent test is drained from the driver
  CPU; per-CPU free distribution is observed, not asserted symmetric.
* No per-CPU cache for the big path (buddy blocks remain heap_lock
  serialized); no decay/purgemaging thread (jemalloc background decay).
* Cache rows are static 8 x 24 x 32 entries (~50 KiB BSS).
