# ReliefNT

**ReliefNT** 是 ReliefOS 项目的独立内核仓库：Ring-0 内核（`kernel/reliefnt`）、
内核调试器模块（`debug/`，ET_REL 的 kerneldebug.sys）、启动加载器（`boot/loader`）、UAPI 头
（`include/uapi`、`include/reliefos`）以及引导期驱动（`drivers/`，console/TTY
显示后端在 `drivers/console`）。构建产物恰为
内核侧四个制品：`kernel.sys`、`kernel.debug`、`loader.elf` 与
`kerneldebug.sys`；设备驱动（mouse/serial/e1000/ac97/es1371/hda）直接链接进
`kernel.sys`。本仓库不含用户态、镜像或打包目标。

在父仓库 [ReliefOS](https://github.com/ReliefOSProject/ReliefOS) 中，本仓库挂载于
`kernel/reliefnt/`。

许可证：Apache License 2.0（见 [LICENSE](LICENSE)），第三方组件的归属与许可见
[NOTICE](NOTICE)。

## 获取

```sh
git clone --recurse-submodules https://github.com/ReliefOSProject/ReliefNT.git
cd ReliefNT
# 若克隆时未带 --recurse-submodules：
git submodule update --init --recursive
```

`third_party/kconfig-frontends` 是真实子模块（Kconfig 前端，仅构建期使用）；
缺失时构建会拒绝继续。`third_party/zlib/contrib/puff/puff.c` 为随库跟踪的源文件。

## 依赖

构建环境为 Linux/WSL，入口为 GNU Make 4.3+（生产链只编译 C 与汇编；Python 仅
`make test` 使用）。典型依赖：

```sh
sudo apt install build-essential clang lld llvm libclang-rt-dev \
  autoconf automake libtool libtool-bin pkg-config bison flex gperf gettext \
  libncurses-dev gzip curl python3 git
```

工具链由 `configs/toolchains/llvm-x86_64.mk` 描述（默认 `TOOLCHAIN=`）。

## 构建

先取回锁定的构建输入（下载缓存 `cache/` 不入库；构建本身从不联网，只校验缓存）：

```sh
make O=$PWD/out/x86_64/release fetch
```

`configs/dependencies.lock.json` 以 sha256 钉住每个输入（当前为 GNU Unifont
16.0.04），`tools/build/fetch.sh` 负责下载与校验。离线构建：预先按同一文件名把
输入放入 `cache/downloads/` 即可（校验不通过会拒绝使用）。

然后构建：

```sh
make O=$PWD/out/x86_64/release ARCH=x86_64 PROFILE=release all
```

`O=` 为输出目录（需绝对路径；默认 `out/<arch>/<profile>`），`ARCH=x86_64`，
`PROFILE=release|debug`。其余目标：

基础内核版本在 `configs/build-version` 中，当前为 `5.0.0`。可以像 Linux 一样
修改 Makefile 的 `EXTRAVERSION`，或使用 `make EXTRAVERSION=-perf all` 构建
`5.0.0-perf`；`LOCALVERSION=-test` 会继续追加为 `5.0.0-perf-test`。
默认后缀为空。后缀变化自动触发版本头和内核重建；系统信息、uname 和 procfs
都报告完整内核版本。完整版本最多 31 字节，后缀接受字母、数字、`._+-`。

| 目标 | 作用 |
| --- | --- |
| `make O=... all` | 四个内核制品（kernel.sys、kernel.debug、loader.elf、kerneldebug.sys） |
| `make O=... headers_install` | 按白名单导出 UAPI 头到 `$(O)/kernel-export/include` |
| `make O=... install` | 制品与 `manifest.txt` 复制到 `$(DESTDIR)`（默认 `$(O)/kernel-install`） |
| `make O=... test` | 宿主工具单测 + `tools/test_abi_layout.py` + `tools/test_header_export.py` |
| `make O=... clean` | 清理输出目录（`distclean` 连同配置一起清理；`cache/` 永远保留） |
| `make help` | 全部目标与变量 |

## 许可证

本仓库自有源码以 Apache License 2.0 发布（[LICENSE](LICENSE)）；GNU Unifont、
zlib puff、kconfig-frontends 等第三方组件维持各自许可，详见
[NOTICE](NOTICE) 与 [resources/licenses/](resources/licenses/)。
