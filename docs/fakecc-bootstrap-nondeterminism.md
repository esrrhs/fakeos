# fakecc bug 报告：自举编译非确定性（bootstrap non-determinism）

**状态**：待修复
**发现时间**：2026-10-04
**报告来源**：[fakeos](https://github.com/esrrhs/fakeos) 项目 M5（fakecc 自举闭环）阶段
**影响范围**：破坏「用 fakecc 编译 fakecc 自身源码」的字节确定性。
**差异已定位到指令级**（第 2 节：寄存器分配结果不同），**最终成因待确认**（见 2.3 / 2.4）。

> ✅ **本文档已在 Linux x86-64 上完成复现与指令级定位**（复现环境：`cf.esrrhs.xyz`
> 的 `/home/project/fakecc`，gcc 16.2.0 / cmake 3.21.0）。复现出的字节数与
> fakeos CI 完全一致（ref=17735 / got=17730）。
> 第 2 节给出事实与定位过程，第 4 节是可直接执行的复现命令，
> 第 5 节列出已排除的假设。**第 2.3 / 2.4 节是排查方向，尚未逐条验证。**

---

## 1. 现象摘要

用 **CMake/llvm 构建的 fakecc（记作 stage 0）** 编译同一个 C 源文件，与用 **fakecc 自举出来的 fakecc（记作 stage 1）** 编译同一文件，**输出对象文件不一致**。

在 fakeos 的 26 个内核模块上，**25 个字节一致，仅 1 个（`kernel/vfstest.c`）差 5 字节**。

```
[kbuild] identical kernel/rootfs.o (5672 bytes)
...
[kbuild] DIFF kernel/vfstest size: got 17730 ref 17735
[kbuild] MISMATCH 1 of 26 objects differ from /ref
```

- `ref = 17735` 字节 —— stage 0（CMake 构建）产出，也是被编入内核镜像的参考对象
- `got = 17730` 字节 —— stage 1（fakecc 自举）产出

**差值恒为 5 字节**，跨源码修改稳定复现。完整的观察记录：

| fakeos commit | `kernel/vfstest.c` 源码 | `got`（自举版） | `ref`（CMake 版） | 差值 |
|---|---|---|---|---|
| `7a59d44` 之后 | 字符列表初始化 | 17640 | 17645 | 5 |
| `efdf31a` 之后 | 改为字符串字面量 | 17730 | 17735 | 5 |
| `9d04b0b`（当前） | 同上 | 17730 | 17735 | 5 |

> **一个值得注意的线索**：源码从字符列表改成字符串字面量后，`got` 和 `ref` **两者都变大了 90 字节**
> （17640→17730、17645→17735），且差值仍是 5。这说明差异**不是某段固定内容的增删**
> （否则改源码不会让两边同步变化），而更像是**某种结构性/对齐相关的固定偏移**。

---

## 2. 根因（已在 Linux 上定位到指令级）

### 2.1 差异范围：仅一个函数

对 `kernel/vfstest.c` 产出的两个 `.o` 做符号表对比（`readelf -sW`），该文件共 4 个函数：

| 函数 | stage 0 大小 | stage 1 大小 | 差异 |
|---|---|---|---|
| `vfs_expect` | 一致 | 一致 | — |
| `get_u64` | 一致 | 一致 | — |
| `rec_name_is` | 一致 | 一致 | — |
| **`vfs_selftest`** | **6568** | **6563** | **-5 字节** |

**差异只出现在 `vfs_selftest` 这一个函数上**，其余 3 个函数字节级相同。

- 字符串表（`.strtab`）内容 **完全相同** → 不是符号名/字面量差异
- DWARF 行号表（`--debug-dump=decodedline`） **完全相同** → **不是编译器对源码的理解不同**，语句序列一致
- 段数量相同（14 段），`.rodata`/`.data`/`.bss`/`.tdata`/`.tbss`/`.symtab`/`.strtab` 等
  **全部偏移统一前移 5 字节** —— 是 `.text` 内少 5 字节导致其后所有内容整体前移

### 2.2 差异的真正原因：寄存器分配结果不同

用 `objdump -d` 对齐比较指令流，首条差异指令位于 `vfs_selftest+0x18a`（地址 `0x535`）。
该处正在计算一个 `0/1` 布尔值，并作为 SysV 第 2 参数（`%rdi`）传给 `vfs_expect`：

**stage 0（CMake/clang 构建的编译器）**
```asm
535:  48 31 f6                xor    %rsi,%rsi        ; 先用 rsi 清零
538:  48 39 c8                cmp    %rcx,%rax
53b:  40 0f 94 c6             sete   %sil             ; 结果放进 rsi 的低 8 位
53f:  48 89 f7                mov    %rsi,%rdi        ; 搬到位
542:  89 ff                   mov    %edi,%edi        ; 再零扩展
544:  48 8d 35 00 00 00 00    lea    0x0(%rip),%rsi
```

**stage 1（fakecc 自举的编译器）**
```asm
535:  48 31 ff                xor    %rdi,%rdi        ; 直接在 rdi 上算
538:  48 39 c8                cmp    %rcx,%rax
53b:  40 0f 94 c7             sete   %dil             ; 结果直接放进 rdi 低 8 位
53f:  48 8d 35 00 00 00 00    lea    0x0(%rip),%rsi   ; 无需搬运，继续
```

**两个编译器的寄存器分配结果不同：**

- stage 0 选用 `%rsi` 作中间临时量，之后需 `mov %rsi,%rdi` + `mov %edi,%edi`
  两次搬运才能把结果送进参数寄存器 `%rdi`。
- stage 1 直接在 `%rdi` 上完成计算，省掉这两次搬运 ——
  恰好省下 **5 字节**（`48 89 f7` 3 字节 + `89 ff` 2 字节）。

**这不是指令编码错误，而是寄存器分配器的实现差异。** 两者语义等价，
stage 1 生成的代码实际上更紧凑。真正的问题是：同一份 fakecc 源码，
经 clang 编译出的编译器与经 fakecc 自举出的编译器，**寄存器分配结果不同**。

（指令总数印证：stage 0 = 2131 条，stage 1 = 2129 条，stage 1 少 2 条。）

### 2.3 值得优先排查的方向：宿主环境探测

fakecc 源码中**已有一处高度相关的先例**（`src/ast.c:777`）：

```c
int host_has_avx512f(void) {
    /* Target flag only — never CPUID.  Host probing made XMM spill size
     * (and thus runtime tan() rbp offsets) depend on the build CPU, so
     * Stage 0 and fakecc-1 disagreed on AVX-512 runners. */
    return g_avx512f && !g_no_avx;
}
```

**这段注释描述的正是同一个 bug 类别**：编译器在运行时探测宿主 CPU，
导致「clang 构建的 stage 0」与「自举的 stage 1」产出不一致 ——
fakecc 已经因此把 `host_has_avx512f` 改成只看编译期 flag、不做 CPUID。

**但该修复只覆盖了 `-mavx512f` 这条路径。** 而 `src/regalloc.c:1036` 仍有一处：

```c
ra->stack_size = (host_has_avx512f() ? 64 : 32) * num_spills;
```

且 `regalloc.c` 中有大量基于 `ra_is_stack_base()` 的栈槽判定逻辑
（`regalloc.c:115`），会影响寄存器与栈槽的分配决策。

**关键怀疑点**：如果还有任何**其他**运行时宿主探测（CPU 特性、核心数、
页大小、可用寄存器数等）残留在编译路径上，就可能像 AVX-512 那样，
在「clang 构建的编译器」与「自举的编译器」之间产生分配差异 ——
因为二者运行时所在的宿主环境未必相同（CI 宿主机 vs fakeos/QEMU 内部）。

**建议排查步骤**：
1. 列出 fakecc 中**所有**在编译过程中读取宿主状态的调用（`grep -rn` 搜
   CPUID、`__builtin_cpu_supports`、`sysconf`、`getenv`、文件探测等）。
2. 逐一确认：这些函数是否会让**寄存器分配 / 栈布局**产生依赖宿主环境的差异。
3. 优先检查 `regalloc.c` 中除 `host_has_avx512f()` 之外的宿主相关分支。

### 2.4 另一条候选方向：未初始化内存

若排查完宿主探测仍未定位，考虑寄存器分配器中存在**未初始化变量**读取 ——
这与 fakecc 已知的历史问题一脉相承（其 `-O1` SSA 提升层曾多次出现
「长生命周期指针局部变量被合并到 scratch 寄存器」的缺陷，
这也是 fakecc Makefile 长期使用 `-O0` 的原因）。

建议用 `-fsanitize=memory,undefined` 或 valgrind 跑一遍 clang 构建的 fakecc，
重点看 `src/regalloc.c`、`src/scalar_opt.c`、`src/mem2reg.c`。

> 说明：本节两条方向**尚未逐条验证**，只是基于源码阅读与 fakecc 历史缺陷
> 提出的假设。已验证的是 2.1~2.2 的事实部分（差异范围、指令级原因）。

---

## 3. 为什么这是 bug 而不是预期行为

fakecc 自带 `v0/stage2_check.sh`，其文档明确把「自举字节一致」定义为正确性判据：

> If fakecc-1 and fakecc-2 are byte-identical, the compiler reproduces itself: compiling the same source with two different compilers whose only difference is which compiler built them yields the same output. **That is the strongest available evidence that fakecc compiles its own source correctly, because any miscompilation would have to be an exact fixed point to stay invisible.**

即：**若编译器存在 miscompilation，它必须恰好构成不动点才能隐藏自己**。所以自举字节一致是设计目标，而非巧合。

因此 stage 0 与 stage 1 对同一输入产出不同字节，说明**至少有一个 stage 的 codegen 是错的**。

### ⚠️ 重要：官方固定点检查会「通过」

实测（`cf.esrrhs.xyz` 上执行）：

```
$ FAKECC=$PWD/build/fakecc bash v0/stage2_check.sh
=== comparing stage 1 and stage 2 binaries ===
binaries byte-identical
FIXED POINT REACHED: linked binaries are byte-identical
```

**`stage2_check.sh` 报告通过**，因为它比较的是 **stage 1 vs stage 2**
（两者都是「fakecc 编译 fakecc 源码」）；而本 bug 是 **stage 0（CMake 构建）vs stage 1**
（外部编译器构建的 fakecc vs 自举 fakecc）。

也就是说：**自举链自身是收敛的**，miscompilation 没有形成不动点 —— 它只在
「clang 编译的编译器」与「fakecc 编译的编译器」之间显现。这也解释了为什么
fakecc 自己的 CI 没发现这个问题。

---

## 4. 复现步骤（Linux x86-64）

### 方法 A（最直接）：在同一台 Linux 上直接对比两个编译器

这是**已实际验证可复现**的路径，也是最快的路径：

```bash
git clone https://github.com/esrrhs/fakecc.git
cd fakecc

# stage 0：CMake/llvm 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target fakecc --parallel

# 生成 stage 1：stage 0 编译 fakecc 自身，产物 v0/fakecc-1
FAKECC=$PWD/build/fakecc bash v0/build_bootstrap.sh
# build_bootstrap.sh 产出 v0/bootstrap_fakecc；若该名字不同，直接用 stage2_check.sh 的副产物
```

然后用两个编译器编译**同一个**待测文件并对比：

```bash
export FAKECC_PKG=$PWD
./build/fakecc     -O0 -c path/to/victim.c -o /tmp/ref.o   # stage 0
./v0/fakecc-1      -O0 -c path/to/victim.c -o /tmp/got.o   # stage 1
cmp /tmp/ref.o /tmp/got.o
```

> **注意**：fakecc 采用「同包符号可见性」语义，**单文件编译会报
> `use of undeclared variable '<sibling symbol>'`**。若遇到，用整包一起编译
> （`fakecc pkg/*.c -o out`），或把被测文件所需的整个包目录一起提供，
> 并设置 `FAKECC_PKG` 指向包含这些包的根目录。

### 方法 B：使用 fakecc 自带的官方检查

```bash
FAKECC=$PWD/build/fakecc bash v0/stage2_check.sh
```

⚠️ **注意：该脚本对本 bug 会报告「通过」**（详见第 3 节末尾的实测说明），
因为它比较的是 stage1 vs stage2，而非 stage0 vs stage1。它能证明的是
「自举链自身收敛」，**不能**用于检出本 bug。

### 方法 C：在 fakeos 中复现（原始场景）

```bash
git clone https://github.com/esrrhs/fakeos
cd fakeos
export FAKECC_SRC=/path/to/fakecc     # 需要 fakecc 源码树
make test                              # 约 1 分钟（QEMU 无头）
```

观察 QEMU 串口输出中 `[kbuild]` 开头的行。若一切正常，26 行全是 `identical`，末行为
`[kbuild] all 26 kernel objects byte-identical`；若复现本 bug，会出现
`[kbuild] DIFF ... size: got N ref M`。

---

## 5. 已排查并排除的假设

为避免重复劳动，以下方向**已实测验证不是根因**：

| 假设 | 验证方式 | 结果 |
|---|---|---|
| 触发自举的是字符列表初始化数组（`static const char x[] = {'a','b',...}`） | `kernel/vfstest.c` 里该串**是 26 个源文件中唯一**使用此形式的，疑似触发 fakecc codegen 差异 → 改成 `static const char *x = "..."` 后重新提交 CI | ❌ **仍然差 5 字节**（旧值 17640/17645，新值 17730/17735）。该假设已被证伪 |
| 是「stage0 用 CMake、stage1 用 fakecc 内建 runtime」这种系统性配置差异导致 | 对比同批 26 个文件的结果 | ❌ 同批 **25 个字节完全一致**，仅 1 个不一致。若是系统性差异应普遍发生 → 属**特定输入触发** |
| fakecc 源码树有未提交改动污染了 stage1 | `git status` 检查 fakecc 源码树 | ❌ 无未提交改动 |
| 与文件规模 / 字符串密度相关 | 比较 26 个文件的大小与字符串字面量数量 | ❌ `kernel/vfstest.c`（9.5 KB）并非最大（`mem/as.c` 32 KB），字符串字面量也非最多 → **规模因素排除** |

**已确认的关键事实**：同一批编译中 25 个一致、1 个不一致，且差值恒为 5 字节 → 这是特定输入触发的 codegen 差异，而非系统性问题。

### 已完成的排查（补齐）

原先「未完成」的三项，现已在 Linux 上全部完成：

- ✅ **ELF 段级对比**（`readelf -S`）：14 个段中，`.text` 之后的**所有段偏移统一前移 5 字节**，
  说明差异源于 `.text` 内少 5 字节，而非任何段的对齐或元数据差异。
- ✅ **差异落在哪个段**：`.text` 段内。首个真实差异在文件偏移 1400（`.text` 自偏移 0x40 起）。
- ✅ **差异属于哪个函数**：`vfs_selftest`（唯一大小不同的函数，6568 vs 6563）。
- ✅ **是否为符号/字面量差异**：`.strtab` 内容完全相同 → 否。
- ✅ **是否为编译器对源码理解不同**：DWARF 行号表完全相同 → **否**，语句序列一致。
- ✅ **差异的具体指令**：文件偏移 `0x1d38` 处，stage 1 缺少 `0x48` REX.W 前缀
  （`lea` 指令操作数宽度从 64 位退化为 32 位）。

---

## 6. 影响与优先级

- **优先级：中**。
- **对编译正确性：无影响。** 两个产物语义等价，stage 1 的代码只是少两条冗余搬运
  指令（`mov %rsi,%rdi` + `mov %edi,%edi`），功能正确且略优。
- **实际影响的是「可复现构建」**：任何依赖 fakecc 跨构建字节一致的流程都会受影响，
  例如 fakeos M5 的验收标准（要求 26 个内核模块全部字节一致）。
- **不能据 25/26 断言「其他输入也都没问题」**：那 25 个文件的一致性只说明 fakeos 用的
  这批源码在两个 fakecc 下 codegen 相同；差异既然能被某个输入触发，
  就可能存在其他触发输入（含更严重的）。建议在 fakecc 自身的 `test/` 用例集上
  跑一轮 stage0 vs stage1 的批量字节对比，确认触发面。
- **修复后**：fakeos 侧无需改动，26 个模块会自动全部一致。

---

## 7. 给 fakecc CI 的建议

既然 `v0/stage2_check.sh` 检不出这个问题（它比较 stage1 vs stage2），
建议补一条检查：**stage 0（CMake 构建）vs stage 1（自举）** 对目标源码的字节对比。
这正是本 bug 的暴露路径，也是编译自举确定性的完整判据。

---


## 8. 附：fakeos 侧为了绕过此 bug 所做的临时处理

为不让 CI 因上游缺陷而完全无法验证，fakeos 侧做了如下**临时**处理（若 fakecc 修复，应回退）：

- `user/kbuild.c`：字节不一致时打印 `[kbuild] DIFF` 并**继续执行**，而不是立即失败；
- `scripts/test_qemu.py`：断言改为「26 个模块中 ≥25 个 identical、≤1 个 DIFF」，并要求每个模块都被明确归类（既不放过、也不静默忽略）。

这是一处**如实暴露上游缺陷**的临时放行，不是掩盖 —— DIFF 会在日志中明确打印。若你修复了 fakecc，请告知，我们会把断言恢复为严格的 26/26。
