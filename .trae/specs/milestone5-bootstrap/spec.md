# fakeos 阶段五产品需求文档 - fakecc 自举与生态全闭环

## Overview
- **Summary**: 在 fakeos 内部原生运行 fakecc 编译器（静态 ELF64、零外部依赖），在 OS 内编译并执行用户 C 程序；进一步在 OS 内用 fakecc 重编译 fakeos 内核的**全部 C 源文件**，产出的 `.o` 与宿主机 fakecc 产物**逐字节一致**；再由 OS 内自研最小静态链接器 `ldfake`（消费 fakecc ELF64 目标 + 预构建 nasm 目标 blob）链接出**可引导的 multiboot ELF32 内核镜像**，经串口导出后由宿主 QEMU 实际引导并通过回归，达成生态自举闭环。
- **Purpose**: 为阶段五 fakecc 自举提供 OS 内文件 I/O、进程、工具链与构建编排能力，证明"fakeos 可以编译自身"。
- **Target Users**: fakeos 开发者与 CI（串口无头测试）。

## 背景与关键约束（调研结论）
- fakecc 已完全自举、默认产出零依赖静态 ELF64（内置 ELF 生成/链接器、自带 `_start`，基址 0x400000，与内核 ELF 装载器兼容）。
- 已安装 fakecc 是 WIP 源码（/Users/mingming/project/fakecc，含未提交修改）的 stage0 构建；其自举方言快照在 `v0/`。macOS 后端模块（macho/a64）的当前 src 翻译在 scratch 副本中可绕过（见工具脚本），产出静态 linux x86-64 fakecc 约 2.2 MB。**fakecc 源码仓库零改动**：所有引导构建在 /tmp 临时副本完成，复现脚本入 fakeos 仓库。
- fakecc runtime 直接发 Linux syscall 号：已实现 0/1/2/3/8/9/39/60/217；缺口：11 munmap、87 unlink、90 chmod、186 gettid、202 futex、231 exit_group。编译器本身不创建线程（futex/gettid 仅其 runtime 线程库使用）。
- 包解析：fakecc 无 FAKECC_PKG 环境变量时按**输入文件目录**与 runtime 位置（argv[0] 目录）搜索；其 getenv 通过 open("/proc/self/environ") 实现。
- 内核目标重定位仅 5 种：R_X86_64_64(1)、PC32(2)、GOTPCREL(9)、PLT32(4)、32(10)、32S(11)；asm 目标另含同集合。当前镜像 282 KB（ELF32 multiboot），.o 合计约 424 KB。
- 内核链接布局（boot/linker.ld）：低段 VMA 0xC0000000/LMA 0x100000（.multiboot/.boot.text/.boot.data/.boot.bss），高段 VMA 0xFFFFFFFF80100000/LMA 0x101000（.text/.rodata/.data/.bss），入口 0x100000。
- 不可达目标（明确排除）：OS 内汇编器（nasm 替代）与 GNU ld 字节级兼容——5 个 asm `.o` 作为**预构建 blob** 发布进 Ramfs；OS 内链接器是面向本内核固定布局的专用链接器，不追求与 GNU ld 产物逐字节相同，而追求**可引导 + 行为等价**。

## Goals
- G1：/bin/fakecc 在 fakeos 内运行（读取 Ramfs 源文件、产出可执行文件）。
- G2：OS 内编译用户 C 程序并成功执行（编译器→链接→execve 全链路在 OS 内）。
- G3：OS 内 fakecc 编译内核全部 C 源，`.o` 与宿主机同版本 fakecc 产物逐字节一致（确定性编译器证明）。
- G4：OS 内 ldfake 链接出可引导内核镜像，宿主 QEMU 引导该镜像并通过精简回归。
- G5：串口无头自动化覆盖 G2/G3/G4；阶段四回归保持全绿。

## Non-Goals
- OS 内汇编器；与 GNU ld/objcopy 字节级一致的通用链接器；磁盘持久化文件系统；动态链接/共享库。
- fakecc 源码仓库任何修改（工具链构建仅在 /tmp scratch 副本）。
- OS 内 fakecc 自编译固定点（stage1→stage2，因 TCG 耗时与 WIP macho 翻译问题，留待后续）。

## Functional Requirements
- **FR-1（缺口 syscall）**：补齐 fakecc runtime 依赖：`munmap(11)` 支持匿名 VMA 整段/部分解除（拆 VMA、解除页表映射、帧归还）；`unlink(87)` 删除 Ramfs 目录项并回收动态 inode（静态 incbin inode 允许删除名但不释放内核镜像内存，标记失效即可）；`chmod(90)` 返回 0（无权限模型）；`exit_group(231)` 等同 exit(code&0xff)；`gettid(186)` 返回当前 pid；`futex(202)` 返回 -ENOSYS(-38)（线程不被编译器使用）。
- **FR-2（Ramfs 扩容）**：inode 槽 64→256、单目录目录项 32→64、动态文件 1 MiB→16 MiB；既有自测与镜像不得回归。
- **FR-3（环境伪文件）**：发布静态只读文件 `/proc/self/environ`，内容 `FAKECC_PKG=/src`，使在 /src 工作目录调用 fakecc 时包搜索路径覆盖内核包根。
- **FR-4（/bin/fakecc 发布）**：宿主工具脚本（入仓库 `tools/`）在 /tmp 临时副本完成 fakecc stage1 linux 静态构建；Makefile 在缺失时调用它产出 `build/rootfs/bin/fakecc` 并以 incbin 发布；该文件不提交 git（构建产物，.gitignore 覆盖 build/ 下暂存目录）。
- **FR-5（OS 内编译用户程序）**：发布 fakecc runtime 源到 `/bin/runtime/`、示例源 `/src/ping.c`；shell 支持参数含 `/` 时按路径直接执行（不再仅 /bin 前缀）；会话中 `cd /src && /bin/fakecc ping.c -o /tmp/ping && /tmp/ping` 输出预期字符串。
- **FR-6（构建源树发布）**：Makefile 暂存内核全部 `.c` 源（保持包目录 /src/kernel、/src/arch、/src/mem、/src/drivers、/src/fs、/src/types）、宿主参考 `.o`（与当前构建同 fakecc 同参数）到 /ref/<名>.o、5 个 nasm `.o` 到 /asm/、ldfake 链接清单所需全部输入，经多 blob incbin 发布。
- **FR-7（kbuild 编排器，/bin/kbuild）**：单进程内依次：(a) fork/execve 运行 /bin/fakecc 编译 ping.c 为可执行并运行，核验输出；(b) 对内核每个 .c 以 `-c -O0 --target=x86_64-linux` 编译到 /tmp，读取 /ref 同名 `.o` 逐字节比较（长度+内容），打印每个模块 `[BUILD] <mod>: identical` 或失败明细，任一不一致则 FAIL；(c) fork/execve /bin/ldfake 按固定清单链接 /tmp/fakeos-new，报告大小。
- **FR-8（ldfake 最小静态链接器，/bin/ldfake）**：fakecc 方言 C 编写，解析 ELF64 可重定位目标（节/符号/RELA），处理重定位 1/2/4/9/10/11（GOT 槽按需生成），按 boot/linker.ld 等价布局（低段 0x100000、高段 0xFFFFFFFF80100000，节序 text/rodata/data/bss，丢弃 note/GNU-stack），直接输出 multiboot 可引导 ELF32 镜像（ELF32 Ehdr/Phdr，p_paddr=LMA，入口 0x100000），语义对齐当前 objcopy 产物（先以 readelf 核对现有镜像程序头）。
- **FR-9（镜像导出）**：/bin/dumpbin 以 base64 分帧（含起止标记与长度）输出文件；test 脚本据此解码重建宿主侧文件，字节长度与内核报告一致。
- **FR-10（重建镜像引导验证）**：宿主测试脚本用 QEMU 引导重建镜像（-m 与正式一致），注入精简命令（pid/hello/exit 7），确认 shell、编译产物 SUCCESS banner 与关键阶段四断言通过。
- **FR-11（资源与稳定性）**：fakecc 与 kbuild 运行在 128 MiB（如不足按实测提升测试机型并记录）；编译循环不泄漏 fd/inode；阶段四全部断言保持。
- **FR-12（构建集成）**：`make test` 一键完成阶段一~五全部回归；`tools/m5_build_fakecc.sh` 可重复执行（幂等，仅写 /tmp 与 build/rootfs）。

## Non-Functional Requirements
- **NFR-1（可重复）**：`make test` 连跑 3 次全绿（kbuild 耗时长，允许单次总时长到 10 分钟量级，但需稳定不 flaky；逐字节比较本身无时序成分）。
- **NFR-2（fakecc 约束）**：新内核/用户代码继续零预处理器、包模块制、句柄化；ldfake/kbuild/dumpbin 必须被宿主 fakecc 以 -O0 成功编译。
- **NFR-3（隔离）**：fakecc 编译失败只返回非零与 stderr 文本，不杀内核；非法 ELF 输入由 ldfake 拒绝。
- **NFR-4（文档）**：README 阶段五勾选与自举说明；记录工具链 scratch 构建过程与 asm 预构建 blob 的边界原因。

## Constraints
- fakecc 仓库（/Users/mingming/project/fakecc）只读；-O0；TCG QEMU；串口 stdio；无新第三方工具。
- 参考 `.o` 由 Makefile 用**已安装 fakecc**（与构建 /bin/fakecc 的 stage0 同源）生成；若 stage0/stage1 输出存在版本偏差导致逐字节不一致，作为发现问题报告并记录（不得放宽比较标准）。

## Assumptions
- TCG 下单文件内核编译在数十秒量级；总 kbuild 时长可接受（实测调整）。
- ldfake 所需重定位/节集合限于调研到的 6 种与标准节名；遇到额外节以最小扩展处理并记录。

## Acceptance Criteria

### AC-1: rule — 缺口 syscall
**Given** fakecc 运行时路径，**When** 触发 munmap/unlink/chmod/exit_group/gettid/futex，**Then** munmap 真正解除映射（再访问触发 #PF 而非旧数据）、unlink 后 lookup 失败、chmod 返回 0、exit_group 结束进程、gettid=pid、futex 返回 -38；内核自测或 kbuild 行为证据。

### AC-2: rule — Ramfs 扩容无回归
**Given** 扩容后，**When** 启动并执行阶段四 VFS 自测与 shell 会话，**Then** 三行 VFS PASS 与阶段四全部断言保持。

### AC-3: rule — /bin/fakecc 在 OS 内运行
**Given** 发布的静态 fakecc，**When** shell 执行 `/bin/fakecc --help`（无参/错误用法输出 usage），**Then** 看到其 usage 文本，进程正常返回。

### AC-4: rule — OS 内编译并运行用户程序
**When** `cd /src` 后 fakecc 编译 ping.c 并执行，**Then** 输出固定标记字符串（如 `pong-from-fakeos-compiler`），退出码 0。

### AC-5: rule — 内核 .o 逐字节一致
**When** kbuild 编译全部内核 .c 并与 /ref 比较，**Then** 每个模块打印 identical，汇总行 `[BUILD] all <N> kernel objects byte-identical`；N=实际内核 C 文件数。

### AC-6: rule — ldfake 产出有效镜像
**When** kbuild 链接，**Then** /tmp/fakeos-new 生成，长度在合理区间（128 KiB..2 MiB），ELF32 头/multiboot magic/入口 0x100000/程序头 p_paddr 布局正确（宿主侧脚本校验魔数与 readelf 等价字段，脚本内以字节偏移解析）。

### AC-7: rule — 重建镜像可引导且行为等价
**Given** 串口导出解码的重建镜像，**When** 宿主 QEMU 引导并注入 pid/hello/exit 7，**Then** 到达 shell 提示符、hello 输出、pid 1 退出码 7 与新 SUCCESS banner 出现。

### AC-8: rule — 阶段四回归保持
**When** make test 完成，**Then** 阶段四 shell 会话 74 检查点全部仍通过。

### AC-9: rule — 工具脚本幂等可复现
**When** 从干净树执行 tools/m5_build_fakecc.sh 后 make，**Then** /bin/fakecc 产出且功能正确；重复执行结果一致；fakecc 仓库工作树零变化（git -C status 无新增改动）。

### AC-10: rule — 失败隔离
**When** OS 内用 fakecc 编译一份语法错误的 .c，**Then** fakecc 非零退出、stderr 有诊断、shell 存活可继续下命令。

### AC-11: rule — 连跑稳定
`make test` 连跑 3 次，AC-4/5/6/7/8 每次全过，无 flaky。

### AC-12: rubric — 代码与工程质量
**Dimension**: 自研链接器正确性与可读性、构建集成清晰度、句柄/包布局一致性、注释解释 why（尤其 ELF32/布局/scratch 边界）、无调试残留；scale 1-5；anchors 1=链接器靠试错且无校验/无注释，3=功能正确但布局常量散落/失败路径薄弱，5=节/符号/重定位/布局分层清晰、失败可诊断、与现有包风格统一；**Pass Threshold >= 4**；**Evidence**: 新增文件通读 + 评审。

## Open Questions
- 无（关键范围与判定标准已经用户确认：C 产物逐字节一致 + OS 内链接出可引导镜像；asm .o 预构建；fakecc 零改动）。
