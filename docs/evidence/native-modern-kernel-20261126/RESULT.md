# Modern Native High-Performance & High-Concurrency Kernel Slice (2026-11-26)

Full Linux x86-64 acceptance (`fakecc` `42439c8f`, QEMU 8.2.2 MTTCG SMP 1/2/4,
including host reference-linker oracle and 43-module in-OS byte-identical
self-bootstrap + second boot).

## Final acceptance gate

The complete gate passed in the `fakeos-dev` QEMU toolchain container:

```text
FAKECC_SRC=/tmp/fakecc FCC=/tmp/fakecc-prefix/bin/fakecc make test
All boot, self-test, shell-session and self-bootstrap checks passed successfully!
```

Evidence includes host units `8/8`, native objects/caps/modern probes on SMP
1/2/4, native mutex and rlimit probes, VDSO/process/wait/VFS/Ext2/network
regressions, 43/43 byte-identical kernel objects, a `21050432`-byte rebuilt
image with `1714` decoded dump frames and matching SHA256, plus successful
second boot of the rebuilt image.

## Summary of Kernel Deliverables

1. **Zero-Leak VM Teardown, Batched Unmap Shootdown & 64-Task Capacity (`mem/as.c`, `kernel/memtest.c`, `kernel/sched.c`, `fs/ramfs.c`)**:
   * Fixed the tracked 1-page-per-fork physical frame leak: `as_fork_h` previously called `as_create()` (allocating a PML4 page via `as_init`) and then `as_cow_clone()` (which called `as_init` a second time, overwriting and leaking the first PML4 page on every fork).
   * Fixed `VMA_PHYS` fault refcounting (`frame_refs[pfn]` pinned at 1 owner + 1 per mapping under `ref_lock`) so multi-process vDSO / DMA mappings never underflow to 0 on exit.
   * Wired 64-page batched `as_shootdown_range` before `pmm_free_page` in `unmap_present_pages`, closing stale-TLB-before-free on concurrent `munmap`. Upgraded VMCC page-drift `[WARN]` to a hard `[FAIL]` assertion: **exact 0-page drift** across 18 concurrent COW forks + 21 cross-CPU shootdowns.
   * Restored `MAX_THREADS = 64` (removing the `BISECT` cap) and scaled `/proc` dynamic slots (`PROC_PID_SLOTS = 64`, `MAX_INODES = 680`, `MAX_OPEN_FILES = 128`).
   * Added atomic `cpu_sti_hlt` (`sti; hlt; ret`) for BSP idle yields, eliminating 100% CPU `cli/sti` busy-spin when idle.

2. **Per-Task Monotonic Deadline Timers & Unified Timed Wait Contract (`kernel/sched.c`, `kernel/sysproc.c`, `kernel/sysfile.c`, `kernel/epoll.c`, `kernel/uproc.c`)**:
   * Added per-task `timer_chan`, `deadline_ns` (calibrated `tsc_ns`), and `deadline_ticks` integrated into `sched_park_commit` / `sched_park_commit_unlock_spin` (`PARK_TIMED_OUT = 3`) and `sched_expire_timers_tick()`. Only expired sleepers wake — zero shared-channel wake floods.
   * Removed `nanosleep` busy-yield spin (`sched_sleep_deadline`), wired relative `timespec` timeouts (`ETIMEDOUT = -110`) into `futex(FUTEX_WAIT)`, parked finite-timeout `select`/`poll`/`epoll_wait` sleeps, and added monotonic absolute deadline syscalls `nat_send_timed` (`518`), `nat_recv_timed` (`519`), and `nat_mutex_lock_timed` (`520`) with overflow validation and commit-wins-over-timeout semantics.

3. **Sharded Native Object Core, Bounded Byte Payloads & Two-Phase Capability Transfer (`kernel/native.c`, `kernel/natcap.c`)**:
   * Widened pools to `NC_OBJECTS = 64`, `NC_HANDLES = 256` (48-bit generations), and `NAT_CAPS = 32` per process.
   * Split global `nc_lock` into a pure leaf registry spinlock (`nc_reg_lock`) plus 64 per-object spinlocks (`nc_obj_locks[64]`), ordered `nc_obj_locks[low..high] -> {nc_reg_lock, sched_run_lock}`.
   * Added `nat_chan_send_msg` (`521`) and `nat_chan_recv_msg` (`522`): `0..256` byte copied messages (validated/copied outside IRQ-off spinlocks; short buffer returns `EMSGSIZE = -90` without consuming the head message) plus `0..4` capabilities (`NATC_TRANSFER = 32`) supporting attenuated `DUP` and two-phase all-or-nothing `MOVE`, with iterative queued-capability discard on final receiver close.

4. **Native Multi-Object `nat_wait_any` (`523`, `kernel/native.c`, `kernel/natcap.c`)**:
   * Multiplexes up to 16 channel (`NATW_RECV`, `NATW_SEND`), mutex (`NATW_MUTEX`), and task/process completion (`NATW_DONE`) capabilities using ascending object-ID lock acquisition, one shared wait token, per-CPU rotating scan cursor (preventing index-0 starvation), monotonic deadlines, and interruptible park-admission with deterministic deregistration.

5. **Zero-Legacy Native Execution Primitives & Process-Shared Resource Namespace (`kernel/sched.c`, `kernel/uproc.c`, `kernel/natcap.c`, `kernel/syscall.c`, `kernel/sysfile.c`)**:
   * `nat_process_spawn` (`524`): spawns an isolated process directly from a Ramfs or Ext2 ELF image without `fork()` COW cloning (works cleanly from multithreaded parents where `fork()` returns `EBUSY`), installs only explicit bootstrap capabilities at `(1<<16)|1..k` (and optional console fds), and returns a typed `NC_PROC` capability (`JOIN|CANCEL|INSPECT|TRANSFER`) with zero POSIX `wait4` zombie residue.
   * `nat_task_spawn` (`525`), `nat_task_exit` (`526`), `nat_exec_wait` (`527`), and `nat_exec_cancel` (`528`): typed `NC_TASK` / `NC_PROC` capabilities decoupling 64-bit completion results from immediate 16 KiB kernel-stack reclamation, with task-scoped cancellation that aborts/exits only the targeted task without killing sibling tasks.
   * Moved `fd[16]` (with `FD_RESERVED` allocation, per-task `pinned_fh`, and `deferred_close[16]` UAF protection against concurrent `close`/`dup2`), `cwd[512]`, `cmdline`, `environ`, `umask`, and credentials out of `struct tcb` into `struct process_owner`, shrinking each TCB by >1 KiB and giving threads true shared descriptor/filesystem semantics.

6. **Per-CPU Runqueue Lock Sharding (`kernel/sched.c`, `kernel/memtest.c`)**:
   * Split `rq_locks[cpu]` + per-CPU `cfs_min_vr_cpu[cpu]` (with vruntime migration normalization on steal, dual-RQ lock order `min(cpu,remote) -> max(cpu,remote)`) from `sched_run_lock`.
   * `sched_tick`, `sched_yield`, `sched_resched_from_ipi`, `sched_ap_enter`, and `switch_context` / `sched_finish_switch` hold only `rq_locks[cpu]`; sleep/park/exit paths drop `sched_run_lock` before switching stacks.
   * **Measured VMCC churn impact**:
     * SMP2 global `state_lock`: `3,752,126` holds / `367` contended (pre-sharding) -> **`9,308` holds (-403x) / `5` contended (-73x)**.
     * SMP4 global `state_lock`: `1,584,368` holds / `1,153` contended (pre-sharding) -> **`9,290` holds (-170x) / `6` contended (-192x)**, with all 4 CPUs independently driving `rq_locks[0..3]` (`18246 / 4552 / 3780 / 4960` holds).
   * Widened `user/kbuild.c` `READ_CAP` from stack `128 KiB` to BSS `512 KiB` so large kernel objects (`ext2.o`, `sched.o`) are compared byte-for-byte in full without truncation.
