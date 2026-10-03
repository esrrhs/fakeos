# fakeos 阶段五实施计划 - fakecc 自举与生态全闭环

> 任务依赖有序。判定标准：AC 见 spec.md。核心风险：ldfake 布局正确性、TCG 下编译耗时、stage0/stage1 确定性偏差。

## Task 1: 缺口 syscall（munmap/unlink/chmod/exit_group/gettid/futex）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: None
- **Description**:
  - mem/as.c：范围解除辅助（清 PTE/帧归还/ref 递减）+ VMA 拆分/截断；`as_munmap_h(handle, va, len)`（整段/首部/尾部/中间四种）。
  - fs/ramfs.c：fs_unlink(path)（删目录项；动态 inode 置 free 回收槽，静态 inode 仅摘名）。
  - kernel/sysproc.c：sys_munmap；sys_chmod 恒 0；sys_gettid=pid；sys_futex=-38；exit_group 派发到 proc_terminate。
  - syscall 号 11/87/90/186/202/231 接入 dispatch。
- **Acceptance Criteria Addressed**: AC-1
- **Test Requirements**:
  - `rule`：unlink 后 open 失败；munmap 后写触发 #PF 被进程隔离（kbuild/ping 侧面 + 必要时内核自测一行）；fakecc 运行不触发 futex 路径。

## Task 2: Ramfs 扩容与 /proc/self/environ
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 1
- **Description**:
  - MAX_INODES 256、MAX_DENTS 64、FILE_MAX 16 MiB（动态缓冲链路上限同步）。
  - rootfs 发布 `/proc/self/environ`（静态串 `FAKECC_PKG=/src`），逐级建 /proc/self 目录。
- **Acceptance Criteria Addressed**: AC-2
- **Test Requirements**:
  - `rule`：阶段四 VFS 自测三行 PASS 与 make test 全绿；OS 内 cat /proc/self/environ 可见该串。

## Task 3: fakecc 原生镜像构建管线（tools 脚本 + Makefile 暂存 + blob 发布）
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 2
- **Description**:
  - tools/m5_build_fakecc.sh：幂等；复制 fakecc 源码（FAKECC_SRC 默认 ../fakecc）到 /tmp scratch、translate、补 shim/桩（记录在脚本注释）、用宿主 fakecc 交叉编译静态 linux ELF 到 build/rootfs-stage/bin/fakecc；校验 static/no-INTERP。
  - Makefile：FAKECC_SRC 变量；stage 目录汇集 /bin/fakecc、fakecc runtime 源（→ binruntime/，供 OS 内可执行链接）、内核 C 源（→ src/<pkg>/）、user ping.c（→ src/）、宿主参考 .o（→ ref/）、5 个 nasm .o（→ asm/）。
  - user/blob.asm 扩展多 blob（宏批量），rootfs.c 扩展发布所有新路径；.gitignore 覆盖 build/rootfs-stage。
- **Acceptance Criteria Addressed**: AC-3、AC-9、FR-6
- **Test Requirements**:
  - `rule`：OS 内 `/bin/fakecc` usage 输出（AC-3）；重复跑脚本幂等；fakecc 仓库 git status 零变化；无参 fakecc 输出 usage 并返回非零但不杀 shell。

## Task 4: shell 任意路径执行 + ping.c OS 内编译运行
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 3
- **Description**:
  - sh：命令含 '/' 时先按该路径 execve（/tmp/ping、./ping），再回退 /bin 前缀。
  - user/progs/ping.c（用 fakecc hosted runtime：printf；main 返回 0，输出 `pong-from-fakeos-compiler`）；构建为普通用户 ELF（不进默认 rootfs，仅源文件入 /src）。
  - 发布 runtime 源位置以 fakecc argv0 解析规则为准（实现时 read find_rt_dir 核实，落 /bin/runtime 或 /src/runtime）。
- **Acceptance Criteria Addressed**: AC-4、AC-10
- **Test Requirements**:
  - `rule`：OS 内 `cd /src; /bin/fakecc ping.c -o /tmp/ping; /tmp/ping` 输出标记串；错误 .c 编译失败 shell 存活。

## Task 5: /bin/kbuild 编排器
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 4
- **Description**:
  - user/kbuild.c（fakeos 用户程序）：(a) chdir /src；fork/execve fakecc 编译 ping.c→/tmp/ping 并运行核验；(b) 固定内核模块清单（包路径+名），逐个 `-c -O0 --target=x86_64-linux`，wait 回收，读 /tmp/x.o 与 /ref/x.o 逐字节比；(c) 输出逐模块 identical 行与汇总 `[BUILD] all N kernel objects byte-identical`，失败给首个差异偏移；(d) 最后 fork/execve /bin/ldfake 传对象清单，报告镜像大小。
  - Makefile 把 kbuild.c 链接为 /bin/kbuild（fakeos crt0/libc 体系）。
- **Acceptance Criteria Addressed**: AC-5
- **Test Requirements**:
  - `rule`：全部模块 identical；数量=内核 C 文件数；任一不一致给出偏移并 FAIL。

## Task 6: /bin/ldfake 最小静态链接器
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 5
- **Description**:
  - 先 readelf -l 现有 build/fakeos.elf 记录 ELF32 程序头/入口/地址语义作为对齐基线。
  - user/ldfake.c：ELF64 目标解析（ehdr/shdr/sym/rela/strtab）；节合并（.text→RX、.rodata→R、.data/.got→RW、.bss→NOBITS；忽略 .rela/.symtab/.strtab/.note*）；全局符号解析（含 fakecc 与 nasm 两侧）；重定位 1/2/4/9/10/11；GOT 槽生成；低/高两段布局；输出 ELF32 multiboot 镜像（phdr-only 可引导，p_paddr LMA，入口 0x100000）。
  - 输入清单由 kbuild argv 提供（顺序固定：boot 起始，高段按链接清单）。
  - 坏目标/未解析符号/未知重定位 → 非零 + stderr 诊断。
- **Acceptance Criteria Addressed**: AC-6、AC-12、NFR-3
- **Test Requirements**:
  - `rule`：镜像长度合理、宿主脚本字节解析校验 ELF32 magic/entry/MB magic/程序头 paddr；ldfake 对坏输入安全失败。

## Task 7: /bin/dumpbin + 重建镜像宿主引导验证 + test_qemu 集成
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 6
- **Description**:
  - user/dumpbin.c：base64 分帧输出 `===DUMP-BEGIN name len===`/base64/`===DUMP-END===`。
  - test_qemu.py：shell 会话后执行 `kbuild`（长等待/分段 drain，直到汇总行）、`dumpbin /tmp/fakeos-new`，解码写 /tmp/fakeos-rebuilt.elf；二次 QEMU 引导重建镜像注入精简命令断言；保留全部阶段四 74 检查点；新增阶段五断言。
  - README 阶段五勾选 + scratch 边界说明；Makefile 依赖链与 clean。
- **Acceptance Criteria Addressed**: AC-7、AC-8、AC-11、NFR-1、NFR-4
- **Test Requirements**:
  - `rule`：重建镜像 QEMU 二次引导通过（提示符/hello/pid/exit 7/SUCCESS）；make test 连跑 3 次全绿。

## Task 8: 评审与修复闭环
- **Status**: `completed`
- **Priority**: high
- **Depends On**: Task 7
- **Description**: 独立只读评审（链接器正确性、确定性比较可信度、资源/失败路径、构建可复现）；修复→复审→提交。
- **Acceptance Criteria Addressed**: AC-12
- **Test Requirements**:
  - `rubric`：AC-12 >= 4；评审 pass 后收尾。
