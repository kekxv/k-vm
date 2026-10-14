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
Bazel 7.4.1 固定在 `.bazelversion`；工具链模块及其校验由 `MODULE.bazel.lock` 固定。

## 自动化

`.github/workflows/linux-release.yml`：

1. main 推送、PR、手动运行：在一个 x86_64 Linux runner 上交叉构建四种产物，共用工具链缓存；核心测试与实际 RISC-V 客户机检查仅在 amd64/musl 上运行一次，每种产物分别检查 ELF ABI。ARM64 产物不在此工作流中执行原生测试。
2. 统一上传 `k-vm-linux` 工作流产物，内含四组独立 tar.gz 与 SHA256，运行检查覆盖 Pi 初始化、自动启动和更新事务，不需要 Pi 认证或真实 npm 安装。
3. 独立镜像 job 交叉编译内核与 OpenSBI，使用 Docker/QEMU 准备 Debian RV64 基础系统；验证客户机文件映射和命令中断后，上传 `k-vm-base-riscv64` 产物（tar.xz 和 SHA256）。不预装 Node.js/Pi，不包含宿主认证文件。
4. `v*` 标签：程序打包 job 及基础镜像 job 全部成功后下载所有产物、再次检查 SHA256，创建或更新该标签的 GitHub Release。

基础镜像包包含启动配置、构建来源、内核配置、内核、固件和根磁盘。打包使用 GNU sparse tar + xz，保留 4 GiB 逻辑磁盘的稀疏区域；本机压缩后约 53.2 MiB，实际大小随构建版本变化。
本地准备并打包：

```sh
sudo python3 tools/prepare_agent_image.py --output images/base-riscv64
python3 tools/package_image.py --image images/base-riscv64
cd dist
sha256sum --check k-vm-base-riscv64.tar.xz.sha256
```

解压后使用 `--image ./k-vm-base-riscv64` 进入 shell 或初始化 Pi，参见 README。
没有部署服务器。PR 仅有只读仓库权限；Release job 单独获得 `contents: write`。
现有 `host-compatibility.yml` 继续检查系统工具链、macOS、Windows 和 SDL。Windows 使用 `bazel test --config=win32 //...`，由同一 hermetic 模块提供 Zig/Clang 与 MinGW 运行库，不再安装 MSYS2/GCC 或重复编译。Linux/macOS 可用该配置交叉编译 Windows EXE；原生 Windows runner 负责执行测试。

要启用 GitHub 工作流，需要把本地仓库推送到你创建的 GitHub 仓库：

```sh
git remote add origin https://github.com/YOUR_ACCOUNT/k-vm.git
git push -u origin main
# 确认 CI 成功后发布版本
git tag v0.1.0
git push origin v0.1.0
```

本地创建仓库不会执行远程 CI。本文命令中的账号需替换为你的实际仓库地址。
