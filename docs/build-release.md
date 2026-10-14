# Linux 构建与发布

## 两条构建路径

默认 `bazel build //:k-vm` 使用宿主系统编译器，需要 curl/OpenSSL 开发库，支持旧版 HTTP 配置、磁盘和远程文件系统。
Hermetic 发布配置仅编译本地存储后端，保留沙盒、9P、TAP/SLIRP、客户机 HTTPS 和 Pi 更新。
它使用 hermetic_cc_toolchain **4.3.0** 的 Zig/Clang/libc，不链接构建机的 curl、OpenSSL 或 glibc。
模块没有全局注册 hermetic 工具链：只有选择相应 `--config` 才会启用，默认 macOS/Linux 系统构建不受影响；`--config=win32` 也使用 hermetic 工具链。

**四种发布版本都支持沙盒内部 HTTPS**，包括 Pi 模型 API、npm registry 下载及 `k-vm update`。
SLIRP 转发客户机网络流量，TLS 和证书验证由客户机内的 Node.js、curl 等客户端完成，
不需要宿主机的 curl/OpenSSL 动态库。现代 Pi 镜像包含 CA 证书，启动时同步宿主机时间；
自定义镜像需要自行安装 HTTPS 客户端和 CA 证书。发布配置关闭的功能是旧版宿主 HTTP 镜像和远程文件系统加载。

| Bazel 配置 | 目标架构 | libc |
| --- | --- | --- |
| `linux-amd64-glibc` | x86_64 | glibc 2.28 ABI |
| `linux-arm64-glibc` | aarch64 | glibc 2.28 ABI |
| `linux-amd64-musl` | x86_64 | 静态 musl |
| `linux-arm64-musl` | aarch64 | 静态 musl |

```sh
bazel build --config=linux-amd64-glibc //:k-vm
python3 tools/package_linux.py --arch amd64 --libc glibc --glibc 2.28
```

打包脚本检查 ELF 架构、动态依赖和最高 GLIBC 符号版本。
musl 产物必须没有 `PT_INTERP` 和 `DT_NEEDED`；glibc 产物只能引用标准 glibc 库且最高版本不超过 2.28。
产物位于 `dist/`，包含程序、README、许可证和 SLIRP 声明，旁边附带 `.sha256`。
基础镜像作为单独的 `k-vm-base-riscv64.tar.xz` 发布，不嵌入程序包。

## 指定不同 glibc 基线

在 `.bazelrc` 复制一个现有配置，替换工具链标签中的 `gnu.2.28`，例如：

```text
build:linux-amd64-glibc231 --config=portable
build:linux-amd64-glibc231 --platforms=@zig_sdk//platform:linux_amd64
build:linux-amd64-glibc231 --extra_toolchains=@zig_sdk//toolchain:linux_amd64_gnu.2.31
```

然后使用 `--glibc 2.31` 打包。选择更旧基线不意味着支持旧 Linux 内核：当前 Linux 文件映射需要 `openat2`（Linux 5.6+）。
Bazel 9.3.0 固定在 `.bazelversion`；工具链模块及其校验由 `MODULE.bazel.lock` 固定。

## 自动化

`.github/workflows/linux-release.yml`：

1. main 推送、PR、手动运行：在一个 x86_64 Linux runner 上交叉构建四种产物，共用工具链缓存；核心测试与实际 RISC-V 客户机检查仅在 amd64/musl 上运行一次，每种产物分别检查 ELF ABI。ARM64 产物不在此工作流中执行原生测试。
2. 统一上传 `k-vm-linux` 工作流产物，内含四组独立 tar.gz 与 SHA256，运行检查覆盖 Pi 初始化、自动启动和更新事务，不需要 Pi 认证或真实 npm 安装。
3. 独立镜像 job 根据 `tools/agent_image/versions.json` 固定内核与 OpenSBI，使用 Actions Cache 保存和复用启动组件；未命中或校验失败时才交叉编译，再使用 Docker/QEMU 准备 Debian RV64 基础系统；验证客户机文件映射和命令中断后，上传 `k-vm-base-riscv64` 产物（tar.xz 和 SHA256）。不预装 Node.js/Pi，不包含宿主认证文件。
4. main 推送（或在 main 上手动运行）时，程序打包、基础镜像、Windows、ARM64 macOS、Linux 和 SDL 检查全部通过后，读取 `MODULE.bazel` 中 `module(version = "...")`。例如 `0.1.0` 对应 `v0.1.0`：远程已有该标签则跳过；没有则将本次提交的程序和基础镜像产物上传到草稿 Release，再公开发布并由 GitHub 创建标签。后续发布只需修改模块版本并推送 main。
5. 仍支持手动推送 `v*` 标签：同样等待全部检查完成、验证 SHA256 后创建或更新该标签的 Release。PR 不创建标签或发布版本。发布 job 串行执行，使用 `GITHUB_TOKEN` 的 `contents: write` 权限，自动创建的标签不会再次触发重复构建。上传中断时草稿保留，标签尚未创建时可通过重新运行工作流继续发布。

基础镜像包包含启动配置、构建来源、内核配置、内核、固件和根磁盘。打包使用 GNU sparse tar + xz，保留 4 GiB 逻辑磁盘的稀疏区域；本机压缩后约 53.2 MiB，实际大小随构建版本变化。
本地准备并打包：

```sh
sudo python3 tools/prepare_agent_image.py --output images/base-riscv64
python3 tools/package_image.py --image images/base-riscv64
cd dist
sha256sum --check k-vm-base-riscv64.tar.xz.sha256
```

启动组件缓存按 Linux/OpenSBI 各自的版本、源码 SHA256、构建脚本和编译器身份区分；修改版本清单后，旧缓存中的未变组件仍可复用。缓存只保存内核、内核配置、固件及校验清单，不上传源码树、磁盘或用户配置。GitHub 缓存可能被淘汰，此时自动重新编译。

解压后使用 `--image ./k-vm-base-riscv64` 进入 shell 或初始化 Pi，参见 README。
没有部署服务器。PR 仅有只读仓库权限；Release job 单独获得 `contents: write`。
`host-compatibility.yml` 作为可复用工作流由 Linux 发布工作流调用，检查系统工具链、ARM64 macOS、Windows 和 SDL。Windows 使用 `bazel test --config=win32 //...`，由同一 hermetic 模块提供 Zig/Clang 与 MinGW 运行库，不再安装 MSYS2/GCC 或重复编译。Linux/macOS 可用该配置交叉编译 Windows EXE；原生 Windows runner 负责执行测试。

要启用 GitHub 工作流，需要把本地仓库推送到你创建的 GitHub 仓库：

```sh
git remote add origin https://github.com/YOUR_ACCOUNT/k-vm.git
git push -u origin main
# CI 全部通过后自动发布 MODULE.bazel 中尚未打标签的版本
# 下一版：修改 module(version = "0.1.1")，提交并推送 main
```

本地创建仓库不会执行远程 CI。本文命令中的账号需替换为你的实际仓库地址。

CI 构建、测试和客户机检查失败时，会在 GitHub Actions 的 Summary 中列出失败命令、退出码、命令输出末尾和失败测试日志（可展开）。完整命令输出、Bazel 构建事件及失败测试日志会上传为 `failure-logs-*` 产物；Summary 中超长日志会截断，完整日志以产物为准。测试日志根据 Bazel 构建事件定位，不依赖 Windows 的 `bazel-testlogs` 链接。
