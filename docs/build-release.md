# Linux 构建与发布

## 两条构建路径

默认 `bazel build //:k-vm` 使用宿主系统编译器，需要 curl/OpenSSL 开发库，支持旧版 HTTP 配置、磁盘和远程文件系统。
Hermetic 发布配置仅编译本地存储后端，保留沙盒、9P、TAP/SLIRP、客户机 HTTPS 和 Pi 更新。
它使用 hermetic_cc_toolchain **4.3.0** 的 Zig/Clang/libc，不链接构建机的 curl、OpenSSL 或 glibc。
模块没有全局注册 hermetic 工具链：只有选择相应 `--config` 才会启用，不干扰 macOS/Win32 系统构建。

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
镜像另行准备，不随程序打包。

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

1. main 推送、PR、手动运行：在原生 x86_64 / ARM64 Linux runner 上构建四种产物，运行核心测试、ELF ABI 检查与实际 RISC-V 客户机检查。
2. 每个构建上传独立 tar.gz 与 SHA256，运行检查不需要 Pi 认证或真实 npm 更新。
3. `v*` 标签：四项构建全部成功后下载产物、再次检查 SHA256，创建或更新该标签的 GitHub Release。

没有部署服务器，也没有把镜像或配置文件上传至 Release。PR 仅有只读仓库权限；Release job 单独获得 `contents: write`。
现有 `host-compatibility.yml` 继续检查系统工具链、macOS、Windows 和 SDL。

要启用 GitHub 工作流，需要把本地仓库推送到你创建的 GitHub 仓库：

```sh
git remote add origin https://github.com/YOUR_ACCOUNT/k-vm.git
git push -u origin main
# 确认 CI 成功后发布版本
git tag v0.1.0
git push origin v0.1.0
```

本地创建仓库不会执行远程 CI。本文命令中的账号需替换为你的实际仓库地址。
