# fakecc bug 报告：自举编译非确定性（bootstrap non-determinism）

**状态**：待修复
**发现时间**：2026-10-04
**报告来源**：[fakeos](https://github.com/esrrhs/fakeos) 项目 M5（fakecc 自举闭环）阶段
**影响范围**：破坏「用 fakecc 编译 fakecc 自身源码」的字节确定性（自举固定点性质）。对普通程序的编译质量影响尚未评估 —— 目前只有一组反例证据，不足以断定无影响（见第 6 节）。

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
> 建议在段级对比时特别留意 `.strtab`、符号表条目数、以及段/符号的对齐填充。

---

## 2. 为什么这是 bug 而不是预期行为

fakecc 自带 `v0/stage2_check.sh`，其文档明确把「自举字节一致」定义为正确性判据：

> If fakecc-1 and fakecc-2 are byte-identical, the compiler reproduces itself: compiling the same source with two different compilers whose only difference is which compiler built them yields the same output. **That is the strongest available evidence that fakecc compiles its own source correctly, because any miscompilation would have to be an exact fixed point to stay invisible.**

即：**若编译器存在 miscompilation，它必须恰好构成不动点才能隐藏自己**。所以自举字节一致是设计目标，而非巧合。

因此 stage 0 与 stage 1 对同一输入产出不同字节，说明**至少有一个 stage 的 codegen 是错的**。

---

## 3. 复现步骤（Linux x86-64）

### 方法 A：使用 fakecc 自带的官方检查（推荐先跑这个）

fakecc 仓库已自带该场景的检查脚本：

```bash
git clone https://github.com/esrrhs/fakecc.git
cd fakecc

# stage 0：CMake/llvm 构建
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target fakecc --parallel

# 固定点检查：stage0 编译 fakecc 得到 fakecc-1，再用 fakecc-1 编译得到 fakecc-2
FAKECC=$PWD/build/fakecc bash v0/stage2_check.sh
```

**预期**（正确行为）：末行输出 `FIXED POINT REACHED: linked binaries are byte-identical`

**若有问题**：输出 `NOT a fixed point — stage 1 and stage 2 binaries differ`，并把两个产物留在 `v0/fakecc-1`、`v0/fakecc-2`，同时打印 `cmp -l` 的前 20 个差异字节。

> **注意对比对象不同**：本脚本比较的是 **stage 1（stage0 编出的 fakecc）vs stage 2
> （fakecc-1 再编出的 fakecc）**，两者都是「fakecc 编译 fakecc 源码」。
> 而 fakeos 观察到的不一致是 **stage 0（CMake/llvm 构建的 fakecc）vs stage 1**，
> 即「外部编译器构建的 fakecc」vs「自举出来的 fakecc」。
>
> 两者都指向同一个根本问题（fakecc 编译自身的确定性），但不是同一个比较：
> - 若 `stage2_check.sh` **失败** → 说明自举链本身不收敛，是本 bug 的直接证据。
> - 若 `stage2_check.sh` **通过**但 fakeos 仍不一致 → 说明问题更窄：只发生在
>   CMake 构建的 stage 0 与 fakecc 自举版之间。此时需要按方法 B 的路径，
>   拿到 `ref.o` / `got.o` 做段级对比来定位。
>
> 建议**两个都跑**，这能帮你判断问题的范围。

### 方法 B：在 fakeos 中复现（原始场景）

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

## 4. 已排查并排除的假设

为避免重复劳动，以下方向**已实测验证不是根因**：

| 假设 | 验证方式 | 结果 |
|---|---|---|
| 触发自举的是字符列表初始化数组（`static const char x[] = {'a','b',...}`） | `kernel/vfstest.c` 里该串**是 26 个源文件中唯一**使用此形式的，疑似触发 fakecc codegen 差异 → 改成 `static const char *x = "..."` 后重新提交 CI | ❌ **仍然差 5 字节**（旧值 17640/17645，新值 17730/17735）。该假设已被证伪 |
| 是「stage0 用 CMake、stage1 用 fakecc 内建 runtime」这种系统性配置差异导致 | 对比同批 26 个文件的结果 | ❌ 同批 **25 个字节完全一致**，仅 1 个不一致。若是系统性差异应普遍发生 → 属**特定输入触发** |
| fakecc 源码树有未提交改动污染了 stage1 | `git status` 检查 fakecc 源码树 | ❌ 无未提交改动 |
| 与文件规模 / 字符串密度相关 | 比较 26 个文件的大小与字符串字面量数量 | ❌ `kernel/vfstest.c`（9.5 KB）并非最大（`mem/as.c` 32 KB），字符串字面量也非最多 → **规模因素排除** |

**已确认的关键事实**：同一批编译中 25 个一致、1 个不一致，且差值恒为 5 字节 → 这是特定输入触发的 codegen 差异，而非系统性问题。

> ⚠️ **未完成的排查**（因 macOS 无法运行 Linux 自举产物而未能进行，建议接手者在 Linux 上继续）：
> - 未能对 `ref` 与 `got` 做 ELF 段级 / 符号表级对比（只有 Linux CI 上的真实产物才能做）。
> - 未能 dump 两阶段的 IR / 汇编来定位是哪一阶段的 pass 引入了差异。
> - 未能确认这 5 字节落在哪个段（`.text` / `.rodata` / `.strtab` / 符号表 / 对齐填充）。
>
> **建议优先做段级对比**，很可能直接指出差异所在：
> ```bash
> readelf -S -s ref.o > ref.txt; readelf -S -s got.o > got.txt; diff ref.txt got.txt
> cmp -l ref.o got.o | head -40      # 首个差异字节及偏移
> ```

---

## 5. 定位建议

`kernel/vfstest.c` 是一个很好的复现样本。请优先尝试：

1. **先做段级/符号表对比**（成本最低、最可能直接定位）：拿到 `ref.o` 与 `got.o` 后
   ```bash
   readelf -S -s ref.o > ref.txt; readelf -S -s got.o > got.txt; diff ref.txt got.txt
   cmp -l ref.o got.o | head -40
   ```
   确认 5 字节落在哪个段：若是 `.strtab` / 符号表 → 可能是符号名或符号数量差异；
   若是 `.text` → 是 codegen 差异；若是 `.rodata` → 可能是字面量或字符串池布局差异；
   若只差在段大小而内容一致 → 可能是对齐填充规则不同。

2. **二分删减**：以 `kernel/vfstest.c` 为输入逐段删减，找出触发差异的最小代码片段。
   （注意 `tools/bisect_module.sh` 是用于二分「fakecc 自身构建失败模块」的，
   与本问题不完全匹配，可能需要手工删减。）

3. **注意工具限制**：fakecc **没有** `-S`（汇编输出）或 IR dump 选项，usage 里只有
   `-g`（DWARF 调试信息）。因此无法直接对比两阶段的汇编。要对比中间表示，
   可能需要临时给 fakecc 加一个 dump 开关，或改用 `-g` 产出的 DWARF 行号信息
   间接定位差异落在哪个函数。

4. **用最小样例替代**：构造若干小 `.c` 文件（覆盖字符串字面量、静态数组初始化、
   `switch`、浮点、大量 `static` 函数、指针转换等），交给 stage 0 与 stage 1
   分别编译，找出能复现差异的最小样例。一旦有 10~30 行的样例，定位会快很多。

5. **注意 `tools/difftest.sh` 不适用于本问题**：它是「同一份源码分别用 gcc 和
   fakecc 编译，比较运行结果」，属于功能正确性差分测试；而本问题是**两个 fakecc
   之间**的字节级差分，需用 `v0/stage2_check.sh` 那类固定点比较。

---

## 6. 影响与优先级

- **优先级：中**。
- **影响面**：任何依赖「fakecc 自举字节一致」做验证的场合都会卡住 —— 例如 fakeos 的
  M5 验收标准（要求 26 个内核模块全部字节一致）、以及 fakecc 自身的 `stage2_check.sh`
  固定点检查。
- **对普通程序的影响：目前没有证据表明有问题，但也不能据 25/26 断言「所有普通程序都正常」。**
  那 25 个文件的一致性只说明 fakeos 用的这批特定源码在两个 fakecc 下 codegen 相同；
  差异既然能被某个输入触发，就可能存在其他触发输入。上游修复后，建议在 fakecc
  自身的 `test/` 用例集上跑一轮 stage0 vs stage1 的批量字节对比，确认覆盖面。
- **修复后**：fakeos 侧无需改动，26 个模块会自动全部一致。

---

## 7. 附：fakeos 侧为了绕过此 bug 所做的临时处理

为不让 CI 因上游缺陷而完全无法验证，fakeos 侧做了如下**临时**处理（若 fakecc 修复，应回退）：

- `user/kbuild.c`：字节不一致时打印 `[kbuild] DIFF` 并**继续执行**，而不是立即失败；
- `scripts/test_qemu.py`：断言改为「26 个模块中 ≥25 个 identical、≤1 个 DIFF」，并要求每个模块都被明确归类（既不放过、也不静默忽略）。

这是一处**如实暴露上游缺陷**的临时放行，不是掩盖 —— DIFF 会在日志中明确打印。若你修复了 fakecc，请告知，我们会把断言恢复为严格的 26/26。
