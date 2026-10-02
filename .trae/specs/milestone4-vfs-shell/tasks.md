# fakeos 阶段四：VFS / Ramfs / POSIX / TTY / libc / Shell — 实施计划

> AC 映射见 [spec.md](./spec.md)。任务按依赖有序排列；内核侧切片先于用户态落地。
> 关键基准地址（实现修正）：heap `0x02000000` 预留 1MiB；mmap 区 `0x10000000` 起向上（上限 1 GiB，避开 8 MiB 处的用户栈）；用户栈保持 `[0x7fc000,0x800000)`。

## Task 1: fs 包：VFS 对象模型与 Ramfs 实现 + 内核自测
- **Status**: `completed`
- **Priority**: high
- **Depends On**: None
- **Description**:
  - 新建 `fs/ramfs.c`（`package fs`，单文件承载 VFS 抽象 + Ramfs 实例，避免跨文件同包耦合）：静态 inode 表（64 槽）、inode 类型（常规/目录/字符设备）、每目录 32 项 dentry（name[32]+ino）、常规文件数据（静态：const 数据指针+大小，只读；动态：kmalloc 缓冲+容量，2 倍扩容上限 1MiB）。
  - 打开文件表：静态 64 项 `{ino, offset, refs, flags}`；file 句柄为表内编号（0 保留为非法）。
  - 路径解析：仅接受内核态绝对路径；折叠多斜杠、`.`/`..`；组件非目录/过长/不存在返回 0。
  - API（句柄风格，对齐 as_h 惯例）：`fs_init`、`fs_root_ino`、`fs_publish_file(path,kva,size)`（逐级建目录 + 挂静态只读文件）、`fs_lookup(path)`、`fs_create(path)`（动态可写文件，父目录须存在）、`fs_mkdir(path)`、`fs_file_open(path,flags)`→file 句柄、`fs_close`、`fs_read_h/off`、`fs_write_h`、`fs_lseek_h`、`fs_getdents_h(ino, kbuf, len, *pos)`（定长记录：ino(8)/off(8)/reclen(2)/type(1)/name[27]，48B/条，跳过 . 与 ..）、`fs_ino_size/type/data_h`。
  - console 特殊 inode：预注册一个字符设备 inode（不挂目录），file 层另开 `fs_console_open_file()` 供 fd 0/1/2 绑定；read/write 行为由 Task 4/3 经 type 分派（fs 层提供 `fs_file_is_console(fh)` 判定）。
  - 新建 `kernel/vfstest.c`（`package kernel`）`vfs_selftest()`：根查找、多级 publish 与查找、mkdir、动态文件写→读→seek→扩容、目录枚举含/不含、6 个负面用例（不存在、组件是文件、名字过长、写只读静态文件、容量上限、句柄越界），打印三行 `[PASS] VFS path resolution` / `[PASS] Ramfs file create/read/write/seek` / `[PASS] Ramfs directory listing`。
- **Acceptance Criteria Addressed**: AC-1
- **Test Requirements**:
  - `rule` TR-1.1: `make` 编译通过；QEMU boot 输出三行 VFS/Ramfs PASS 且无 FAIL；负面用例均返回失败且系统继续运行（证据：boot 串口输出）
  - `rule` TR-1.2: 静态文件写保护生效：对 publish 的只读 inode 任何 fs_write 返回失败且不修改原 incbin 数据（证据：自测中写静态文件后读回内容不变的断言行）
- **Notes**: 分派一律 type 字段 + if 链，不使用结构体函数指针；fs 可依赖 mem 包的 kmalloc（slab）与 kprintf，不依赖 kernel 包（防包环）。

## Task 2: 初始镜像发布表（路径→incbin 符号）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 1
- **Description**:
  - 在 fs 包内新增发布表数据结构与 `fs_publish_rootfs()`：表项 `{path, start*, end*}` 由链接符号填充；符号在 Task 9 随 `user/blob.asm` 落地。
  - 本任务先用 kernel 侧自测临时占位验证发布循环（例如在 vfstest 中 publish 两条已知字符串常量到 /tmp/x 与 /etc/y），Task 9 替换为真实 6 文件表；发布函数幂等（重复路径失败不崩）。
  - 发布后统计并逐行打印：`[FS] published <path> (<size> bytes)`。
- **Acceptance Criteria Addressed**: AC-2（联调满足于 Task 9/10）
- **Test Requirements**:
  - `rule` TR-2.1: 发布循环对缺失符号（0 长度）安全跳过并计数；自测临时文件 lookup 成功且内容一致（证据：boot 输出 published 行 + TR-1.1 自测通过）

## Task 3: 每进程 fd 表 + 文件类系统调用（open/close/read/write/lseek）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 1
- **Description**:
  - `kernel/sched.c` TCB 扩展：`u32 fd[16]`（file 句柄，0xFFFFFFFF 空槽）、`char cwd[64]`（初值 "/"）、`u32 ppid`；`kthread_create_user` 初始化 cwd="/"、ppid=0、fd 0/1/2 绑定 console file（经 fs_console_open_file 三次 refs 独立或共享同一 fh——采用共享同一 file 句柄，offset 对 console 无意义）。
  - 新建 `kernel/sysfile.c`：用户路径有界拷贝（128B 栈缓冲，range 校验）、open 标志（0/1/2/0x40/0x200/0x400）、fd 分配/释放、read/write/lseek 经 file 句柄转 fs；console file 经 fs_file_is_console 走 UART（write 复用现有 uart_putc 循环；read 接 Task 4 钩子，本任务先返回 0）。
  - 相对路径：以 TCB cwd 拼接到内核路径缓冲后交 fs_lookup（拼接收尾在 Task 8 前可仅支持绝对路径，预留 helper）。
  - `syscall.c` 新增号：read=0、open=2、close=3、lseek=8；SC_WRITE 改走 fd→file 分派（fd 1/2 console）；保留未知号拒绝。
  - fork 继承：`sched_clone_user` 复制父 fd 数组与 cwd，对每个非空 fd 调 fs 引用计数 +1（新增 `fs_file_retain(fh)`）；设置子 ppid=父 pid。
  - 进程退出路径 `proc_terminate`：关闭全部 fd（fs_close 减引用）后再毁地址空间。
- **Acceptance Criteria Addressed**: AC-3、AC-9（部分，端到端证据在 Task 10/11）
- **Test Requirements**:
  - `rule` TR-3.1: `make` 通过；阶段三既有序列（write(1) 走新分派）输出字节完全一致（证据：make test 在本任务后仍保持阶段三全绿——此时用户态仅 write 相关）
  - `rule` TR-3.2: 坏 fd/坏路径/用户指针越界返回 (u64)-1，内核无异常（证据：Task 10 init 负面用例 PASS 行；本任务先以内核代码走查记录，端到端在 Task 11 复核）

## Task 4: 串口 TTY 标准输入（canonical 行缓冲）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 3
- **Description**:
  - `drivers/uart.c` 增 `uart_getc_nonblock(void)→int`（LSR bit0 判定，无字符返回 -1）。
  - 新建 `kernel/tty.c`：全局单行缓冲 256B + len；`tty_read(user 无关，内核侧产出 kbuf)`：无完整行时循环（取字符→回显；遇 `\n` 收行并回 `\r\n`；0x7F/0x08 非空则删一字符并回显 `\b \b`；缓冲满 255 强制收行），无字符时 `sched_yield()`）；按调用 len 返回行内字节切片，残余保留。
  - console read 分派接入 tty_read（sysfile.c 的 console 分支），先拷到内核小缓冲再 copy_to_user。
  - console write 路径不变（UART）。
- **Acceptance Criteria Addressed**: AC-4（端到端证据在 Task 11）
- **Test Requirements**:
  - `rule` TR-4.1: 无输入时系统不挂死：轮询路径每次无字符必 yield，其他线程（含定时器抢占）照常推进（证据：Task 11 shell 等待注入期间内核 monitor/定时器活动正常、最终收到注入行）
  - `rule` TR-4.2: 退格与回显处理正确（证据：Task 11 注入含退格序列的一行，最终执行命令反映删除后的词；若脚本注入退格不稳定则以代码走查 + 手工冒烟记录为准）

## Task 5: brk 与匿名 mmap（as 层 + syscall）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 3
- **Description**:
  - `mem/as.c`：`struct address_space` 增 `u64 brk_base, brk_cur, mmap_next`；常量 HEAP_BASE=0x02000000、HEAP_SIZE=0x100000（1MiB 预留 VMA）、MMAP_BASE=0x70000000；首次 brk 调用时 as_map_anon 预留堆区并令 brk_cur=brk_base；`as_brk_h(h,newbrk)`：参数 0 表示查询；向上不超过 base+size、向下不低于 base；实际仅移动边界（按需分页由 #PF 完成），返回当前 brk。
  - `as_mmap_anon_h(h,len,prot)`：长度向上页对齐，从 mmap_next 预留匿名 VMA 并推进 hint（贴近 0x800000 栈区下沿即停，失败 0），返回首址。
  - COW clone/ destroy 已按 VMA 泛化，新字段随 vma 复制无需特殊处理（核对 clone 后 brk/mmap 字段复制：在 as_cow_clone 复制元数据）。
  - syscall：brk=12（rdi）、mmap=9（rdi addr 忽略，rsi len，rdx prot，r10 flags，r8 fd 须 -1，r9 off 须 0）；仅 MAP_ANONYMOUS(0x20)|MAP_PRIVATE(0x02)；prot 1/2/4 直传 VMA。
- **Acceptance Criteria Addressed**: AC-7
- **Test Requirements**:
  - `rule` TR-5.1: 用户态 init 哨兵写读通过，输出 `[U][PASS] brk heap malloc` 与 `[U][PASS] anonymous mmap`（证据：Task 11 make test）
  - `rule` TR-5.2: 越界 brk（超 1MiB）、非匿名/非私有 flags、len=0 返回 -1 且不建 VMA（证据：init 负面用例输出；VMA 槽数不被无效调用消耗，自测通过后进程仍可正常 mmap）

## Task 6: 退出记录表与 wait4（含 ppid）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 3
- **Description**:
  - 新建 `kernel/proc.c`（或并入 sched.c，优先独立文件）：静态 `exit_record[32]{used,pid,ppid,code}`；`proc_record_exit(pid,code)`（proc_terminate 调用）；`sys_wait4(pid,status_u,options)`：遍历记录找当前 pid 的子（pid==-1 任意；指定 pid 精确匹配）；命中则写 `(code&0xff)<<8` 到用户 status（range 校验，允许 0）、清记录、返回 pid；未命中且确有存活子：options&WNOHANG(1)→返回 0，否则循环 `sched_yield()` 等待；无子返回 (u64)-1（ECHILD 语义）。
  - ppid 在 Task 3 已接线；核对 init 两轮 fork 的子 ppid=1。
  - syscall 号 wait4=61；`usys`/libc 在 Task 9 暴露。
- **Acceptance Criteria Addressed**: AC-6（部分，端到端在 Task 11）
- **Test Requirements**:
  - `rule` TR-6.1: 子先退出记录可被父 wait 回收，返回子 pid、status=(42<<8)；记录回收后槽位可复用（证据：Task 11 echo $? = 42 与两轮 COW 子回收正常）
  - `rule` TR-6.2: 无子 wait4 立即返回 -1；WNOHANG 对存活子立即返回 0（证据：init/shell 自测输出或内核自测打印，Task 11）
  - `rule` TR-6.3: 等待期间为内核态 yield 轮询，子进程调度与执行不受阻（证据：hello 命令在 shell wait 期间实际运行并输出）

## Task 7: execve（文件 ELF 装载 + 映像替换 + argv 栈）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 2, Task 5, Task 6
- **Description**:
  - `kernel/uproc.c` 重构：抽出 `load_elf_into(as_h, kbuf,size)→entry`（由现有 load_elf 主体改造，失败 0）；新增 `build_user_stack(as_h, argc, argv_k[])→sp`：在 [0x7fc000,0x800000) 顶端自顶向下拷字符串（含 \0）、16B 对齐后布局 `[argc][argv*…][NULL][NULL(envp)]`；需要 as 层新原语：`as_force_page_h(h,uva)`（resolve+分配+映射用户 RW 页，不经 #PF）与 `as_copy_in_h`（逐页 translate/force 后经 HHDM 写入）——在 `mem/as.c` 新增，供 exec 与新进程栈构造使用（首次 init 启动同样走它，替代手工预留）。
  - `sys_execve(path_u, argv_u, envp_u)`（号 59，sysfile.c 或新 sysproc.c）：拷路径；fs_lookup 取文件（须常规文件），读数据 kva/size；解析 argc（上限 8，每个串 ≤127B，argv_vec 用户指针逐个 range 校验并拷字符串到内核临时数组；envp 必须为 0 或首项为 0，否则拒绝）。
  - 建 new_as→load_elf_into→预留栈 VMA→build_user_stack；失败路径销毁 new_as 返回 -1；成功后：保留当前 TCB（fd/cwd/ppid 不动），记录旧 as_h，更新 tcb.as_h、as_activate(new)、销毁旧 as；改写当前 128B syscall 帧：rip 槽=entry、帧尾 user rsp 槽=new sp、rax 槽=0（参照 sysentry.asm 布局 [104]=rcx/rip、[120]=user rsp、[0]=rax）；正常返回即从新映像执行。
  - pid 1 初始启动改走同一装载器：uproc_selftest 从 fs 取 /bin/init 数据（Task 10 接线），argv={"/bin/init"}。
- **Acceptance Criteria Addressed**: AC-5
- **Test Requirements**:
  - `rule` TR-7.1: init execve 到 sh 后自报 pid=1、cwd 与 fd 0/1/2 保持可用（证据：Task 11 输出）
  - `rule` TR-7.2: argc/argv 正确：外部命令打印 argv[0] 与参数（证据：Task 9 给 hello 增加可选 argv 打印或 cat 参数行为正确；Task 11 `cat /etc/motd` 打开的是参数路径而非固定路径）
  - `rule` TR-7.3: 坏路径/坏 ELF/超长 argv 返回 -1 且当前映像与 fd 完好（证据：nosuchcmd 后新提示符出现；init 中对坏 ELF exec 的负面用例 PASS 行）

## Task 8: 目录与进程环境系统调用（mkdir/chdir/getcwd/getdents64）
- **Status**: `completed`
- **Priority**: medium
- **Depends On**: Task 3
- **Description**:
  - syscall：mkdir=83、chdir=80、getcwd=79、getdents64=217。
  - `sys_chdir`：相对/绝对路径解析后要求 ino 为目录，拷贝规范化绝对路径到 TCB cwd（相对拼接在本任务完善：单一内核 helper `resolve_cwd(cwd, u_path, k_out)`，支持 . / .. 折叠与长度限制）。
  - `sys_getcwd(buf,len)`：拷 cwd（含 \0），len 不足返回 -1。
  - `sys_getdents(fd,buf,len)`：fd 须指向目录文件；fs_getdents_h 产出 48B 定长记录到内核缓冲再 copy_to_user；维护目录文件 offset（u64 条目序号）；返回写入字节数，末尾返回 0。
  - open 相对路径支持（cwd 拼接）；mkdir 相对路径支持。
- **Acceptance Criteria Addressed**: AC-8
- **Test Requirements**:
  - `rule` TR-8.1: `ls /bin` 记录含 sh/init/hello/cat/ls 五项；`mkdir /home`+`cd /home`+`pwd` 输出 /home，`cd ..` 回 /（证据：Task 11 会话断言）
  - `rule` TR-8.2: 对文件 fd 调 getdents、对文件路径 chdir、重复 mkdir 均返回 -1 且 shell 存活（证据：Task 11 注入重复 mkdir 得到错误提示后提示符再现）

## Task 9: 用户态构建管线：crt0、libc、5 个 ELF、多 blob 与 Makefile
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 4, Task 5, Task 6, Task 7, Task 8
- **Description**:
  - `user/crt0.asm`：global `_start`；`mov rdi,[rsp]`（argc）、`lea rsi,[rsp+8]`（argv）、`call main`、`mov rdi,rax` 调 u_exit。
  - `user/usys.asm` 扩展全部包装：read(0)/write(1)/open(2)/close(3)/lseek(8)/mmap(9)/brk(12)/sched_yield(24)/getpid(39)/fork(57)/execve(59)/exit(60)/wait4(61)/chdir(80)/mkdir(83)/getcwd(79)/getdents64(217)。
  - `user/libc.c`（package user）：memcpy/memset/memmove/strlen/strcmp/strncmp/strcpy/strncpy/strchr/strdup(用 malloc)/strtok_r 风格分词；mini_printf（%s/%d/%u/%x/%c/%%，返回写字节数）+ printf；malloc/free（brk bump，每块头 16B {next,size}，free 挂链，首次合并非必需）。
  - 程序：`user/init.c` 重写为 main——阶段三序列（hello、3M 忙等、两轮 fork COW 打印既有全部 [U]/[U][PASS] 行）+ 文件测试（O_CREAT 写/lseek 0/读回/追加 + 三个负面用例，打印 `[U][PASS] vfs file write/read/seek`）+ brk/malloc 哨兵（`[U][PASS] brk heap malloc`）+ mmap 两页哨兵（`[U][PASS] anonymous mmap`）+ getdents("/bin") 校验含 sh（`[U][PASS] vfs getdents`）+ 坏 ELF exec 负面用例，最后 execve("/bin/sh",{"/bin/sh",0},0)。
  - `user/sh.c`：提示符 `fakeos:~$ `；read 一行→分词（≤8 词）；内建 echo（`$?` 展开）、cd、pwd、exit[code]、help；外部命令：先 "/bin/<name>" 再 cwd/<name> 查找；fork→子 exec（失败打印 `sh: <name>: command not found` 并 exit 127）→父 wait4（打印下一个提示在 wait 之后；记录 rc 供 $?）。
  - `user/hello.c`（固定输出 `hello from /bin/hello`、打印 argv[0] 与 pid、exit 42）、`user/cat.c`（逐参数 open/read/write 4KiB 循环）、`user/ls.c`（getdents 格式化，默认 cwd）、`user/motd.txt`（欢迎文本 ≥20B）。
  - `user/user.ld` ENTRY 改 _start（所有程序共用此 ld）；`user/blob.asm` 扩为 6 对符号 incbin（build/user/{init,sh,hello,cat,ls}.elf + motd 直接 incbin txt）。
  - Task 2 发布表在本任务接真实符号（fs 包或 kernel 侧表经 fs_publish_file 逐项发布；表放 `kernel/rootfs.c` 引用 blob 符号，避免 fs 包依赖具体镜像）。
  - Makefile：user 下多 ELF 模式规则（每 ELF = crt0.o + libc.o + prog.o，-T user/user.ld）；blob.o 依赖 5 ELF + motd；新增 fs.o、vfstest.o、sysfile.o、tty.o、proc.o、rootfs.o 进 OBJS。
- **Acceptance Criteria Addressed**: AC-2、AC-3、AC-7、AC-9、AC-10（用户态部分）
- **Test Requirements**:
  - `rule` TR-9.1: `make clean && make` 全管线通过，5 ELF 均为 x86-64 静态 ET_EXEC、入口 _start（证据：make 输出 + `objdump -f`/readelf 抽查记录）
  - `rule` TR-9.2: init 全部 [U][PASS] 行（COW×2 轮 4 行 + vfs/brk/mmap/getdents/坏 ELF）在 boot 中出现且随后 shell 启动（证据：QEMU 输出）

## Task 10: kmain/uproc 启动序列接线与监控重构
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 9
- **Description**:
  - `kmain.c`：slab 初始化后调 `fs_init()` + `rootfs_publish()` + `vfs_selftest()`；uproc_selftest 改为：从 fs 查 /bin/init 取数据→load_elf_into→建栈(argv=/bin/init)→创建 pid 1 单实例；启动监控线程。
  - 监控逻辑重构：等待 init 的 2 个 COW 子退出（计数达 2 或沿用 exits 口径 4：包含后续 shell 子进程计数——以"2 个测试子退出"为阶段三判定门，打印 Ring-3/SYSCALL/COW/抢占 PASS 文案更新为 1 实例 2 子）；随后等待 pid 1 退出（shell exit）或 30s 兜底，放行 kmain 打印新 SUCCESS 语 `[SUCCESS] fakeos: VFS, Ramfs, POSIX syscalls, TTY input, libc and interactive shell online!`；banner 副标题更新为 Milestone 4。
  - pid 1 退出码透传日志保留（code 7 可见）。
- **Acceptance Criteria Addressed**: AC-5、AC-10、AC-11（内核编排部分）
- **Test Requirements**:
  - `rule` TR-10.1: boot 自动跑完内核与 init 自测到达 `fakeos:~$ `；注入 exit 7 后出现 pid 1 exited (code 7) 与新 SUCCESS 语（证据：Task 11 make test）
  - `rule` TR-10.2: 无注入时系统在 shell 提示符处稳定停住（30s 兜底后仅 kmain 收尾 SUCCESS 打印、shell 仍可读输入——核对兜底只解除监控等待而不杀 pid1；手工冒烟记录）

## Task 11: test_qemu.py 交互式回归改造 + 连跑稳定性
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 10
- **Description**:
  - 脚本改 stdin=PIPE：启动后轮询/固定等待至出现首个 `fakeos:~$`（分段读输出，线程或 select；macOS 无 selectors 限制时用短超时读循环），随后注入命令流（每条间隔 0.3s）：`hello`、`echo $?`、`echo INPUT123`、`cat /etc/motd`、`mkdir /home`、`cd /home`、`pwd`、`cd ..`、`ls /bin`、`mkdir /home`（重复，错误路径）、`nosuchcmd`、`exit 7`；等待 SUCCESS 或 6s 超时后 terminate。
  - 断言集：保留阶段三全部仍适用项（启动/异常/内存/sched/COW/ring3/COW fork/ring3 抢占行），新增：VFS 三行 PASS、published 清单六路径、[U][PASS] vfs/brk/mmap/getdents 行、提示符、hello 输出+pid、42、INPUT123、motd 文本（取 motd 实际首行）、/home、ls 五项、not found、pid 1 exited (code 7)、新 SUCCESS 语。
  - 更新 Ring-3 entry 文案断言（1 实例 2 子的新文案）。
- **Acceptance Criteria Addressed**: AC-4、AC-6、AC-8、AC-11、NFR-1
- **Test Requirements**:
  - `rule` TR-11.1: `make test` 退出码 0，全部断言匹配（证据：脚本输出）
  - `rule` TR-11.2: 连跑 5 次均退出码 0、无新增 flaky（证据：5 次运行结果记录）

## Task 12: 多档内存冒烟、README 里程碑更新、代码质量收尾
- **Status**: `completed`
- **Priority**: medium
- **Depends On**: Task 11
- **Description**:
  - 手工冒烟 -m 128M/512M/2G：启动→简单命令→exit，记录 SUCCESS。
  - README：Mermaid M4 标签更新、阶段四各勾选条目中 VFS/Ramfs/POSIX/init/libc/Shell 标记完成并补详细描述；TTY/键盘条目注明"串口 TTY 先行，PS/2 键盘后续"。
  - 全仓扫调试残留（[SC]/dbg_/临时打印）、核对 fakecc 约束（无预处理器指令、-O0、句柄 API）、git 阶段四提交（不 push）。
- **Acceptance Criteria Addressed**: AC-12、AC-13、NFR-2、NFR-3、NFR-4
- **Test Requirements**:
  - `rule` TR-12.1: 128M/512M/2G 三档冒烟均见 shell 提示符与 SUCCESS（证据：输出摘录）
  - `rubric` TR-12.2: 代码质量自查；维度=与既有风格/fakecc 约束/资源纪律的契合；scale 1-5；anchors 1/3/5 同 AC-13；threshold >= 4；证据=新增文件通读与 grep 检查记录（最终以独立评审为准）

## Completion Evidence（实施汇总）

- **Task 1**：`fs/ramfs.c`（inode/dentry/打开文件表/静态+动态文件/路径解析/getdents）+ `kernel/vfstest.c`；修复根 inode 必须固定槽 1（alloc 从 2 起）的初始化 bug；boot 输出三行 PASS、全部检查 ok。
- **Task 2**：发布能力经 vfstest 多级 publish 验证；真实发布表随 Task 9 `kernel/rootfs.c` 落地（避免未定义符号中间态）；`[FS] published <path> (<n> bytes)` 六行齐全（修正 kprintf 不支持 `%-14s` 宽度修饰）。
- **Task 3**：TCB 增 fd[16]/cwd/ppid；`kernel/sysfile.c` 实现 open/close/read/write/lseek 与分块用户缓冲；fork 继承 fd（fs 引用计数）、退出统一关闭；阶段三 write 路径经新分派全绿。
- **Task 4**：`drivers/uart.c` 非阻塞接收 + `kernel/tty.c` canonical 行缓冲（回显/退格/CR+LF/yield 轮询/残余切片）；端到端经 shell 注入证实。
- **Task 5**：as 层 brk_base/brk_cur/mmap_next + `as_brk_h/as_mmap_anon_h/as_force_page_h/as_copy_in_h`；**修正设计错误**：mmap 区初版 0x70000000 与 8MiB 栈限制自相矛盾，改为 0x10000000 向上、1 GiB 上限。
- **Task 6**：`kernel/proc.c` 退出记录表 + wait4（轮询阻塞/WNOHANG/ECHILD，status=(code&0xff)<<8）；修复 DEAD 槽从不回收的资源泄漏（wait 时 reap + 孤儿 reparent pid 1）。
- **Task 7**：ELF 装载器重构为 load_elf_into + proc_build_image + SysV argv 栈；sys_execve 同 TCB 换映像（帧 rip/usersp 改写，RAX=0）；argv/路径用户指针按页校验（修复栈顶 argv 字符串 128B 窗口越界 VMA 导致 exec 后 open 失败）。
- **Task 8**：mkdir/chdir/getcwd/getdents64 + 词法 canonical 相对路径（`.`/`..`/折叠/上界检查）。
- **Task 9**：crt0、usys 全套（**修复 u_mmap 第 4 参 RCX 被 SYSCALL 摧毁，显式 mov r10,rcx**）、libc（string/mem/printf/brk first-fit malloc）、init/sh/hello/cat/ls、motd、多 blob、Makefile 多 ELF 模式规则；5 ELF 均静态 ET_EXEC、入口 _start。
- **Task 10**：rootfs_publish 接线；pid 1 单实例从 /bin/init 装载（argv=/bin/init）；两阶段监控（2 COW 子 → 阶段三 PASS；pid 1 退出或 30s 兜底放行，手工 shell 不被杀）；Milestone 4 banner/SUCCESS。
- **Task 11**：test_qemu.py stdin 注入 + selectors 等提示符 + 12 命令会话；70 个断言/上下文检查点（63 子串 + 7 上下文）；连跑 5 轮（实测 10 次）全绿、无新增 flaky；CRLF 规范化处理行尾。
- **Task 12**：128M（test）/512M/2G 三档冒烟均到 shell 且 exit 7 后 SUCCESS；README Mermaid 与阶段四全部条目标注（PS/2 键盘明确推迟）；grep 确认无 DBG/dbg 残留、无预处理器违规；`make clean && make` 零错误。

