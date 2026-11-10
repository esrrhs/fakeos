# Daily command-line status (fakeos)

This is a short note of what a login shell can actually do today, not a
roadmap and not a claim that fakeos is a daily driver.

## Landed in this increment

Live `/proc/<pid>/{status,maps,cmdline,environ}` for every visible user
process (including unreaped zombies), `/proc/self` (unchanged contract),
and `/proc/meminfo` built from real PMM page counters (`MemTotal`,
`MemFree`, `MemAvailable`, `MemUsed` in kB — no invented Cached/Buffers
figures). `/bin/ps` and `/bin/free` read those nodes; writes, creates,
unlinks and renames on the synthetic tree are refused. Numeric pid
directories are bound to a reserved inode slot on lookup because
`next_pid` grows past the thread-table size.

A QEMU login workflow is `tools/m5_daily_proc_smoke.py`: `ps` shows the
shell as pid 1, `/bin/sleep 20 &` appears, `kill` plus `wait` removes it,
and `free` / `cat /proc/meminfo` show a non-zero `MemTotal`.

Init selftest: `[U][PASS] /proc pids meminfo`.

Userland ELFs are published from a packed `user_blob_table` in
`user/blob.S` so `rootfs.c` does not take a GOT entry per `*_start`/`*_end`.
That keeps the kernel link under fakecc `--kernel-multiboot`'s current
256-entry GOT cap (which fails silently if exceeded).

## Limits

`ps` lists user process leaders only (no kernel idle threads, no `-L`
thread rows, no tty/time/RSS columns). `/proc/<pid>/fd` and a richer
`/proc/stat` are not present. `free` is kB-only (`-k` accepted).
After `init` execve's into `login`/`sh`, pid 1 is the shell, not
`/bin/init`.

## Next gaps (command-line, not GUI)

1. `cp -p` / `touch -d`: `utimensat` already exists, but `cp` still
   documents `-p` as deferred and `touch` has no `-d`.
2. Job/session usability: named pipes (`mkfifo`), a pager (`less`/`more`),
   and richer job control remain missing.
3. Persistence is available (`ext2` `/home`, `qemu-disk`) but a
   documented isolated reboot-survival smoke is not part of this
   increment; treat it as the next file-workflow check if something
   looks broken. Historical in-OS kbuild restage is still pinned and
   is not fixed by this work.
