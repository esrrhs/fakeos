# fakecc arm64-darwin 移植收尾项：open(2) 标志位未做 per-target 拆分，Linux 目标的 fopen("w"/"a") 全部失效

**状态**：仅存在于 WIP 移植分支 **`arm64-macos-backend`**（调查时 HEAD `d1b20401`，2026-10-11）；
**不是 master 缺陷**，不建议作为 master bug 上报。这是该分支在跨内核（Linux ↔ Darwin）ABIBI 拆分过程中的一处**漏网之项**，
性质与该分支 10-10 自行修复的 mmap 标志位问题（`2a2c34ca`）完全同类。
**发现时间**：2026-10-11
**报告来源**：[fakeos](https://github.com/esrrhs/fakeos) 项目 M5 在 arm64 macOS 宿主上用该 WIP 分支构建 Linux stage1 fakecc 时实证
**影响范围**：用该分支构建的 **Linux 目标**程序（含自举产出的静态 Linux x86-64 stage1 fakecc）中，
`fopen(path, "w"/"wb"/"a"/"ab"/"w+"/"a+")` 对不存在的文件一律失败，对已存在文件语义也错（详见第 4 节）。
Darwin 目标行为正确，**不能用「把共享文件改回 Linux 值」来修——那会重新打破 Darwin**；
正确修法见第 6 节（该分支已有现成模式）。

> 结论可信度：
> - 源码级：分支 HEAD `d1b20401` 的共享文件 `runtime/stdio.c:73-82` 硬编码 Darwin 值；
>   per-target 选择机制（`src/pkg.c` 的 `c_file_matches_target`）、syscall 号后端翻译
>   （`src/cg64.c`）、per-target 常量先例（`__fakecc_map_*` in `stdlib_{darwin,linux}.c`）均已核实；
> - 运行级：fakeos M5 用未修补的该分支构建 Linux stage1，kbuild 稳定失败
>   （`fakecc: cannot write '/tmp/ping'`，exit=256）；按 Linux 值绕行后，`make test`
>   连续多轮 98/98、26/26 内核对象字节一致（含连续 3 轮干净通过）。

---

## 1. 现象（Linux 目标侧实证）

fakeos M5 的 kbuild stage a 会在 OS 内用 `/bin/fakecc`（该分支自举产出的静态 Linux ELF）
编译并运行冒烟程序 `ping.c`：

```
[kbuild] stage a: compiler/exec chain on ping.c
fakecc: cannot write '/tmp/ping'
[kbuild] FAIL exit=256
```

- 报错来自 fakecc 自身（`src/emit.c:673`、`src/link.c:2898/3021`：`fopen()` 返回 0 后 `exit(1)`；
  fakeos `wait4` 状态为 `1<<8 = 256`）。
- `/tmp/ping` 此前不存在。读路径（`"r"/"rb"`，flags=0）完全正常，所以 fakecc 读源码、
  读输入都没问题，只有输出打开失败，隐蔽性强。
- 上游 `origin/master`（`fa8eb615`）同位置是 Linux 值（`64 | 512` / `64 | 1024`），无此问题。

---

## 2. 该分支已有的跨内核机制（说明 fopen 应该归位到哪里）

移植分支处理「同一套 C runtime、两个内核 ABI」已有两层机制，且代码注释写明了契约：

### 2.1 syscall 号：后端翻译，runtime 保持 Linux 味道

`src/cg64.c` 的 `emit_syscall()` 对立即数 syscall 号做 Linux→Darwin 映射，
注释原文（约 3640 行）：

> The builtin runtime is written against Linux x86-64 syscall numbers, but this backend
> talks to Darwin … The sources predate the arm64 port and are shared with x86,
> so **translate here rather than in the runtime**.

```c
static int64_t c64_darwin_syscall(int64_t num) {
    switch (num) {
    case 0:  return 3;   /* read  */
    case 1:  return 4;   /* write */
    case 2:  return 5;   /* open  */
    ...
```

**该翻译只覆盖 syscall 号，不覆盖任何参数标志位**（全后端 grep 无 O_CREAT 等
flag 转换）。也就是说，调用点传进来的 flags 按字面进入 Darwin 内核。

### 2.2 跨内核常量：per-target 文件，按文件名后缀选择

10-05 的 `fead10a3 feat(pkg): select per-target source files by filename suffix`
给包加载器加了后缀过滤（`src/pkg.c` 的 `c_file_matches_target()`：
`*_linux.c` / `*_darwin.c`（及 `_amd64`/`_arm64`）按当前 target 二选一）。

10-10 的 `2a2c34ca fix(runtime): use each target's mmap flags for anonymous mappings`
遇到的问题与本次**完全同类**，其 commit message 写道：

> MAP_ANONYMOUS and MAP_NORESERVE do not share bit values across the two kernels …
> the constants the runtime carried named an undefined bit to Darwin and asked for no MAP_ANON at all

修复方式是在 per-target 文件里各放一份常量，共享代码只引用变量：

- `runtime/stdlib_darwin.c`：`__fakecc_map_private_anon = 0x0002 | 0x1000;`
- `runtime/stdlib_linux.c`：`__fakecc_map_private_anon = 0x0022;`

`malloc.c` / `sanitizer.c` 等共享文件不再出现裸 mmap 常量。

**fopen 的 open 标志位应当走同一条路**（或按 2.1 的契约在后端翻译，见第 6 节），
而现状是写死在共享文件 `runtime/stdio.c` 里。

---

## 3. 根因：711e6c77 在共享文件中硬编码 Darwin 值

10-05 22:52 的 `711e6c77 fix(runtime): use the real O_CREAT/O_TRUNC/O_APPEND in fopen`：

```diff
+    /* O_CREAT is 0x200 and O_TRUNC is 0x400 on both Linux and Darwin; the
+     * 0x40 used here belongs to O_NOCTTY, so "w" created nothing ... */
     if (mode[0] == 'w') {
-        *flags = (plus ? 2 : 1) | 64 | 512;
+        *flags = (plus ? 2 : 1) | 512 /* O_CREAT */ | 1024 /* O_TRUNC */;
     }
     if (mode[0] == 'a') {
-        *flags = (plus ? 2 : 1) | 64 | 1024;
+        *flags = (plus ? 2 : 1) | 512 /* O_CREAT */ | 8 /* O_APPEND */;
     }
```

两个事实问题：

1. 注释的核心论断错误。0x200/0x400/0x008 **只是 Darwin 的值**；Linux 上
   0x40 才是 O_CREAT，0x400 是 O_APPEND，0x40 是合法的创建位而非 O_NOCTTY（Linux 的
   O_NOCTTY 是 0x100）。
2. 落点错误。修改发生在**与 x86/Linux 共享**的 `runtime/stdio.c`，且时间点在
   per-target 后缀机制（`fead10a3`，同日上午）落地**之后**约 12 小时。该提交在 Darwin
   上修好了它的 e2e（`printf_n_a` 等），同时把 Linux 目标打回了旧行为。

这是移植中的临时迁就，不是最终设计——分支随后（10-10）为同类问题建立的
per-target 模式，而 open 标志位没有跟着迁移。

---

## 4. 两套 ABI 的数值与 Linux 目标下的实际后果

| 标志位 | Linux（`asm-generic/fcntl.h`，八进制） | Darwin（`sys/fcntl.h`） | 当前共享 stdio.c 用值 |
|---|---|---|---|
| `O_CREAT` | `0100` = **0x040** | **0x200** | 512（Darwin）❌ Linux |
| `O_TRUNC` | `01000` = **0x200** | **0x400** | 1024（Darwin）❌ Linux |
| `O_APPEND` | `02000` = **0x400** | **0x008** | 8（Darwin）❌ Linux |
| `O_WRONLY`/`O_RDWR` | 1 / 2 | 1 / 2 | 1 / 2 ✅ |

把 Darwin 值交给 Linux 内核后（0x008 在 Linux open 标志位中无定义，被忽略）：

| fopen 模式 | 实际 flags | Linux 解码 | 不存在文件 | 已存在文件 |
|---|---|---|---|---|
| `"w"/"wb"` | 0x601 | O_WRONLY|O_TRUNC|O_APPEND，**无 O_CREAT** | **ENOENT** | 截断（意外） |
| `"w+"` | 0x602 | O_RDWR|O_TRUNC|O_APPEND，无创建 | **ENOENT** | 截断 |
| `"a"/"ab"` | 0x209 | O_WRONLY|O_TRUNC，**无创建无追加** | **ENOENT** | **被截断而非追加（静默丢数据）** |
| `"a+"` | 0x20a | O_RDWR|O_TRUNC | **ENOENT** | **被截断** |
| `"r"` 系列 | 0 / 2 | 正确 | 正确 | 正确 |

fakeos ramfs 是 Linux 标志位语义子集（`fs/ramfs.c:57-59` 常量；`:520-528`
无 O_CREAT 即返回坏句柄；`:541`/`:544` 截断/追加），行为与上表逐项一致，
故 stage1 fakecc `fopen("/tmp/ping","wb")` 必败。

---

## 5. 为什么分支自己还没发现

- 开发宿主是 macOS：分支 e2e 在 Darwin 上编译+运行，Darwin 内核恰好认 0x200/0x400/0x008，
  所以 `711e6c77` 之后 Darwin 侧是绿的。
- `origin/arm64-macos-backend` 当前**没有任何 CI run、也没有开 PR**（API 已核实），
  分支上的 Linux 目标 e2e 处于无人执行状态。
- 现成的 Linux 侧用例其实已经存在：`test/e2e/cases/runtime/stdio_file.c`
  第一步就是 `fopen("/tmp/fakecc_stdio_test.dat", "w")` 创建此前不存在的文件——
  只要在该分支上跑一轮 Linux e2e（或 bootstrap suite），它就会以退出码 1 报红。

---

## 6. 建议修法（二选一，推荐方案 A）

### A. 走分支自己的 per-target 模式（与 mmap 完全对齐，推荐）

1. 在 `runtime/stdlib_linux.c` 增加：
   ```c
   int __fakecc_o_creat = 0x040;
   int __fakecc_o_trunc = 0x200;
   int __fakecc_o_append = 0x400;
   ```
   在 `runtime/stdlib_darwin.c` 增加对应 Darwin 值 `0x200 / 0x400 / 0x008`，
   并在注释中互相引用，照抄 `__fakecc_map_*` 的写法。
2. `runtime/stdio.c` 的 `parse_fopen_mode()` 改为 OR 这些变量，删掉错误注释。
3. 收益：与分支现有架构一致；未来第三个 OS 只需再加一个后缀文件；
   不依赖后端对参数做模式识别。

### B. 按 2.1 节的契约在 arm64 后端翻译 open flags

在 `src/cg64.c` 的 open 路径（`__syscall(2,...)`）上，对**立即数 flags 做常量折叠翻译**
（该后端对 syscall 号的翻译已经依赖「调用点都是常量」这一前提，见其注释）。
缺点：open 标志位的重排是多 bit 映射（Linux O_TRUNC 0x200↔Darwin O_CREAT 等交叉换位），
比 syscall 号表更容易写错，且与 2.2 的既有先例不统一，不推荐作为首选。

### 不要做的事

- **不要把 `runtime/stdio.c` 直接改回 `64 | 512` / `64 | 1024`**：这会让 Darwin 目标的
  fopen 重新失效（`711e6c77` 修的就是这个），等于把移植成果回滚。
- 也不建议给 `fopen` 加 darwin-only 的独立实现文件绕过——`stdio.c` 整体是共享的，
  只有三个整型常量需要分目标，per-target 变量足够。

建议顺手补一条分支 CI：Linux runner 上至少跑 `stdio_file` 等 runtime e2e，
Darwin runner 跑现有 darwin e2e，两侧分别守门。

---

## 7. fakeos 侧的临时绕行与验证

fakeos 在不动上游源码的前提下，在构建管线对 scratch/ staged 的 `runtime/stdio.c`
做等价于「Linux 目标值」的字符串改写（gated：仅匹配 Darwin 字面量；
对 master 完全 no-op）：

- `tools/m5_build_fakecc.sh` 步骤 1b：改 scratch 构建树（影响静态 Linux stage1 `/bin/fakecc`）；
- `tools/m5_stage.sh`：改 staged 入 OS 的 `/src/runtime/stdio.c`（影响 OS 内编译用户程序时链接的 runtime）。

这等于是在 fakeos 侧**单独取了 Linux 那份 per-target 值**，上游按第 6 节 A 方案落地后，
这两个 gated 步骤即可删除。

改写脚本在替换常量的同时**一并改写了那段注释**。原文声称
「O_CREAT is 0x200 and O_TRUNC is 0x400 on both Linux and Darwin」——
而这句论断恰恰是本次要绕开的东西，只换常量会留下一段与代码自相矛盾的注释，
误导后续维护者。新注释改为说明「已由脚本改写为 Linux 值」及其原因，
并保留 `assert` 强制两条替换都必须命中，任一未命中即构建失败。

验证（2026-10-11 首次，2026-10-12 复跑）：绕行后 `make test` 全绿 —— 98/98 断言、
26/26 内核对象字节一致、34 帧/410304 字节重建镜像、二次引导 12 项检查全过；
未绕行时 kbuild 稳定复现第 1 节的失败。10-12 的复跑在**清空 `build/` 且删除
`/tmp` scratch** 的条件下执行，强制重建 clang stage0，确认两处改写（scratch 与
staged）都真实生效，而非命中缓存。

> **对 CI 的影响：无。** CI 的 `.github/workflows/ci.yml` clone 的是 fakecc
> **master**，而 `origin/master`（`fa8eb615`）的 `runtime/stdio.c` 本就是 Linux 值
> `64 | 512` / `64 | 1024`（第 1 节已核实「master 同位置无此问题」）。
> 两处改写的 `grep -q` 前置条件对 master 不成立，因此整段逻辑在 CI 上是 no-op。
> 这些改动只对用 WIP `arm64-macos-backend` 分支做本地构建的开发者生效。

---

## 8. 参考

- 分支机制：`src/pkg.c` `c_file_matches_target()`（per-target 后缀）；
  `src/cg64.c` `c64_darwin_syscall()` / `emit_syscall()` 注释（runtime 按 Linux ABI、后端翻译号）
- 同类先例：提交 `2a2c34ca`（mmap flags 拆分）、`fead10a3`（后缀选择机制）
- 问题引入：提交 `711e6c77`（2026-10-05），现状在 HEAD `d1b20401` 仍未拆分
- Linux 常量：`include/uapi/asm-generic/fcntl.h`；Darwin 常量：`/usr/include/sys/fcntl.h`
- Linux 侧现成用例：`test/e2e/cases/runtime/stdio_file.c`（`fopen("w")` 创建新文件）
- fakeos：`fs/ramfs.c:57-59,513-547`；`src/emit.c:673`、`src/link.c:2898,3021`
