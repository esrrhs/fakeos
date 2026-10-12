# fakeos 本地 Linux x86-64 工具链环境（UTM）

## 为什么需要它

fakecc **master 只能在 Linux 上驱动 fakeos 构建**。在 macOS 上它能编译成
二进制，但凡编译有跨包导入的文件（`arch.c`、`kprintf.c`、`vfstest.c` …）
就 `rc=140` 崩在 `src/pkg.c:38` 的 `syscall(SYS_getdents64, ...)`
（macOS 无此 syscall，内核发 SIGSYS）。详见
[`docs/fakecc-arm64-port-fopen-flags.md`](fakecc-arm64-port-fopen-flags.md)
以及 CI 上的既定做法：Ubuntu x86-64 + master。

本目录的虚拟机就是把这个 CI 环境搬到本地，用来复现那条正确链路：

```
Linux x86-64 宿主  --master编出-->  fakecc (x86-64)
                  --fakecc 编出-->  fakeos 内核 (x86-64 ELF)
                  --QEMU 跑-->     被测对象
```

## 虚拟机规格

| 项 | 值 |
|---|---|
| 位置 | `~/Library/Containers/com.utmapp.UTM/Data/Documents/fakeos-linux-x86.utm` |
| 后端 | **QEMU**（Apple Virtualization 只支持 aarch64 客户机） |
| 架构 | x86_64（Apple Silicon 上纯模拟，无加速） |
| CPU / 内存 | 4 核 / 8 GB，EPYC 模型 |
| 磁盘 | 由 UTM 向导创建；ISO 与云镜像均已下载并校验（见下） |
| 客户机用户 | `fakeos`，免密sudo，仅允许 SSH 密钥登录 |
| 共享目录 | 宿主**整个** `/Users/mingming/project` → 客户机 `/mnt/host-proj` |

> **共享范围是整个 `project` 目录**（约 12 GB，20 个项目），不是只共享
> fakecc 与 fakeos。这台 VM 是通用开发环境，目录里已有的 `divvy`、
> `fakefish`、`yellowdns`、`domain-socks` 等项目都能直接在客户机里访问。
> 9p 是按需读取、不会预拷贝全部内容，所以目录大不影响启动速度。

## ⚠️ 创建虚拟机：必须用 UTM 界面向导

**不要手工写 `config.plist` 造 `.utm` 包。** 实测 UTM 对非自身创建的包一律报：

```
error com.utmapp.UTM : Failed to create object for file:///.../<name>.utm/
```

然后窗口根本不创建、`utmctl list` 也看不到它。已排除 plist 键值问题——补齐
`Virtualization` 等键组、二分移除各配置组、手搓最小配置，
乃至**以能正常加载的 macOS.utm 配置为模板只改后端与架构**，全部仍然失败。
所以这不是配置写法问题，而是包的身份/授权问题（疑似 UTM 沙盒访问授权或
TCC 记录缺失），命令行绕不过去。

### 路径 A：用安装 ISO（推荐）

在 UTM 里 `+` → **Virtualize** → **Linux**，向导会创建合法的包：

| 向导项 | 填什么 |
|---|---|
| 名称 | `fakeos-linux-x86` |
| 架构 | **Other → x86_64**（不能选 aarch64） |
| 内存 / CPU | 8 GB / 4 核 |
| 镜像 | 挂 `~/utm-images/ubuntu-24.04.5-live-server-amd64.iso` |

装完再配共享目录（见下节）与依赖：

```bash
sudo apt install -y build-essential nasm cmake git python3 qemu-system-x86 binutils
```

ISO 版的代价：要在安装向导里手工设用户名密码，且没有 cloud-init 无人值守。

### 路径 B：用云镜像（挂到已有虚拟机）

如果 UTM 里已有（或向导里选了"使用现有磁盘镜像"），挂
`~/utm-images/noble-server-cloudimg-amd64.img`，并把 CIDATA 种子盘
`~/utm-images/cloudinit/seed.iso` 挂成光驱。此时 cloud-init 会自动建用户、
装依赖、配 9p 挂载，**无需手工配置**。

两个镜像都已校验：

| 文件 | 大小 | SHA256（前 16 位） |
|---|---|---|
| `ubuntu-24.04.5-live-server-amd64.iso` | 4.08 GB | `97f3d7ffb032c3eb…` |
| `noble-server-cloudimg-amd64.img` | 597 MB | `6a81c37564db9b1e…` |

## 启动后

连进客户机（SSH，或直接在控制台登录）：

```bash
ls /mnt/host-proj            # 应看到 fakecc、fakeos 及其余共享项目
uname -m                     # x86_64
nasm -v && cmake --version
```

> 走 ISO 路径时 9p 共享要手动配：在 UTM 设置里把宿主
> `/Users/mingming/project` 加入共享目录，然后在客户机执行
> `sudo mkdir -p /mnt/host-proj && mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/host-proj`

## 通用开发环境，不只服务 fakeos

共享的是整个 `project` 目录，所以任何需要**真实 Linux x86-64** 的任务都能在这里做，
而不必局限于 fakeos。典型用途：

- 需要 glibc / Linux syscall 语义的项目（macOS 上行为不同或根本没有）
- 要在 x86-64 上跑、而宿主是 arm64 的构建
- 依赖 Linux 专有工具链（`ld`、内核头文件、systemd 服务等）
- 复现只在 Linux 上出现的问题（本机多次遇到：fakecc master 的 `getdents64` 崩溃）

客户机里已预装 `build-essential` / `cmake` / `git` / `python3` / `nasm` / `binutils` /
`qemu-system-x86`。其他语言工具链（Go、Rust、Node 等）按需 `sudo apt install` 即可。

## 共享目录为什么是 9p 而不是 virtiofs

virtiofs 是 Apple Virtualization 后端专有的，且挂载tag 硬编码为 `share`；
而本机客户机是 **x86-64，只能用 QEMU 后端**，共享走 **9p**：

```bash
mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/host-proj
```

cloud-init 已配置为开机自动挂载（`/etc/systemd/system/mnt-host-proj.mount`，
带 `nofail` 以免挂载失败卡住启动）。

### UTM 把共享目录存在哪里（容易踩坑）

**不在 `config.plist` 里**，而在：

```
~/Library/Containers/com.utmapp.UTM/Data/Library/Preferences/com.utmapp.UTM.plist
  → Registry.<VM-UUID>.SharedDirectories[]
```

每台 VM 各有一条记录，元素含一个 macOS security-scoped `Bookmark`。
VM 的 `config.plist` 里的 `Sharing.DirectoryShares` 同样会被读取，
但用 `plistlib` 直接查 `config.plist` 是**看不到** macOS 那台已配置的共享的——
这也是「查 plist 找不到共享 → 误判没共享」这个坑的来源。

若要手工增删共享，改偏好文件里的 `Registry.<UUID>.SharedDirectories`，
`Bookmark` 字段直接复用同一宿主路径已有条目即可（书签是宿主侧的，
与哪个客户机消费它无关）。⚠️ **改之前先 `quit app "UTM"`**，
否则 UTM 退出时会用内存里的旧值覆盖你的修改。

## ⚠️ 不要在共享目录里直接构建

宿主是 **APFS**（大小写不敏感），客户机是 **ext4**，符号链接/权限/大小写
语义都不同。在 `/mnt/host-proj` 里构建**可能产出不一致的字节**，
会直接破坏「26个内核对象字节一致」这个断言。

正确做法：**源码从 9p 读，构建在客户机本地盘做**。

```bash
cp -a /mnt/host-proj/fakecc /home/fakeos/fakecc
cp -a /mnt/host-proj/fakeos /home/fakeos/fakeos
cd /home/fakeos/fakecc && git checkout master      # 关键：用 master，不是 WIP 分支
cmake -S . -B build && cmake --build build --parallel
cd /home/fakeos/fakeos && sudo apt install -y qemu-system-x86
FAKECC_SRC=/home/fakeos/fakecc make test
```

这套流程天然满足「产物落本地盘」：
`tools/m5_build_fakecc.sh` 的 `SCRATCH=/tmp/fakeos-m5-fakecc` 是绝对路径，
而 `BUILD_DIR := build` 是相对路径 → `build/` 与 scratch 都在本地 ext4。

## 重新生成 cloud-init 种子盘

改了 `~/utm-images/cloudinit/user-data` 后：

```bash
# 生成 ISO（本机无 cloud-localds，用 pycdlib）
/Users/mingming/.workbuddy/binaries/python/envs/default/bin/python3 \
    tools/make_cloudinit_seed.py \
    ~/utm-images/cloudinit/user-data \
    ~/utm-images/cloudinit/seed.iso
# 然后替换虚拟机里的 seed 盘并重挂（虚拟机需处于关闭状态）
cp ~/utm-images/cloudinit/seed.iso \
   ~/Library/Containers/com.utmapp.UTM/Data/Documents/fakeos-linux-x86.utm/Data/C96815CA-F9C3-4455-A66D-5983F2BF1BB2.img
```

## 性能预期

x86-64 客户机在 Apple Silicon 上是**纯软件模拟**，CPU 密集步骤（fakecc 全量
自举、26 个模块重编）会明显比 CI 慢。`make test` 在本机 QEMU TCG 下约 2 分钟，
虚拟机里再套一层模拟，预估更久。这不是环境配置问题——架构上就是双层模拟。

需要快速迭代时，用 CI（每次push 自动跑，约 1 分钟）；需要本地完整闭环时用这台 VM。
