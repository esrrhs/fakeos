# fakeos 阶段四：VFS / Ramfs / POSIX / TTY / libc / Shell — 产品需求文档

## Overview
- **Summary**: 在已具备 Ring-3 进程、COW fork 与 SYSCALL/SYSRET 的 fakeos 上，一次性补齐从虚拟文件系统到交互式 Shell 的完整用户空间链路：VFS 抽象层 + Ramfs、POSIX 系统调用集（文件/进程/内存）、串口 TTY 标准输入、自研极简用户态 libc，以及可执行外部命令的交互式 shell；内核启动后由 pid 1（/bin/init）完成用户态回归并 `execve` 到 /bin/sh，串口终端可登录式交互。
- **Purpose**: 让 fakeos 从"能跑内嵌测试程序"的内核进化为"有文件、有程序、有 shell"的可用操作系统环境，为阶段五（fakecc 自举）提供文件 I/O、进程执行与用户态 C 运行时基础。
- **Target Users**: fakeos 开发者与 CI（无头 QEMU 串口）；后续为 fakecc 自举提供运行环境。

## Goals
- 现代 VFS 对象模型：inode / 目录项 / 打开文件表 / 挂载点，文件系统以操作分派表接入。
- Ramfs：支持目录与常规文件，初始文件（用户 ELF、文本）经 incbin 随内核镜像发布，启动时释放到根文件系统。
- POSIX 风格系统调用：`read/write/open/close/lseek/brk/mmap/fork/execve/wait4/exit/getpid/yield/mkdir/chdir/getcwd/getdents64`。
- 每进程文件描述符表：fork 继承、execve 保留；fd 0/1/2 接串口控制台（输出 UART，输入 canonical 行缓冲）。
- 进程生命周期补全：父子关系、退出状态记录（zombie 数据）、`waitpid` 回收与阻塞等待、`execve` 映像替换（pid/fd 不变，argv 入栈）。
- 用户态 libc：syscall 包装、string/mem、printf、brk bump malloc；crt0 按 SysV 栈约定传 `main(argc, argv)`。
- 交互式 /bin/sh：提示符、内建命令（echo/cd/pwd/exit/help/$?）、外部命令 fork+execve+wait、PATH 查找 /bin。
- 附带用户程序 /bin/hello、/bin/cat、/bin/ls 与 /etc/motd。
- 全部新功能有内核自测或用户态自测证据；`make test` 经串口 stdin 注入完成 Shell 端到端自动化回归；阶段三既有断言全部保持。

## Non-Goals
- PS/2 键盘硬件扫描码驱动（README 该项保留未勾选；本阶段标准输入只走 COM1 串口）。
- 管道（pipe）、shell 重定向（`<`/`>`）、作业控制、进程组/会话/信号、dup/dup2。
- 文件-backed mmap（本阶段仅匿名 MAP_PRIVATE；文件映射留待后续 FS 演进）。
- 磁盘文件系统（ext2 等）、块设备层、真实 initrd/initramfs 压缩格式；多挂载点。
- 权限/用户模型（uid/gid/模式位强制校验）、符号链接/硬链接、unlink（文件一经创建持续存在）。
- 环境变量持久化（execve 接收 envp 参数但透传空表）、ELF 动态链接（仅静态 ET_EXEC）。
- VGA 控制台与 shell 的集成（shell 仅串口；VGA 继续显示内核日志）。

## Background & Context
- 现状（阶段三末）：syscall 仅 write/getpid/yield/fork/exit；write 硬连 UART、无 fd；用户 ELF 单个 incbin 内嵌；ELF 装载器在 [uproc.c](file:///Users/mingming/project/fakeos/kernel/uproc.c)；进程退出即销毁地址空间，无退出状态留存；VMA 仅匿名段（[as.c](file:///Users/mingming/project/fakeos/mem/as.c)，`as_map_anon` / `as_map_preload`，64 VMA 槽）。
- 工具链约束：fakecc 无预处理器、无系统头文件，package 模块制，跨包以 u64 句柄传递对象；必须 `-O0 --target=x86_64-linux`；不支持内联汇编；为规避已知代码生成风险，跨对象操作分派沿用"type 字段 + if 链"风格，不依赖结构体函数指针调用。
- 测试基建：`scripts/test_qemu.py` 以 `-serial stdio` 无头启动、sleep 3s 后终止并做子串断言；本阶段需扩展为 stdin 注入命令流。
- 调度器现状：RR 抢占、无阻塞态；本阶段阻塞语义（wait4、read 输入）以"在内核中循环 `sched_yield()` 等待条件"实现，不引入 TCB_BLOCKED 状态。

## Functional Requirements
- **FR-1（VFS 核心）**：提供 inode（类型：常规文件/目录/字符设备）、目录项父子关系、绝对/相对路径逐级解析（`/` 分隔，`.`/`..` 解析，多分隔符折叠）、打开文件表（offset/refcount）、根挂载点；查找失败、路径过长、组件非目录等均有明确失败返回。
- **FR-2（Ramfs）**：常规文件支持读、按偏移写、末尾追加扩容（容量指数增长，上限 1 MiB/文件）；目录支持创建子目录与目录项枚举；初始静态文件数据指向内核 incbin 只读镜像（只读打开），运行时新建文件数据走 kmalloc/buddy 且可读可写。
- **FR-3（初始镜像发布）**：构建期把 5 个用户 ELF（/bin/init、/bin/sh、/bin/hello、/bin/cat、/bin/ls）与 /etc/motd 经 incbin 链入内核；启动 VFS 初始化时按路径表释放到 Ramfs（建目录、建文件、校验大小非零）。
- **FR-4（fd 表与文件 syscall）**：每 TCB 16 个 fd 槽；进程诞生时 fd 0/1/2 指向 console 打开文件；`open`（O_RDONLY/WRONLY/RDWR/CREAT/TRUNC/APPEND）、`close`、`read`、`write`、`lseek`（SET/CUR/END）语义正确；fork 复制 fd 表并对共享打开文件加引用；execve 保留 fd；close 后槽位可复用。
- **FR-5（用户指针安全）**：所有以用户地址为来源/目标的 syscall（路径、读写缓冲、argv/envp、getdents 缓冲、getcwd 缓冲）先经 `as_user_range_ok` 校验，再以固定上限（路径 128B、单次 I/O 受 len 与 range 校验约束）拷贝；非法指针返回 -1，不得内核态取指/访存崩溃。
- **FR-6（串口 TTY）**：console inode 的 read 走 COM1 接收：阻塞读取直到行结束（`\n`），canonical 缓冲 256B；输入字符回显（`\n` 回 `\r\n`），退格（0x7F/0x08）删字符并回显处理；一次 read 返回当前行中不超过 len 的字节（行内残余保留下次读取）；无输入时 `sched_yield()` 让出，不饿死其他线程。console write 维持现有 UART 输出与 `\n→\r\n` 行为。
- **FR-7（进程退出与回收）**：TCB 记录 ppid；exit 维护一张退出记录表（pid、退出码低 8 位、ppid），地址空间照常释放；`wait4(pid, status, options)`：子已退出则回收记录并经 status 回传 `(code&0xff)<<8`、返回 pid；子仍存活则阻塞（yield 轮询）；options 含 WNOHANG(1) 时立即返回 0；无子/无此子返回 -1。pid 1 退出后系统回到 kmain 收尾路径。
- **FR-8（execve）**：`execve(path, argv, envp)` 从 VFS 取 ELF，复用装载逻辑在**新地址空间**映射 PT_LOAD；保留同一 TCB（pid、ppid、fd 表、cwd 不变）；构造 SysV 用户栈（向上：argc、argv 指针数组、NULL、envp 仅一个 NULL 终止、16B 对齐），改写当前 syscall 帧的用户 RIP=e_entry、用户 RSP=新栈；成功不返回（用户从新映像 _start 执行，返回值 RAX=0）；失败（不存在/非 ELF/段映射失败）返回 -1，原映像继续运行。
- **FR-9（brk/mmap）**：`brk(new)` 在 ELF 段后的堆区（1 MiB 预留 VMA，按需分页）移动 program break，返回当前 break；`mmap(0,len,PROT_*,MAP_ANONYMOUS|MAP_PRIVATE,...)` 在低半固定 mmap 区（0x70000000 起向上）逐页对齐分配匿名 VMA 并返回首址；越界/参数非法返回 -1；`munmap` 非本阶段目标。
- **FR-10（目录 syscall）**：`mkdir(path)` 创建空目录；`chdir(path)`/`getcwd(buf,len)` 以每进程字符串 cwd（上限 64B）工作，相对路径 open/mkdir 基于 cwd 拼接解析；`getdents64(fd,buf,len)` 以定长记录（d_ino/d_off/d_reclen/d_type/d_name，跳过 "."/".."）枚举目录，返回字节数或 0。
- **FR-11（libc）**：用户态单一 `package user` 提供：全部上述 syscall 的汇编包装；memcpy/memset/memmove/strlen/strcmp/strncmp/strcpy/strncpy/strchr/strtok 风格分词；`mini_printf`（%s %d %x %c %%）；brk bump malloc/free（自由链表合并非必需，不泄漏即可）。
- **FR-12（crt0 与用户程序）**：每个用户 ELF 经 crt0 进入：从初始栈取 argc/argv 调 `main`，main 返回值作为 exit 码；init 顺序执行：hello/getpid 打印、3M volatile 忙循环、两轮 fork+COW 校验（阶段三断言行保留）、文件读写/lseek、brk malloc、匿名 mmap、getdents 校验后 `execve("/bin/sh",...)`；sh 提供提示符 `fakeos:~$ `、行编辑（canonical 在内核）、空格分词、内建 `echo`（含 `$?`）、`cd`、`pwd`、`help`、`exit [code]`；外部命令按 /bin 查找并 fork/exec/wait；hello 打印固定串与自身 pid；cat 顺次读文件并 write；ls 列目录（默认 cwd 与 /bin）。
- **FR-13（启动序列与监控）**：fs 初始化与初始文件释放先于用户进程；启动 1 个 /bin/init（pid 1，argv={"/bin/init"}）；内核监控线程等待 init 派生的 2 个 COW 测试子进程退出（共 4 次退出：2 子 + 后续 shell 外部命令不计此数）后打印阶段三 Ring-3/SYSCALL/COW/抢占 PASS 行；pid 1（exec 后为 sh）经 `exit` 内建退出后监控线程放行 kmain 打印 SUCCESS；设 30s 兜底超时放行，避免手工交互时永久阻塞内核收尾。
- **FR-14（回归与测试）**：test_qemu.py 保留阶段三全部仍适用断言，新增 VFS 内核自测、用户态文件/mmap 断言与 Shell 注入会话断言（ls /bin、cat /etc/motd、hello、cd/pwd、未知命令提示、exit）；脚本以 stdin 管道注入命令，时序为等待启动输出后注入、等待退出后收尾，总等待窗口不超过 8s。

## Non-Functional Requirements
- **NFR-1（可重复回归）**：`make test` 连跑 5 次全绿，不引入新的时序 flaky（既有 APIC 校准 35..75 tick 窗口保持）。
- **NFR-2（资源纪律）**：文件描述符、打开文件表项、Ramfs inode 均有界静态/可回收分配；进程退出关闭其 fd 并释放 file 引用；128 MiB 机型可完整跑完全部测试与 shell 会话；512 MiB / 2 GiB 机型手工冒烟 SUCCESS。
- **NFR-3（惯例一致）**：新代码遵循现有 package 布局与命名（snake_case、句柄化跨包 API、kprintf 中文/英文风格与现有自测一致），无调试探针残留，不引入新第三方工具链依赖（仅 nasm/ld/objcopy/fakecc/qemu/python3）。
- **NFR-4（故障隔离）**：用户态非法路径、坏 ELF、越界 fd、权限不符等一律 -1 或进程被猎杀，内核不死机、不挂死（除 read console 的预期阻塞外无新增不可恢复路径）。

## Constraints
- **Technical**: fakecc -O0 / 无预处理器 / 无内联汇编 / package 跨包句柄化；用户态与内核共用同一 fakecc 版本；ELF 仅静态 ET_EXEC、固定加载基址 0x400000；无阻塞等待队列原语（yield 轮询）。
- **Business**: 不更改 MIT 许可与现有 CI 工作流形态；`make`/`make test`/`make qemu` 入口保持兼容。
- **Dependencies**: 现有 buddy/slab/VMA/COW/调度/syscall 帧机制；QEMU 串口可作为 stdin/stdout。

## Assumptions
- 单 CPU、单一控制台（COM1）实例足以代表本阶段需求；多线程竞争同一 console read 的场景不出现（shell 等待子进程期间自身不读）。
- Ramfs 文件规模小（ELF 数 KiB 级，文本 <1 KiB），单文件 1 MiB 上限与总量静态 inode 64 个、目录项 32/目录 足够。
- shell 命令行长度 ≤255B、参数 ≤8 个、路径 ≤127B、cwd ≤63B 可满足本阶段全部演示与测试。
- argv 仅需 ASCII，参数与文件名字符集为可打印 ASCII 且不含空格以外的空白。

## Acceptance Criteria

### AC-1: VFS 与 Ramfs 内核自测通过
- **Type**: `rule`
- **Given**: 内核启动完成、fs 初始化释放完初始文件
- **When**: kmain 执行 VFS/Ramfs 自测序列（根目录查找、逐级路径解析、mkdir、文件创建/write/read/lseek/扩容、枚举目录项、负面用例）
- **Then**: 串口输出 `[PASS] VFS path resolution`、`[PASS] Ramfs file create/read/write/seek`、`[PASS] Ramfs directory listing`（或等效三行），无 FAIL
- **Pass Condition**: 三行 PASS 全部出现在 captured output
- **Evidence**: `make test` 输出

### AC-2: 初始镜像文件在 Ramfs 中可见且非空
- **Type**: `rule`
- **Given**: fs 初始发布完成
- **When**: 内核统计 /bin 与 /etc 下释放的文件
- **Then**: /bin/init、/bin/sh、/bin/hello、/bin/cat、/bin/ls、/etc/motd 均存在，大小与 incbin 符号区间一致（>0）
- **Pass Condition**: 内核启动日志含一行初始文件清单且 6 个路径全部出现；shell 会话中 `ls /bin` 输出含 sh/init/hello/cat/ls，`cat /etc/motd` 输出非空文本
- **Evidence**: `make test` 输出（清单行 + ls + cat 断言）

### AC-3: 用户态文件读写/lseek 往返正确
- **Type**: `rule`
- **Given**: pid 1 init 运行用户态自测
- **When**: 以 O_CREAT|O_RDWR 创建文件、写入已知模式字节串、lseek 回 0、读回并逐字节比较，再做一次追加写
- **Then**: 输出 `[U][PASS] vfs file write/read/seek`；错误路径（open 不存在、坏 fd、越界缓冲）均安全返回 -1 且进程存活
- **Pass Condition**: 该 PASS 行出现，随后的 exec shell 仍成功（证明未被错误路径猎杀）
- **Evidence**: `make test` 输出

### AC-4: read(0) 串口 canonical 输入与 Shell 端到端应答
- **Type**: `rule`
- **Given**: sh 已在 pid 1 运行并打印提示符
- **When**: 测试脚本经 stdin 注入 `hello\n` 与 `echo INPUT123\n`
- **Then**: 串口看到回显行、/bin/hello 的固定输出及其 pid、`INPUT123` 回显，且每轮结束后重新出现提示符
- **Pass Condition**: captured output 含 `fakeos:~$`、hello 固定输出、`INPUT123`，且提示符在命令输出之后再次出现
- **Evidence**: `make test` 输出

### AC-5: execve 映像替换语义正确
- **Type**: `rule`
- **Given**: pid 1 自测完成后执行 execve("/bin/sh")
- **When**: sh 启动并执行 `getpid` 等价观测（sh 内建或提示符附带/专用命令打印自身 pid）
- **Then**: sh 报告自身 pid=1（映像替换不换进程）；外部命令 /bin/hello 经 fork+exec 运行，报告新 pid 且其输出正常，父 shell 的 fd 0/1/2 在 exec 后仍工作（hello 输出到达串口）
- **Pass Condition**: 输出含 shell 自报 pid=1；hello 输出到达串口；exec 不存在路径返回 -1 后 shell 存活（注入 `nosuchcmd` 得到 not-found 提示与新提示符）
- **Evidence**: `make test` 输出

### AC-6: wait4 同步回收与退出码
- **Type**: `rule`
- **Given**: shell 执行外部命令
- **When**: 外部命令退出（hello 固定退出码 42）
- **Then**: shell 的 wait4 阻塞至其退出后才打印下一个提示符；`echo $?` 打印 42；`exit 7` 使 pid 1 以码 7 退出（内核日志可见）
- **Pass Condition**: 提示符时序在命令输出之后；输出含 `42`（echo $?）；内核 `[UPROC] pid 1 exited (code 7)` 出现
- **Evidence**: `make test` 输出

### AC-7: brk 与匿名 mmap 自测
- **Type**: `rule`
- **Given**: init 用户态自测
- **When**: malloc 若干对象并写入哨兵、扩大堆；mmap 两页匿名内存写入并读回；非法参数处理
- **Then**: 输出 `[U][PASS] brk heap malloc` 与 `[U][PASS] anonymous mmap`
- **Pass Condition**: 两行 PASS 出现
- **Evidence**: `make test` 输出

### AC-8: 目录系统调用（mkdir/chdir/getcwd/getdents64）
- **Type**: `rule`
- **Given**: shell 会话
- **When**: 注入 `mkdir /home`、`cd /home`、`pwd`、`cd ..`、`ls /bin`
- **Then**: pwd 输出 `/home`；ls 输出 hello/cat/ls/sh/init；重复 mkdir 已存在目录得到错误提示但 shell 存活
- **Pass Condition**: 上述输出按序出现
- **Evidence**: `make test` 输出

### AC-9: fd 跨 fork/exec 保持可用
- **Type**: `rule`
- **Given**: shell fork 出的子进程默认携带 fd 0/1/2
- **When**: 子进程 execve 到 cat/hello 并 write(1)/read 文件
- **Then**: 输出正常到达同一控制台；cat 读 /etc/motd 成功（文件 fd 在显式 open 后可用于 read）
- **Pass Condition**: AC-4/AC-8 的外部命令输出即为证据；另需 init 自测中 fork 子进程使用继承的 write(1) 打印 COW PASS 行（阶段三行保持）
- **Evidence**: `make test` 输出

### AC-10: 阶段三回归无回退
- **Type**: `rule`
- **Given**: 新启动序列（1 个 init + 两轮 fork COW）
- **When**: 跑完阶段三用户态序列
- **Then**: `[U] hello from ring 3, pid=`、`[U][PASS] child COW write took effect`、`[U][PASS] parent COW value intact after child write`、Ring-3/SYSCALL/COW/抢占 PASS 行以及既有 35 项断言中所有仍适用项全部保持
- **Pass Condition**: test_qemu.py 保留断言集合全部通过（启动数字相关文案允许同步更新，但 COW/抢占语义断言不得删减）
- **Evidence**: `make test` 输出

### AC-11: Shell 会话脚本完整通过并正常收尾
- **Type**: `rule`
- **Given**: 测试脚本按启动→注入→退出的时序运行
- **When**: 注入序列：`hello`、`echo INPUT123`、`cat /etc/motd`、`mkdir /home`、`cd /home`、`pwd`、`cd ..`、`ls /bin`、`echo $?`（紧跟 hello）、`nosuchcmd`、`exit 7`
- **Then**: 全部预期输出出现，pid 1 退出后 kmain 打印 `[SUCCESS] fakeos: VFS, Ramfs, POSIX syscalls, TTY input, libc and interactive shell online!`（或等效里程碑成功语）
- **Pass Condition**: 新 SUCCESS 语及全部会话断言匹配，脚本退出码 0
- **Evidence**: `make test` 退出码与输出

### AC-12: 多内存档位冒烟
- **Type**: `rule`
- **Given**: QEMU -m 分别为 128M/512M/2G
- **When**: 手工（非 test 脚本固定 128M）启动并等待 init 与 shell 就绪、发送 exit
- **Then**: 三档均完成内核自测并到达 shell、最终 SUCCESS，无 #PF/#GP 或挂死
- **Pass Condition**: 三档手工冒烟记录全部 SUCCESS
- **Evidence**: 实施记录中的命令与输出摘录

### AC-13: 代码与项目惯例一致性
- **Type**: `rubric`
- **Dimension**: 新增/修改代码与 fakeos 既有风格、fakecc 约束及内核质量基线的契合度
- **Scale**: 1-5
- **Anchors**: 1 = 引入预处理器/外部头文件、-O0 约束被破坏、调试残留或跨包裸类型指针；3 = 功能正确但风格割裂（命名/包布局/分派方式不一致）或有明显冗余；5 = 包划分清晰、句柄 API 与 as_h 风格统一、无探针残留、注释解释 why、资源回收路径完整
- **Pass Threshold**: >= 4
- **Evidence**: 独立评审对新增文件（fs/、user/、syscall.c/sched.c/uproc.c/as.c 改动、Makefile、test_qemu.py）的通读结论

## Open Questions
- 无（范围、文件发布方式、输入通道三项关键决策已由用户确认：全链路交付 / incbin / 串口 stdin 先行；实现中如遇 fakecc 代码生成阻断，按既有"规避特性 + 注释说明"惯例处理并在评审中披露）。
