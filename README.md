# k-vm

把当前项目放进 RISC-V 虚拟机，在隔离的 Linux 环境里运行 **Pi coding agent** 或 shell。
基于 TinyEMU，使用 Bazel 构建；纯软件模拟，无需 Docker、`/dev/kvm` 或 WSL。

- **即开即用**：当前目录自动挂载到 `/work`。
- **最小配置导入**：Pi 只导入认证、设置和模型文件，不导入历史、会话或整个 HOME。
- **沙盒内部支持 HTTPS**：通过 SLIRP 用户态网络访问 Pi 模型 API、npm registry 和其他 HTTPS 服务，无需 root；glibc 和 musl 静态发布版本均支持。
- **本地镜像**：根磁盘默认使用内存快照，退出后丢弃系统改动；项目文件改动写回宿主。
- **安全更新**：`k-vm update` 更新镜像内的 Pi，成功关机才提交；失败或正常取消保留原盘。
- **跨平台**：Linux、macOS 和 Windows 原生 Win32，客户机始终是 RISC-V。

## 开始使用

先准备或取得带有 k-vm 引导脚本的本地镜像。默认提供 **Debian 13 Slim 基础镜像**，
包含 shell、文件映射、网络、HTTPS 证书和 Pi 初始化脚本；Node.js/Pi 按需安装。
BusyBox 提供引导和救援工具。镜像准备需要 Linux 和 Docker，运行及初始化现有镜像不需要 Docker。
[镜像准备与详细说明](docs/usage.md#沙盒模式)。程序和基础镜像分别下载，均不包含认证文件。
GitHub Actions / Release 提供 `k-vm-base-riscv64.tar.xz` 及 SHA256；下载后：

```sh
sha256sum --check k-vm-base-riscv64.tar.xz.sha256
tar -xJf k-vm-base-riscv64.tar.xz
k-vm sandbox --image ./k-vm-base-riscv64 --agent shell
# 按需安装 Node.js 和 Pi
k-vm init --image ./k-vm-base-riscv64 --agent pi
```

从项目目录运行：

```sh
k-vm sandbox --image /path/to/pi-riscv64 --agent pi
k-vm sandbox --image /path/to/pi-riscv64 --agent shell
k-vm sandbox --image /path/to/pi-riscv64 --agent pi -- --model MODEL
```

首次选择 Pi 时自动下载并初始化 Node.js/Pi，验证成功后保存到本地镜像；以后直接启动。
也可以提前安装，或指定 Pi 版本：

```sh
k-vm init --image /path/to/pi-riscv64 --agent pi
k-vm init --image /path/to/pi-riscv64 --agent pi --version 1.2.0
```

只使用 shell 无需安装 Node.js/Pi。初始化与更新均不挂载项目或认证配置；失败或中断保留原盘。

可用 `K_VM_IMAGE` 设置默认镜像，然后直接运行 `k-vm sandbox --agent pi`。
Pi 配置默认来自 `~/.pi/agent`，可通过 `--config-dir DIR` 指定；只导入
`auth.json`、`settings.json`、`models.json`。Windows 可以直接使用 `C:\…` 路径。

| 操作 | 按键或命令 |
| --- | --- |
| 停止内部命令，保留沙盒 | `Ctrl+C` |
| 从 shell 正常关闭沙盒 | `exit` |
| 退出模拟器 | `Ctrl+A`，然后按 `x` |
| 查看终端快捷键 | `Ctrl+A`，然后按 `h` |
| 初始化 Node.js 和 Pi | `k-vm init --image /path/to/pi-riscv64 --agent pi` |
| 更新镜像内的 Pi | `k-vm update --image /path/to/pi-riscv64` |
| 安装指定 Pi 版本 | `k-vm update --image /path/to/pi-riscv64 --version 1.2.0` |

Pi 结束后会进入 shell。执行更新前先退出使用该镜像的沙盒；更新不挂载项目或认证配置。
`update` 更新 Pi 及其 npm 依赖；内核、Node.js 和系统包通过重新准备镜像升级。

### 镜像体积与运行时

本机新构建的基础镜像 `images/base-riscv64` 实际占用约 **304 MiB**（含内核和固件），
根磁盘逻辑容量默认 4 GiB。安装 Node.js/Pi 后体积会增加；逻辑容量代表预留空间，
不等于实际占用或压缩包下载大小。`--size-gb 3` 可以减少预留空间。
本机重新构建的完整 Pi 镜像约 **665 MiB**；初始化后的磁盘占用还会受文件系统写入影响。

Linux/OpenSBI 版本与源码校验固定在 [versions.json](tools/agent_image/versions.json)。
本地和 CI 缓存已编译的启动组件，命中后直接复用；更新版本文件、构建脚本或编译器时重新构建。

准备工具默认构建基础镜像；需要预装完整 Pi 环境时添加 `--with-pi`：

```sh
bazel run //tools:prepare_agent_image
bazel run //tools:prepare_agent_image -- --with-pi --output images/pi-full-riscv64
```

安装期间下载文件和 npm 缓存放在客户机内存中的临时目录，结束后清理。
初始化时优先使用宿主 curl 下载公开的 Node.js 归档，再交给客户机校验；宿主没有 curl 时
使用客户机 HTTPS 下载。curl 是可选的下载加速工具，不是程序的动态链接依赖。
可通过 `init --packages DIR --network none --version X.Y.Z` 使用完整的离线安装包，
目录格式见[使用手册](docs/usage.md#初始化-nodejs-和-pi)。

Pi 的正式运行时是 Node.js（要求 Node.js 22.19 或更高版本）。Pi 的 npm 依赖和 Node API
尚未针对 Deno 做完整验证，Bun 也没有可依赖的官方 Linux RISC-V 运行时包，因此 Deno/Bun
目前不能作为这个 RISC-V Pi 镜像的直接替代品。Pi 源码构建脚本使用 Bun 不代表生产 CLI
可以换用 Bun；如需尝试，应使用单独的实验镜像。

## Linux 发行版与构建

GitHub Actions 在一个 Linux job 中交叉构建以下四种版本，统一上传 `k-vm-linux` 产物；`v*` 标签触发 Release 和 SHA256 发布。
仓库创建到 GitHub 并推送后，工作流才会实际运行。
基础镜像独立构建一次，上传为 `k-vm-base-riscv64` 产物，并随标签发布；
它包含内核、固件、基础系统和初始化脚本，未预装 Node.js/Pi，可供 Linux、macOS 和原生 Windows 使用。
本机基础镜像压缩包约 **53.2 MiB**，后续构建大小随上游系统和内核版本变化。

| 产物 | 宿主要求 | 适用场景 |
| --- | --- | --- |
| `linux-amd64-glibc-2.28` | x86_64、glibc ≥ 2.28 | glibc ABI 满足 Debian 10+（内核另需 5.6+） |
| `linux-arm64-glibc-2.28` | aarch64、glibc ≥ 2.28 | ARM64 Linux |
| `linux-amd64-musl-static` | x86_64 Linux，无动态 libc 依赖 | Alpine、精简系统、单文件分发 |
| `linux-arm64-musl-static` | aarch64 Linux，无动态 libc 依赖 | ARM64 精简系统 |

这些是**宿主机程序架构**，不会改变 RISC-V 客户机。静态版本仍依赖 Linux 内核接口。
映射后端使用 `openat2`，需要 Linux 5.6+ 且该系统调用可用。

安装 Bazelisk 与 Python 3；项目通过 `.bazelversion` 固定使用 Bazel **9.3.0**。然后选择构建配置：

```sh
bazel build --config=linux-amd64-musl //:k-vm
# 或固定 glibc 2.28 ABI（不是链接到构建机的 glibc）
bazel build --config=linux-amd64-glibc //:k-vm
# ARM64 可在 x86_64 主机交叉编译
bazel build --config=linux-arm64-musl //:k-vm
```

上述配置使用 `bazel_dep(name = "hermetic_cc_toolchain", version = "4.3.0")`。
工具链由 Bazel 下载，首次构建需联网；版本和下载校验记录在模块锁文件中。
要改变 glibc 基线，在 `.bazelrc` 的 `gnu.2.28` 工具链标签中指定工具链支持的版本，
同时调整 CI 的 ABI 检查和产物名称。[构建与发布说明](docs/build-release.md)。

发布配置支持本地镜像、文件映射、SLIRP 和 Pi 更新；关闭旧版**宿主 HTTP 镜像/文件系统加载**，
因此无需宿主机安装 curl/OpenSSL 动态库。**沙盒内部的 HTTPS 完整保留**：Pi 的模型 API 请求、
npm 包下载和 `k-vm update` 均可通过 HTTPS 联网。TLS 和证书验证由客户机内的 Node.js、curl 等工具处理；
现代 Pi 镜像包含 CA 证书，启动时同步宿主机时间。自定义镜像需自行提供 HTTPS 客户端和 CA 证书。

需要旧版远程资源或 SDL 时，使用系统编译器：

```sh
sudo apt-get install build-essential python3 libcurl4-openssl-dev libssl-dev
bazel build //:k-vm
bazel test //...
```

## macOS 与 Windows

macOS 使用 Xcode Command Line Tools、Bazelisk、curl/OpenSSL；支持 Apple Silicon 和 Intel。
Windows 10/11 x64 使用 `hermetic_cc_toolchain 4.3.0` 的 Zig/Clang 构建原生 EXE，不需要 WSL：

```powershell
# 已安装 Bazelisk，无需安装 MSYS2/GCC
bazel test --config=win32 //...
.\bazel-bin\k-vm.exe sandbox --image C:\vm-images\pi-riscv64 --agent pi
```

Linux/macOS 可用 `bazel build --config=win32 //:k-vm` 交叉构建同一 EXE。
工具链自带 MinGW 头文件和运行库；MSYS2/GCC 的独立脚本保留为可选构建方式。
[各平台依赖、映射语义与限制](docs/usage.md#宿主机兼容)。

## 项目与边界

```text
src/                  CPU、设备、网络、存储和各平台前端
src/sandbox/          启动、工作目录与配置策略、更新流程
third_party/slirp/    内置用户态网络
build/                Bazel 配置和工具链配置
guest/                客户机引导脚本
tools/                镜像准备、构建和发行打包
tests/                单元测试与真实客户机检查
.github/workflows/    Linux 发布及宿主兼容性 CI
docs/                 详细手册、构建说明与上游资料
```

这是虚拟机文件共享边界：整个当前项目（含隐藏文件）可见且可写。
不要把整个 HOME 或文件系统根目录作为项目目录。Linux/macOS 拒绝符号链接穿越；
Windows 拒绝 reparse point、ADS 和设备名。硬链接及同用户恶意宿主进程的文件替换不属于隔离保证。
默认 SLIRP 允许客户机向外连接，包括访问宿主的 `10.0.2.2`；需要断网时使用 `--network none`。
软件模拟速度低于硬件虚拟化，运行和更新 Pi 可能需要更长时间。

详细参数、额外文件映射、TAP、镜像格式与验证范围请见 [使用手册](docs/usage.md)。
TinyEMU 原始版权及 MIT 许可保留于 [LICENSE](LICENSE)；SLIRP 保留各源文件的许可证声明。
