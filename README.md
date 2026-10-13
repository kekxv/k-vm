# k-vm

把当前项目放进 RISC-V 虚拟机，在隔离的 Linux 环境里运行 **Pi coding agent** 或 shell。
基于 TinyEMU，使用 Bazel 构建；纯软件模拟，无需 Docker、`/dev/kvm` 或 WSL。

- **即开即用**：当前目录自动挂载到 `/work`。
- **最小配置导入**：Pi 只导入认证、设置和模型文件，不导入历史、会话或整个 HOME。
- **默认联网**：SLIRP 用户态网络，无需 root；客户机可使用 HTTPS/npm。
- **本地镜像**：根磁盘默认使用内存快照，退出后丢弃系统改动；项目文件改动写回宿主。
- **安全更新**：`k-vm update` 更新镜像内的 Pi，成功关机才提交；失败或正常取消保留原盘。
- **跨平台**：Linux、macOS 和 Windows 原生 Win32，客户机始终是 RISC-V。

## 开始使用

先准备或取得带有 k-vm 引导脚本的本地镜像。Pi 镜像使用 **Debian 13 Slim + Node.js + Pi**；
BusyBox 提供引导和救援工具。镜像准备需要 Linux 和 Docker，运行现有镜像不需要 Docker。
[镜像准备与详细说明](docs/usage.md#沙盒模式)。发行包只包含程序，不包含镜像或认证文件。

从项目目录运行：

```sh
k-vm sandbox --image /path/to/pi-riscv64 --agent pi
k-vm sandbox --image /path/to/pi-riscv64 --agent shell
k-vm sandbox --image /path/to/pi-riscv64 --agent pi -- --model MODEL
```

可用 `K_VM_IMAGE` 设置默认镜像，然后直接运行 `k-vm sandbox --agent pi`。
Pi 配置默认来自 `~/.pi/agent`，可通过 `--config-dir DIR` 指定；只导入
`auth.json`、`settings.json`、`models.json`。Windows 可以直接使用 `C:\…` 路径。

| 操作 | 按键或命令 |
| --- | --- |
| 停止内部命令，保留沙盒 | `Ctrl+C` |
| 从 shell 正常关闭沙盒 | `exit` |
| 退出模拟器 | `Ctrl+A`，然后按 `x` |
| 查看终端快捷键 | `Ctrl+A`，然后按 `h` |
| 更新镜像内的 Pi | `k-vm update --image /path/to/pi-riscv64` |
| 安装指定 Pi 版本 | `k-vm update --image /path/to/pi-riscv64 --version 1.2.0` |

Pi 结束后会进入 shell。执行更新前先退出使用该镜像的沙盒；更新不挂载项目或认证配置。
`update` 更新 Pi 及其 npm 依赖；内核、Node.js 和系统包通过重新准备镜像升级。

## Linux 发行版与构建

GitHub Actions 自动构建以下版本，上传工作流产物；`v*` 标签触发 Release 和 SHA256 发布。
仓库创建到 GitHub 并推送后，工作流才会实际运行。

| 产物 | 宿主要求 | 适用场景 |
| --- | --- | --- |
| `linux-amd64-glibc-2.28` | x86_64、glibc ≥ 2.28 | glibc ABI 满足 Debian 10+（内核另需 5.6+） |
| `linux-arm64-glibc-2.28` | aarch64、glibc ≥ 2.28 | ARM64 Linux |
| `linux-amd64-musl-static` | x86_64 Linux，无动态 libc 依赖 | Alpine、精简系统、单文件分发 |
| `linux-arm64-musl-static` | aarch64 Linux，无动态 libc 依赖 | ARM64 精简系统 |

这些是**宿主机程序架构**，不会改变 RISC-V 客户机。静态版本仍依赖 Linux 内核接口。
映射后端使用 `openat2`，需要 Linux 5.6+ 且该系统调用可用。

安装 Bazelisk 与 Python 3，然后选择构建配置：

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
因此无需 curl/OpenSSL 动态库。客户机 HTTPS 和联网保持可用。

需要旧版远程资源或 SDL 时，使用系统编译器：

```sh
sudo apt-get install build-essential python3 libcurl4-openssl-dev libssl-dev
bazel build //:k-vm
bazel test //...
```

## macOS 与 Windows

macOS 使用 Xcode Command Line Tools、Bazelisk、curl/OpenSSL；支持 Apple Silicon 和 Intel。
Windows 10/11 x64 使用 MinGW-w64 构建原生 EXE，不需要 WSL：

```powershell
# 已安装 MSYS2 MINGW64 和 Bazelisk
$env:BAZEL_SH = 'C:/msys64/usr/bin/bash.exe'
bazel test --config=win32 //...
.\bazel-bin\k-vm.exe sandbox --image C:\vm-images\pi-riscv64 --agent pi
```

也可在 MSYS2 MINGW64 中运行 `python tools/build_win32.py --test`。
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
