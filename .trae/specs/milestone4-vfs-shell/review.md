# Review R1

- 评审对象：fakeos HEAD `1834d94`（feat: milestone 4 - VFS, Ramfs, POSIX syscalls, serial TTY, libc and shell）
- 评审方式：fresh context 只读走查 + 实机运行；未修改任何源码（仅产生构建产物与本报告）
- 运行证据：
  - `make test` 退出码 0；断言 63 子串 + 7 上下文全部 `[OK]`；随后又独立连跑 5 次 `scripts/test_qemu.py`，全部退出码 0（NFR-1 成立，无 flaky）
  - AC-12 抽查（评审员亲跑，非仅引用实施记录）：128M 由 make test 覆盖；512M / 2G 经 stdin 注入 `hello` + `exit 7`，均得到提示符、hello 输出、`pid 1 exited (code 7)`、SUCCESS，无 `*** CPU EXCEPTION` / `[FATAL]`
  - 关键启动数据：published 六文件大小 22128/15864/14808/14800/14824/111 字节，均非零；pid 2/3 为 COW 子，pid 4=hello(42)、5=cat(0)、6=ls(0)、7=nosuchcmd(127)、pid 1 code 7
  - 增量构建链实测：`touch user/hello.c && make` 依次重建 hello.o → hello.elf → userblob.o(nasm incbin) → fakeos64.elf → fakeos.elf，单文件用户程序改动可正确传播到内核镜像

## Checkpoint Results

- [x] CP-R1 (AC-1 rule): **pass** — 证据：`fs/ramfs.c` 全量实现 + `kernel/vfstest.c:63-243` 约 40 个检查点；运行日志第 247-249 行三行 `[PASS] VFS path resolution` / `[PASS] Ramfs file create/read/write/seek` / `[PASS] Ramfs directory listing`，无 FAIL。覆盖根查找、`.`/`..`/多斜杠折叠、mkdir、6400B 跨 4096→8192 扩容、1MiB 上限拒绝、只读静态文件写保护、枚举与 6 类负面用例。
- [x] CP-R2 (AC-2 rule): **pass** — 证据：`kernel/rootfs.c:34-41` 发布 6 路径；日志 `[FS] published /bin/{init,sh,hello,cat,ls}` 与 `/etc/motd` 六行且字节数 >0；会话断言 `ls /bin` 含 init/sh/hello/cat/ls（5 条上下文检查全 OK）、`cat /etc/motd` 输出 111B 文本两行。
- [x] CP-R3 (AC-3 rule): **pass** — 证据：`user/init.c:59-126` O_CREAT|RDWR 写双份 pattern、lseek 0 读回逐字节比较、第二段比较、追加路径；日志 `[U][PASS] vfs file write/read/seek`；三个负面用例（不存在路径、坏 fd=99、静态文件写打开）后进程存活并成功 execve 到 sh。
- [x] CP-R4 (AC-4 rule): **pass** — 证据：`kernel/tty.c` canonical 行缓冲（CR/LF 收行、回显、0x7F/0x08 退格 `\b \b`、255 字符强制收行、无字符 yield）；日志含多次 `fakeos:~$`（上下文检查 count ≥ 10）、`hello from /bin/hello ... pid=4`、`INPUT123`，每条命令输出后提示符再现。
- [x] CP-R5 (AC-5 rule): **pass（证据形态说明）** — sh 未内建 getpid、提示符也不带 pid，严格字面意义的"shell 自报 pid=1"不存在；但语义有等价强证据：`[UPROC] Started pid 1 from /bin/init`（uproc.c:410）后同一 TCB execve /bin/sh，外部命令 hello 以 **新 pid=4** 运行且输出到达串口（fd 0/1/2 exec 后保持），最后 `exit` 时为 `[UPROC] pid 1 exited (code 7)`——映像替换不换进程成立；`nosuchcmd` 返回 not-found 并出新提示符（exec 失败回滚干净）。
- [x] CP-R6 (AC-6 rule): **pass** — 证据：sh.c:135-148 fork→exec→wait4 后才打印下一提示；上下文检查 `$? status propagation` 通过（hello code 42 后 `echo $?` → 42，日志 `[UPROC] pid 4 exited (code 42)`）；`[UPROC] pid 1 exited (code 7)` 出现。
- [x] CP-R7 (AC-7 rule): **pass** — 证据：init.c:128-178 三个 malloc 块哨兵（含 2048B 越界写校验）、8192B 匿名 mmap 四哨兵、len=0/非 MAP_ANONYMOUS 两个负面；日志 `[U][PASS] brk heap malloc`、`[U][PASS] anonymous mmap`。
- [x] CP-R8 (AC-8 rule): **pass** — 证据：日志按序 `mkdir /home` → `cd /home` → `pwd` 输出 `/home` → `cd ..` → `ls /bin` 五程序；重复 mkdir 输出 `sh: mkdir: /home: cannot create directory` 且提示符再现；`fs_file_is_dir`/`fs_ino_kind` 对文件 fd/路径的拒绝在 sysfile.c:351、388 接线。
- [x] CP-R9 (AC-9 rule): **pass** — 证据：fork 继承 fd 表（sched.c:288-299 复制 + fs_file_retain），execve 不动 TCB fd（uproc.c:301-308 仅换 as）；hello/cat 输出到达同一控制台、cat 显式 open /etc/motd 读成功；阶段三 COW 子用继承的 write(1) 打印两行 PASS。
- [x] CP-R10 (AC-10 rule): **pass** — 证据：test_qemu.py:112-141 保留阶段三全部适用断言（异常捕获 3、APIC 校准 35..75、pmm/vmm/slab 9、VMA/COW 3、协作/抢占 2 等）；日志含 `[U] hello from ring 3, pid=1`、两轮 COW 子/父四行、Ring-3/SYSCALL/COW/抢占四行 PASS（ring-3 timer ticks: 4）。
- [x] CP-R11 (AC-11 rule): **pass** — 证据：12 条注入命令全部得到预期输出，新 SUCCESS 语 `[SUCCESS] fakeos: VFS, Ramfs, POSIX syscalls, TTY input, libc and interactive shell online!` 出现，脚本退出码 0。
- [x] CP-R12 (AC-12 rule): **pass** — 证据：128M（make test，5 连跑）；评审员实跑 512M 与 2G：提示符 ✓、外部命令 hello ✓、`pid 1 exited (code 7)` ✓、SUCCESS ✓、无异常/致命错误。三档均由本次评审独立验证。
- [x] CP-R13 (AC-13 rubric): **4 / 5（阈值 4，通过）** — 锚点依据：
  - 包划分清晰：fs 包自包含 VFS+Ramfs（ramfs.c:10-13 用 extern kprintf 而非 import kernel 避免包环，注释解释了 why），rootfs 发布表放 kernel 侧（rootfs.c）避免 fs 依赖具体镜像；
  - 句柄风格统一：inode/fh/as 全为 u32/u64 索引，无跨包裸结构指针，与 as_h 风格一致；分派全部为 type 字段 + if 链，无结构体函数指针调用；
  - fakecc 契合：grep 全仓无 `#include/#define/#ifdef`、无 DBG/dbg/TODO 残留、无 C 内联汇编；Makefile 全局 `-O0 --target=x86_64-linux` 并注释原因；sysretq 手工字节（sysentry.asm:80-83）、u_mmap RCX→R10（usys.asm:55-62）均有注释；
  - 资源回收主干完整：fd fork 引用/退出 close_all 平衡、console permanent 正确跳过 retain/release、COW frame refs 在 release_pt 正确递减、execve 失败销毁 new_as、DEAD 槽 wait 时 reap；
  - 扣分点：孤儿 reparent 实现参数错误（FINDING-1）、两处用户输入可致内核挂死的硬化缺口（FINDING-2/3，FR-5/NFR-4 明文要求），故不给 5 分。

## Findings

### FINDING-1: 孤儿退出记录 reparent 传错参数，死子记录/TCB 槽泄漏且会错抢兄弟记录
- Severity: major
- 位置: [syscall.c](file:///Users/mingming/project/fakeos/kernel/syscall.c#L136-L141)（proc_terminate）；配合 [proc.c:61-68](file:///Users/mingming/project/fakeos/kernel/proc.c#L61-L68)、[sched.c:525-532](file:///Users/mingming/project/fakeos/kernel/sched.c#L525-L532)
- 问题: 进程 X 退出时依次执行 `sched_reparent_children(pid, 1)`（把 X 的 live **及 DEAD** 子 TCB 的 ppid 改为 1）与 `proc_reparent_exits(ppid, 1)`。但 `proc_reparent_exits(old_ppid,...)` 的语义是“把 ppid==old_ppid 的退出记录改挂”，要收容的是 **X 的死子**（记录里 ppid==X），应传 `pid`；现传 `ppid`（X 的父亲 P）导致两个方向的错误：
  1. X 已死未回收的子：exit record 仍挂 ppid=X 永不被任何 wait 取走（pid 1 按 ppid=1 匹配不到），exit_table 槽与对应 DEAD TCB 槽双双泄漏；
  2. P 名下其他已死未回收的子（X 的兄弟）记录被错误改挂到 pid 1，P 随后 wait 会得到 ECHILD，尽管它确有 zombie 子。
- 复现/推理: 当前自动化流程不触发——init 的两个 COW 子、sh 的每个外部命令都被同步 wait4 回收（日志 pid 2-7 均如此），系统中不存在“父先于 wait 死亡且留有死子”的时刻；故 make test 全绿但 FR-7“孤儿 reparent pid 1”实际未正确生效。构造：父 fork 两个子 A/B，A 退出后父不 wait 直接 exit，B（或任意路径）即暴露泄漏/错挂。
- 建议: 将该行改为 `proc_reparent_exits(pid, 1);`；并建议让 `sched_reparent_children` 只改 `state != TCB_DEAD`（DEAD 槽随 record 一起由 wait 路径处理）或在 proc.c 内对 record/TCB 同批切换后补一行不变量注释。

### FINDING-2: as_user_range_ok 缺加法溢出检查，用户可借超长 len 绕过校验使内核挂死
- Severity: major
- 位置: [as.c:878-888](file:///Users/mingming/project/fakeos/mem/as.c#L878-L888)；利用点 [sysfile.c:288-298](file:///Users/mingming/project/fakeos/kernel/sysfile.c#L288-L298)（console write 先一次性校验再按 len 逐字节取用户地址）；同类隐患 [as.c:746](file:///Users/mingming/project/fakeos/mem/as.c#L746)（mmap hint+len）、[as.c:729](file:///Users/mingming/project/fakeos/mem/as.c#L729)（brk 边界）
- 问题: 校验只做 `(va + len) <= vma->end`，`va+len` 为 u64 可回绕。取 buf=0x7FFFFF（栈 VMA 内最后字节）、len=0xFFFFFFFFFF800001，则 va+len 回绕为 0，通过校验；console 写循环随即访问 p[1]=0x800000（无 VMA），ring0 #PF 经 isr.c:152-166 无法解决且非 ring3，落入 [isr.c:183-187](file:///Users/mingming/project/fakeos/kernel/isr.c#L183-L187) `[FATAL] system halted`。
- 复现/推理: 任意用户程序执行 `u_write(1,(void*)0x7fffff,0xFFFFFFFFFF800001UL)` 即可；read/getdents 路径因 4096 分块且 EOF 会 break 不可达，console write 是确认可达路径。违反 FR-5“非法指针/越界不得内核态访存崩溃”与 NFR-4。
- 建议: `as_user_range_ok` 增加 `if (va + len < va) return 0;`；`as_mmap_anon_h` 对 `hint + len < hint`（及对齐后的 len==0）返回 0。

### FINDING-3: ELF 装载器不校验 phoff/段偏移是否在文件范围内，坏 ELF 可越界读内核内存或致 ring0 #PF
- Severity: major
- 位置: [uproc.c:78-119](file:///Users/mingming/project/fakeos/kernel/uproc.c#L78-L119)（load_elf_into）；越界拷贝点 [as.c:820-855](file:///Users/mingming/project/fakeos/mem/as.c#L820-L855)（as_map_preload 直接 `src_kva + off` 读 filesz 字节）
- 问题: 仅校验 ELF magic 与 size≥64；`phoff`、`phnum` 不校验 `phoff + phnum*56 <= size`，每个 PT_LOAD 的 `off`/`filesz` 也不校验 `off + filesz <= size`、`filesz <= memsz`。用户可 open(O_CREAT) 写一个 magic 合法但 phoff=0xFFFF...FF0000（或 filesz 巨大）的文件再 execve：uproc.c:91-117 会从 kmalloc/rodata blob 之外取 u32（内核堆相邻内容随后被映进用户页→可被读回，构成信息泄露），或直接读到未映射 HHDM 触发 ring0 #PF → FATAL 挂死。
- 复现/推理: init 的负面用例（init.c:232-241）只覆盖“路径不存在”，NFR-4 明确要求“坏 ELF 一律 -1 或进程被猎杀，内核不死机”，该路径目前不满足。
- 建议: load_elf_into 增加：`phoff + (u64)phnum*56 <= size`；每段 `off + filesz <= size`（回绕安全检查）、`filesz <= memsz`、vaddr/memsz 页对齐与用户半区范围；任一不满足返回 0（execve 已能干净回滚 -1）。

### FINDING-4: chdir 成功但 cwd 超过 63 字节时被静默截断，后续相对路径解析落到错误目录
- Severity: minor
- 位置: [sched.c:150-157](file:///Users/mingming/project/fakeos/kernel/sched.c#L150-L157)（cwd_copy_from 截断）；调用点 [sysfile.c:344-356](file:///Users/mingming/project/fakeos/kernel/sysfile.c#L344-L356)
- 问题: FR-10 规定 cwd 上限 64B；sys_chdir 拿到规范化绝对路径后无条件 `sched_cwd_set`，复制时按 CWD_LEN-1 静默截断。例如 `/<31字符>/<31字符>`（规范化后 65 字符）mkdir 可达（名字上限 31、路径缓冲 128），chdir 返回 0，但 cwd 被截，后续 `open("x")` / `cd ..` 解析到不存在的路径且无任何错误提示。
- 建议: sys_chdir 在 set 前检查 `kstr_len(path) + 1 <= CWD_LEN`，超长返回 -1。

### FINDING-5: O_APPEND 只在 open 时定位一次，lseek 后写不会强制回到 EOF
- Severity: minor
- 位置: [ramfs.c:473-476](file:///Users/mingming/project/fakeos/fs/ramfs.c#L473-L476)（open 时置 off=size）；[ramfs.c:562-592](file:///Users/mingming/project/fakeos/fs/ramfs.c#L562-L592)（fs_write_h 不感知 append）
- 问题: POSIX O_APPEND 要求每次 write 前把 offset 移到 EOF；当前实现打开后可用 lseek 改到中间再写，产生覆盖而非追加。vfstest 的 O_APPEND 用例只验证“打开即 EOF”，未拦截该偏差。现有用户程序不使用 O_APPEND，无功能影响。
- 建议: open_file 增记 append 标志，fs_write_h 入口对 append 句柄先 `off = ip->size`；或在头文件/注释中显式声明本系统 O_APPEND 仅为初始定位。

### FINDING-6: wait4 指定非本进程子 pid 时，只要还有其他存活子就永久阻塞（应 ECHILD）
- Severity: minor
- 位置: [proc.c:92-118](file:///Users/mingming/project/fakeos/kernel/proc.c#L92-L118)
- 问题: 未命中 record 后用 `sched_has_live_child(parent)`（任意子）决定是否继续等，不区分 want。`wait4(999,...)` 在父进程恰有另一个存活子（如 shell 在外部命令运行期间被异常代码调用）时会 yield 轮询到天荒地老；POSIX 语义为 ECHILD。WNOHANG 路径同样误返回 0 而非 -1。
- 建议: want != (u32)-1 时改为“存在 pid==want 且 ppid==parent 的存活子”判断；无此子、无此 record 即 -1。

### FINDING-7: console read 先阻塞收行、后校验用户缓冲，坏指针要赔进一行输入
- Severity: suggestion
- 位置: [tty.c:84-111](file:///Users/mingming/project/fakeos/kernel/tty.c#L84-L111)
- 问题: `tty_line_len==0` 时先 tty_recv_line() 阻塞收满一行，第 99 行才做 as_user_range_ok。坏 ubuf 的 read(0) 会先吃掉一行输入再返回 -1（行内容保留可重读，语义尚可，但阻塞时序与 Linux 相反）。
- 建议: 入口先按 len 做 range 校验再收行。全局单行缓冲的单读者前提已在 Assumptions 声明，无需改动。

### FINDING-8: execve 对 argv 向量固定要求 72 字节可读，短向量落在 VMA 末端会被误拒
- Severity: suggestion
- 位置: [uproc.c:255-275](file:///Users/mingming/project/fakeos/kernel/uproc.c#L255-L275)
- 问题: 即使 argv[1] 即为 NULL（argc=1），仍要求 `argv_u` 起 8*(8+1) 字节全部在同一 VMA。字符串已做到按页校验（sysfile.c:150-176，修复了栈顶窗口问题），向量表反而是固定窗口。in-tree 程序栈 VMA 宽裕不受影响，仅对紧贴映射区尾部手工构造 argv 的程序过严。
- 建议: 向量指针也逐元素按页 range 校验，遇 NULL 即停。

### FINDING-9: wait4 向 status 写 8 字节，POSIX status 为 int（4 字节）
- Severity: suggestion
- 位置: [proc.c:102-105](file:///Users/mingming/project/fakeos/kernel/proc.c#L102-L105)
- 问题: `*(u64*)status_u = code<<8`。现有调用方（init.c:36、sh.c:128）声明的都是 `long status`，当前无破坏；但后续若按 C 惯例传 `int*`，高 4 字节零会覆盖相邻栈槽。
- 建议: 按 4 字节写（u32），range 校验 8 字节可保留或改为 4。

### FINDING-10: 测试脚本最坏等待窗口合计约 17s，与 FR-14“总等待窗口不超过 8s”不符
- Severity: suggestion
- 位置: [scripts/test_qemu.py:65](file:///Users/mingming/project/fakeos/scripts/test_qemu.py#L65)（8s）、[:89](file:///Users/mingming/project/fakeos/scripts/test_qemu.py#L89)（12×0.5s）、[:92](file:///Users/mingming/project/fakeos/scripts/test_qemu.py#L92)（3s）
- 问题: 功能与稳定性无问题（5 连跑全绿），仅最坏预算与 FR-14 文案不一致；实测命令间隔 0.5s 偏保守。
- 建议: 将每条注入后的等待改为“读到下一个提示符即续跑”，可把总窗口压到 8s 内；或回填修订 FR-14 措辞。

### FINDING-11: 打开标志只记录 writable，O_WRONLY 句柄仍可 read；getdents 缓冲 <48B 时返回 0 与 EOF 不可区分
- Severity: suggestion
- 位置: [ramfs.c:446-477](file:///Users/mingming/project/fakeos/fs/ramfs.c#L446-L477)（无 readable 位）、[ramfs.c:535-560](file:///Users/mingming/project/fakeos/fs/ramfs.c#L535-L560)（fs_read_h 不查访问模式）；getdents [ramfs.c:634-666](file:///Users/mingming/project/fakeos/fs/ramfs.c#L634-L666)
- 问题: 以 O_WRONLY 打开的文件/目录仍能 read/getdents 成功（POSIX 应 EBADF/EINVAL）；len<48 时 fs_getdents_h 产出 0，用户态无法区分“缓冲太小”与“目录结束”。现有程序均以 O_RDONLY 读、缓冲 1920B，无实际触发。
- 建议: open_file 增存 access mode 并在 read/write 分派时校验；getdents 在 len<48 且目录非空时返回 -1。

## 深度审查结论（竞态/内存/资源/语义/构建）

- 竞态时序：单核 + 无阻塞态模型下，tty 轮询每次无字符必 yield、wait4 yield 轮询、monitor 计数均无共享数据竞争；syscall 帧随内核栈旅行且 fork 整帧拷贝、execve 仅在 commit 后改写本帧 rip/usersp（帧在共享高半内核栈，旧 as 销毁不影响），抢占点分析安全。唯一逻辑缺陷为 FINDING-1 的 reparent 参数。
- 内存安全：copy_user_cstring 按页校验 + demand paging、I/O 4096 分块、getcwd/getdents/argv/envp 全部过 as_user_range_ok，主干正确；问题集中在 u64 溢出回绕（FINDING-2）与 ELF 边界（FINDING-3）。COW refs 在 cow_copy_pt/release_pt 配对正确，as_destroy/execve 释放旧空间不误伤共享帧（ref>1 仅递减）。
- 资源泄漏：fd/file 引用（fork retain、close、exit close_all、console permanent no-op）平衡；TCB DEAD 槽随 wait reap；ramfs 动态文件扩容 kfree 旧缓冲；静态 inode/打开表为有界静态分配。泄漏仅在 FINDING-1 的孤儿路径出现。
- 语义：lseek 负偏移因 u64 模算恰好等价于减法且超界被 FILE_MAX 挡（目录无上限检查但 cast 后安全）；getdents offset 挂 open file、fork 后共享符合 POSIX；偏差为 FINDING-4/5/6/11。execve 失败路径在任何提交前返回，旧映像/fd/cwd 完整，nosuchcmd 实测证实。
- 构建：多 ELF 模式规则（crt0+usys+libc+prog.o，依赖 user.ld）、blob.o 依赖 5 ELF+motd、内核 OBJS 含全部新文件；touch user/hello.c 实测传播链完整。

## Review History

R1: Result **pass**（12 条 rule AC 全部 pass，rubric AC-13 = 4/5 达到阈值；make test 5 连跑全绿；128M/512M/2G 三档冒烟由评审员独立验证）。checkpoint 统计：13/13 通过（12 rule + 1 rubric）。发现问题 11 个：blocker 0、major 3（FINDING-1/2/3）、minor 3（FINDING-4/5/6）、suggestion 5（FINDING-7/8/9/10/11）。建议在阶段五自举前优先修 3 个 major（均为 FR-7/FR-5/NFR-4 的边界硬化，不影响当前演示与自动化回归）。
