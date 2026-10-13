# k-vm 详细使用手册

基于 Fabrice Bellard 的 TinyEMU 2019-12-21，使用 Bazel 构建的轻量虚拟机项目。
仅支持 RISC-V RV32/RV64/RV128 软件解释器、VirtIO 设备、9P 文件系统，不需要 `/dev/kvm`。
默认启用 SLIRP 用户态网络；Linux 另提供可选 TAP。支持 HTTP/HTTPS 配置加载、远程磁盘和远程文件系统。
自动启动会同步宿主机时间，确保客户机 HTTPS 证书验证和 npm 更新可用。

## 沙盒模式

沙盒模式自动将**执行时的当前目录**可写映射到客户机 `/work`，并在该目录启动 shell 或 Pi。
网络默认使用无需 root 的 SLIRP；根磁盘修改默认保存在内存中，退出即丢弃。
将构建产物 `bazel-bin/k-vm` 放到 PATH 后，可以从项目目录直接运行：

```sh
k-vm sandbox                            # 进入 /work 下的交互式 shell
k-vm sandbox --agent pi                  # 自动启动 Pi
k-vm sandbox --agent pi -- --model MODEL
```

工作目录的修改会写回宿主机，包括 agent 创建和修改的文件。整个当前目录都可见，
包括项目自己的隐藏文件；不会额外导出宿主机 HOME 或其他项目。
请直接运行可执行文件；`bazel run` 的运行目录可能不同于你当前所在目录。

需要旧版快速启动参考镜像时（Linux 4.15 + BusyBox 1.21.1，仅支持 shell，需要 `debugfs`）：

```sh
bazel run //tools:prepare_linux_image
```

Pi 使用现代 Debian 13（trixie-slim）RISC-V 镜像；BusyBox 提供引导和救援工具。
镜像准备时自动选择 kernel.org 的最新稳定内核（排除 RC），可以用 `--kernel-version X.Y.Z` 固定版本。
本次验证镜像为 Linux 7.2.10、OpenSBI 1.9、Debian 13、Node.js 24.21.0、Pi 1.2.0。
OpenSBI 1.9 包含本地 HTIF MMIO `volatile` 修补，避免关机写入被编译器优化掉。
准备工具交叉编译 Linux 和 OpenSBI，安装 Node.js RV64 社区构建与 Pi；下载文件检查发布者 SHA256。
构建时需要 Docker、RV64 binfmt/QEMU user、交叉编译器、`mke2fs` 和网络：

```sh
sudo apt-get install gcc-riscv64-linux-gnu g++-riscv64-linux-gnu qemu-user-static \
  binfmt-support device-tree-compiler e2fsprogs make bc bison flex libssl-dev libelf-dev
# Docker 必须能运行 linux/riscv64 容器；在原生 RV64 主机上无需 QEMU user。
bazel run //tools:prepare_agent_image
```

Docker 上下文只包含 Dockerfile、客户机引导脚本和公开的 Node.js 归档，不包含项目或认证文件。
Docker 仅用于准备镜像，沙盒运行通过 TinyEMU 软件解释器完成，不要求 Docker 或硬件虚拟化。
shell 与 Pi 默认均查找 `~/.local/share/k-vm/images/pi-riscv64`；设置 `XDG_DATA_HOME` 时使用对应目录。
`K_VM_IMAGE` 或 `--image` 可以覆盖路径。RV32/RV128 支持保留给旧镜像，现代 Pi 镜像仅使用 RV64。

### 更新客户机内的 Pi

```sh
k-vm update                              # 更新默认 Pi 镜像到 npm latest
k-vm update --image /path/to/image
k-vm update --version 1.2.0              # 指定版本
```

更新会克隆根磁盘，在客户机中执行 `npm install -g @earendil-works/pi-coding-agent@VERSION`，
验证 `pi --version` 并成功关机后才替换原磁盘；失败或中断保留原镜像。更新不会挂载当前项目、
宿主机 HOME 或 Pi 配置，只暴露只读启动目录和单独的结果目录。运行中的镜像会被文件锁保护，
请退出沙盒后再更新。需有克隆磁盘的临时空间；支持 reflink 时不需要完整复制；其他文件系统按稀疏块复制。
本次真实 Pi 更新安装约 5 分钟（软件解释器）；已验证更新后重新启动、Pi CLI、HTTPS 和正常关机。
`update` 仅更新 Pi 及其 npm 依赖；内核、Node.js 和系统包需要重新准备镜像。
不要在默认临时磁盘模式中使用 `npm install` 来保存更新，退出沙盒后这种修改会丢弃。

仅将所选 agent 的必要配置复制到私有只读启动映射，再导入客户机临时 HOME：

| 模式 | 默认配置来源 | 导入文件 |
| --- | --- | --- |
| shell | 无 | 无 |
| pi | `$PI_CODING_AGENT_DIR` 或 `~/.pi/agent` | `auth.json`、`settings.json`、`models.json` |

用 `--config-dir DIR` 指定配置来源。不会带入宿主机历史、会话、缓存、skills、扩展或整个
配置目录，不转发宿主机环境变量中的密钥。配置使用启动时的副本，客户机对配置的修改不会
写回宿主机；默认磁盘快照模式下新会话记录也随沙盒退出而丢弃。配置中引用的外部路径、
MCP 程序或插件仍需在客户机安装和配置，不能假定宿主机路径在客户机可用。
如果所有可用临时目录都位于可写映射内（例如从 `/` 启动），程序会拒绝启动，
避免从工作映射间接修改私有配置。

已准备的本地 RISC-V 示例可直接运行：

```sh
./bazel-bin/k-vm sandbox --image images/linux-riscv64
```

原有 `k-vm config.cfg` 虚拟机入口仍可使用；手工挂载模式使用 `--no-workspace`：

```sh
./bazel-bin/k-vm sandbox --no-workspace --image images/linux-riscv64 \
  --map work="$PWD":rw
# 客户机内：
mkdir -p /mnt/work
mount -t 9p -o trans=virtio,version=9p2000.L work /mnt/work
```

支持重复 `--map`，总计最多 4 个 9P 文件系统（自动模式中 `work`、`kv_launch` 占用两个，标签保留）。单个文件映射示例：

```sh
./bazel-bin/k-vm sandbox --image images/linux-riscv64 \
  --map input=/absolute/path/input.txt
```

客户机将 `input` 挂载到 `/mnt/input` 后，只能看到 `/mnt/input/input.txt`，看不到宿主机同级文件。
默认只读由宿主机强制执行，不依赖客户机的挂载参数。

| 选项 | 行为 |
| --- | --- |
| `--agent shell\|pi` | 默认 shell，自动在 `/work` 执行 |
| `--config-dir DIR` | 仅导入所选 agent 的配置文件白名单 |
| `--no-workspace` | 禁用自动工作目录和 agent 启动，手工启动客户机 |
| `--image DIR_OR_CFG` | 本地目录中的 `vm.cfg`，或已有本地 TinyEMU 配置 |
| `--map TAG=PATH[:ro\|rw]` | 映射目录或单个文件；默认只读 |
| `--network user` | 默认 SLIRP 用户态网络 |
| `--network none` | 不创建客户机网卡 |
| `--network tap:tap0` | 使用已配置的宿主机 TAP |
| `--disk-mode snapshot` | 默认模式；磁盘修改仅保存在内存中 |
| `--disk-mode ro` | 磁盘只读 |
| `--disk-mode rw` | 修改直接写回本地镜像 |
| `--memory 256` | 内存大小，范围 16–4096 MB |
| `--append 'kernel arguments'` | 追加客户机内核参数 |

沙盒中 **Ctrl+C** 默认传给客户机，用于中断内部前台命令（如 Pi），不会退出沙盒。
退出沙盒请使用 **Ctrl+A 后按 X**，退出时会恢复宿主机终端。
自动模式下 agent 结束或被中断后进入 shell，沙盒保持运行；此时 `exit` 结束 shell 并关闭客户机。
手工模式的 `exit` 仅退出当前 shell。某些 Linux 镜像执行 `poweroff` 后会停在关机状态，
此时仍可用上述快捷键结束模拟器。

### 本地镜像准备

镜像目录约定：

```text
images/linux-riscv64/
  vm.cfg
  bbl64.bin
  kernel-riscv64.bin
  root-riscv64.bin
```

`vm.cfg` 保留上游配置语法；示例见 [examples/sandbox/vm.cfg](../examples/sandbox/vm.cfg)。
沙盒启动只接受本地配置、BIOS、内核、initrd、raw 磁盘和本地映射，不会启动时下载资源。
QCOW2、VMDK、VHDX 等容器格式需要先用 `qemu-img convert -O raw` 转换。
镜像格式可转换，但其中的客户机操作系统仍需兼容 TinyEMU 的 CPU、启动协议与 VirtIO 设备；
只接受 RISC-V 机器配置；现代 RV64 镜像使用 OpenSBI 固件和 Linux Image，根磁盘驱动内置。

克隆代码后，或需要重新准备镜像时，可运行一次准备工具：

```sh
bazel run //tools:prepare_linux_image -- --output "$PWD/images/linux-riscv64"
```

工具下载上游 2018-09-23 Linux/BusyBox 镜像包（约 7.7 MB），检查固定 SHA256，
只复制指定的常规镜像文件，并拒绝覆盖已有目录。之后每次启动均离线使用本地文件。
使用已下载的可信归档包时：

```sh
python3 tools/prepare_linux_image.py --archive /absolute/path/diskimage-linux-riscv-2018-09-23.tar.gz \
  --output /absolute/path/new-image --arch 64
```

可指定 `--arch 32` 准备 RV32 示例。`images/` 已加入 `.gitignore`，镜像不纳入源码。
Linux 和 BusyBox 使用各自上游许可证，示例用于验证和开发。

### 启动速度和隔离范围

启动路径直接读取本地资源，不经过 HTTP 下载、不复制整个根磁盘；内存磁盘写入层按写入扇区分配。
在当前 Debian 12 x86_64 宿主机，本地 128 MB 内存的示例 RISC-V Linux 启动到 shell 约 **0.22 秒**；
这是本机测量，实际速度取决于镜像、宿主机和客户机初始化过程。
现代 Debian/Pi 镜像不是该 0.22 秒参考镜像；当前宿主机启动到现代 shell 约 5 秒，Pi 初始化还需要额外时间。
本版本提供快速冷启动和临时磁盘写入层，尚不提供 CPU／内存状态快照恢复。

Linux 文件映射后端要求 **Linux 5.6+ 和可访问的 `/proc/self/fd`**，使用目录描述符和 `openat2`
限制路径范围，阻止 `..` 越界及所有符号链接跟随；特殊文件不允许打开，单文件映射不暴露同级文件。
映射不支持创建符号链接、硬链接、设备节点或改变宿主机所有者。
沙盒不导出整个宿主机根目录，但映射范围内已有的硬链接与内容会按显式授权暴露；请选择所需的最小目录。
`--network user` 允许客户机主动访问宿主机和外网，不是网络访问白名单。
当前隔离基于虚拟机及受限文件映射，没有额外进程 namespace/seccomp 隔离，
旧版 TinyEMU/SLIRP 也未经本项目安全审计；不要将其描述为已审计的恶意代码安全边界。

可重复执行真实 Linux 的映射、网络、磁盘保护及退出验证：

```sh
python3 tests/linux_sandbox_check.py --image images/linux-riscv64
python3 tests/agent_workspace_check.py --image images/linux-riscv64
python3 tests/agent_dispatch_check.py --image images/linux-riscv64
python3 tests/pi_update_check.py --image images/linux-riscv64
python3 tests/agent_workspace_check.py --image images/pi-riscv64
python3 tests/pi_runtime_check.py --image images/pi-riscv64
```

agent_dispatch_check 使用客户机内的测试程序验证调度和配置白名单，不代表真实 Pi 运行验证。
网络验证只连接本地 HTTP 服务，不需要外网；本地示例镜像需提前准备。
普通 `bazel test //...` 使用生成的最小客户机，不依赖 Linux 下载文件。
`pi_runtime_check` 验证真实 Pi、Node 和 npm registry HTTPS，需要准备现代镜像和外网。

### 宿主机兼容

| 宿主机 | 运行方式 | 文件映射与网络 |
| --- | --- | --- |
| Linux 64 位 | 原生可执行程序 | `openat2`；SLIRP 或 Linux TAP |
| macOS 13+，Apple Silicon / Intel | 原生构建 | `openat` + `O_NOFOLLOW_ANY`；SLIRP |
| Windows 10/11 x64 | 原生 `k-vm.exe`，MinGW-w64 构建 | Win32 HANDLE / 9P；Winsock / SLIRP |

文件访问已拆成 Linux、macOS/POSIX 和 Win32 后端，Bazel 按目标平台选择；配置导入也复用这些后端。
两者均拒绝路径穿越、符号链接访问、设备/FIFO 等特殊文件，并保留只读策略；macOS 在打开后
校验 inode/device，验证后才截断文件。Linux 使用 `O_PATH` 固定文件后重开，因此还能避免
检查和打开之间被恶意宿主进程替换成设备而调用设备驱动。macOS 没有等价的重开接口，
该设备替换竞态仍有局限；不能把 macOS 后端视作与 Linux 等同的内核隔离机制。
硬链接到映射外文件、宿主机主动移动已打开目录等行为也不构成这里承诺的隔离边界。

已在 Linux 运行全部测试和真实客户机检查，并交叉编译 macOS ARM64 / Intel 本地代码；
当前环境没有 macOS/Windows 实机。`.github/workflows/host-compatibility.yml` 提供两种 macOS
架构及 Windows 的原生构建、测试和客户机文件映射/终端/更新检查；这里不将尚未运行的 CI 当作已验证结果。

macOS 构建（Xcode Command Line Tools 和 Homebrew）：

```sh
xcode-select --install
brew install bazelisk openssl@3 e2fsprogs
bazel build //:k-vm \
  --copt="-I$(brew --prefix openssl@3)/include" \
  --linkopt="-L$(brew --prefix openssl@3)/lib"
./bazel-bin/k-vm sandbox --image /path/to/pi-riscv64 --agent pi
```

运行现有镜像无需 Docker。现代镜像准备工具目前使用 Linux 交叉编译工具链；
可在 Linux 构建镜像后将整个目录复制到 macOS 或 Windows。旧 shell 参考镜像可在 macOS 准备，
先将 Homebrew `e2fsprogs` 的 `sbin` 加入 PATH。SDL 构建另需 `sdl12-compat` 的 include/lib 路径。

Windows 原生构建使用 MSYS2 的 **MINGW64** 环境和 GCC（MSVC 不支持模拟器所需的 `__int128`）：

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-python
python tools/build_win32.py --test
```

项目也提供原生 Windows Bazel 配置，安装 Bazelisk 后在 PowerShell 中执行：

```powershell
$env:BAZEL_SH = 'C:/msys64/usr/bin/bash.exe'
bazel test --config=win32 //...
.\bazel-bin\k-vm.exe sandbox --image C:\vm-images\pi-riscv64 --agent pi
```

Linux 上可交叉构建同一 EXE：`python3 tools/build_win32.py --cc x86_64-w64-mingw32-gcc`。

产物 `out/win32/k-vm.exe` 可直接从 PowerShell/cmd 运行，运行时不需要 MSYS2、WSL、
Hyper-V 或 `/dev/kvm`。将 EXE 放入 PATH，或与 `tools/k-vm.cmd` 放在同一目录：

```powershell
k-vm.exe sandbox --image C:\vm-images\pi-riscv64 --agent pi
k-vm.exe sandbox --image C:\vm-images\pi-riscv64 --agent shell
k-vm.exe update --image C:\vm-images\pi-riscv64
```

当前 Windows 工作目录自动映射到 `/work`，路径和参数支持 Unicode。
Pi 只复制 `auth.json`、`settings.json`、`models.json`；不会导入历史和 sessions，shell 不导入配置。
默认用户目录取 `HOME` 或 `USERPROFILE`，也可通过 `--config-dir` 指定。
`Ctrl+C` 发给内部命令；`Ctrl+A` 后按 `x` 退出沙盒，shell 中 `exit` 正常关机。
原生终端支持 VT 输出、Unicode 输入和窗口大小变化，建议使用 Windows Terminal。

Win32 映射拒绝 reparse point（符号链接/junction）、ADS、DOS 设备名及路径穿越，
持有目录句柄防止访问期间祖先目录被移动。Windows ACL 决定宿主实际访问权限；
客户机 POSIX mode 在映射生命周期内模拟，支持 `chmod +x`，不会成为持久的 NTFS 权限。
私有启动包只授权当前用户和 SYSTEM。更新采用稀疏复制、独占文件锁和 `ReplaceFileW`，
客户机退出且所有磁盘关闭后才提交；失败或正常取消保留原镜像。
同一用户的恶意宿主程序在身份检查与替换之间修改文件命名空间不属于隔离保证。
建议镜像置于支持锁、稀疏文件和原子替换的本地 NTFS 文件系统。

Windows 支持本地镜像和客户机 SLIRP 网络；TAP、SDL 和旧版宿主 HTTP 镜像/文件系统暂不支持。
RISC-V 软件模拟无需硬件虚拟化。现代镜像在 Linux 上准备后复制整个目录即可使用。

## 构建与测试

构建面向 **64 位 Linux / macOS / Windows（编译器支持 `__int128`）**。Linux 已在 Debian 12 x86_64 验证。
安装 GCC、Python 3、Bazelisk，以及系统开发库：

```sh
sudo apt-get install build-essential python3 libcurl4-openssl-dev libssl-dev
bazel build //...
bazel test //...
bazel run //:k-vm -- -h
```

`.bazelversion` 固定 Bazel 7.4.1，由 Bazelisk 选择该版本。`MODULE.bazel` 定义模块
`k_vm`（Bazel 模块名不能包含连字符），可执行程序名为 `k-vm`。
当前 curl、OpenSSL 和可选 SDL 使用系统头文件与动态库，构建并非完全 hermetic；
SLIRP 源码随项目提供。首次构建需联网下载 Bazel 及 Bazel 工具链规则。

构建产物：

- `bazel-bin/k-vm`：虚拟机主程序。
- `bazel-bin/tools/build_filelist`：生成远程文件系统文件列表。
- `bazel-bin/tools/splitimg`：将磁盘镜像拆成 HTTP 后端所用的数据块。

默认是终端版本。需要图形界面时安装 **SDL 1.2**（不是 SDL 2）：

```sh
sudo apt-get install libsdl1.2-dev
bazel build --config=sdl //:k-vm
bazel run --config=sdl //:k-vm -- /absolute/path/to/graphical.cfg
```

优化构建仍保留断言：上游源码的一些文件系统操作位于 `assert()` 中，关闭断言会破坏功能。
RV32/RV64/RV128 解释器分别用私有 `MAX_XLEN` 宏编译，共享接口统一使用
`CONFIG_RISCV_MAX_XLEN=128`，不会把三种解释器拼成单一错误配置。

## 目录结构

```text
build/                 通用编译宏与 Bazel 模块辅助规则
src/include/           公共机器、设备、存储接口
src/core/              工具、JSON、内存映射与机器配置
src/arch/riscv/        RISC-V CPU、机器与 SoftFP（模板头文件留在模块内）
src/arch/x86/          上游参考源码，不参与构建
src/devices/           VirtIO、PCI、显示、键盘、IDE 等设备
src/net/               从主程序拆出的 TAP／SLIRP 宿主机网络适配
src/storage/           本地文件系统、受限 9P 映射、raw 写入层、HTTP 后端
src/frontend/          命令行主程序、可选 SDL 前端
src/sandbox/           沙盒选项、本地镜像校验及启动策略
src/crypto/            上游浏览器前端使用的 AES／SHA256
src/web/               保留的上游 JavaScript／Emscripten 前端
third_party/slirp/     内置用户态 TCP/IP 网络库，独立 Bazel 目标
tools/                 build_filelist、splitimg、上游网络脚本
examples/              用户态网络与 TAP 配置示例
tests/                 ARP 网关报文与 HTTP/RISC-V 启动测试
docs/upstream/         上游说明、版本、旧 Makefile 和原始发行包
```

本次 Bazel 支持覆盖 Linux 原生程序；浏览器前端作为上游资源保留，尚无 Emscripten Bazel 目标。
归档的 Makefile 记录原始构建，不适用于重排后的目录。

## 运行与网络

快速运行带用户态网络的 RISC-V Linux 示例（配置引用上游远程镜像）：

```sh
bazel build //:k-vm
./bazel-bin/k-vm examples/riscv64-user.cfg
```

也可以直接加载远程配置：

```sh
./bazel-bin/k-vm https://bellard.org/jslinux/buildroot-riscv64.cfg
```

配置文件使用 TinyEMU 的 JSON 风格语法，支持注释、无引号字段名和尾逗号。
本地镜像路径相对于配置文件所在目录；远程配置中的相对路径相对于配置 URL。
使用 `bazel run` 加载本地配置时，建议传入绝对路径，避免运行目录影响查找。
按 `Ctrl-a x` 退出，按 `Ctrl-a h` 查看控制台帮助；默认磁盘使用快照模式，
`-rw` 才会写入原始镜像。PC/x86 配置及加速选项已移除。

### 用户态网络（推荐）

```text
eth0: { driver: "user" }
```

不需要创建网桥、TAP 或 root 权限。内置 DHCP 提供客户机地址，默认网关为
`10.0.2.2`，DNS 为 `10.0.2.3`，默认客户机地址为 `10.0.2.15`。
客户机可执行 `udhcpc -i eth0`，或手动设置该地址、默认路由与 DNS。
当前上游实现只允许一个 SLIRP 实例；用户态网络主要支持客户机主动发起的连接，
命令行尚未暴露入站端口转发选项。

### TAP 网络

```text
eth0: { driver: "tap", ifname: "tap0" }
```

`examples/riscv64-tap.cfg` 展示本地磁盘与 TAP 的组合，需要准备其中的 BIOS、内核、
根文件系统镜像。宿主机示例（将 `$USER` 替换为运行虚拟机的用户）：

```sh
sudo ip tuntap add dev tap0 mode tap user "$USER"
sudo ip addr add 192.168.3.1/24 dev tap0
sudo ip link set tap0 up
./bazel-bin/k-vm examples/riscv64-tap.cfg
```

客户机配置 `192.168.3.2/24`，通过 `192.168.3.1` 可与宿主机通信。
需要出站访问时，在宿主机启用 IP 转发，并按实际出口网卡和防火墙配置 NAT 或网桥；
`tools/netinit.sh` 保留了上游参考脚本，需按环境修改后使用。

### 网络存储

`fs0: { file: "https://server/filesystem" }` 使用 HTTP 9P 后端；
`drive0: { file: "https://server/disk/blk.txt" }` 使用 HTTP 块设备。
可用辅助工具生成服务端数据：

```sh
bazel run //tools:build_filelist -- /absolute/source/root /absolute/output/fs
mkdir -p /absolute/output/disk
bazel run //tools:splitimg -- /absolute/path/disk.img /absolute/output/disk
```

服务端用普通 HTTP 文件服务提供输出目录即可；`build_filelist -h` 或不带参数运行 `splitimg` 可查看用法。

## 验证范围与来源

`bazel test //...` 验证帮助输出、HTTP 配置解析，以及 RV32/RV64/RV128 从本地 HTTP
服务下载 BIOS、初始化 HTTP 文件系统和 SLIRP 网卡并执行指令正常关机。
网络测试向宿主适配层发送真实 ARP 请求，检查 SLIRP 网关的回应。
测试不依赖外部镜像、外网或 root 权限。现代 GCC／OpenSSL 编译时仍会报告部分上游警告（如废弃 API）。TAP 的实际收发需要对应宿主机设备。

上游版权与 MIT 许可保留在源码和 `LICENSE` 中；SLIRP 各文件保留其原有 BSD 等许可。
原始发行包位于 `docs/upstream/dist/tinyemu-2019-12-21.tar.gz`。
