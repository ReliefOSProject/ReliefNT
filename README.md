# ReliefNT

**ReliefNT** 是 ReliefOS 项目的独立内核仓库：Ring-0 内核（`kernel/reliefnt`）、
内核调试器模块（`debug/`，ET_REL 的 `kerneldebug.sys`）、UAPI 头和引导期驱动。
`kernel.sys` 自带 Multiboot2 头，由 GRUB 直接引导。独立构建产物为
`kernel.sys`、`kernel.debug` 和 `kerneldebug.sys`；驱动直接链接进内核。
本仓不包含用户态、系统镜像或打包目标。

在父仓库 [ReliefOS](https://github.com/ReliefOSProject/ReliefOS) 中，本仓挂载于
`kernel/reliefnt/`。构建迁移的源列表、旧参数与产物基线见
[迁移审计记录](docs/KBUILD_MIGRATION_AUDIT.md)。

许可证：Apache License 2.0（见 [LICENSE](LICENSE)）；第三方组件归属与许可见
[NOTICE](NOTICE)。Linux 6.12 的原生 Kbuild/Kconfig 源文件按
[`scripts/kbuild-source.json`](scripts/kbuild-source.json) 的清单和校验值导入。

## 获取

```sh
git clone https://github.com/ReliefOSProject/ReliefNT.git
cd ReliefNT
```

## 依赖

构建环境为 Linux/WSL，入口为 GNU Make 4.3+。目标 C/汇编由 Clang/LLVM 生成；
原生 Kconfig 的 `menuconfig` 需要 ncurses，测试使用 Python 3。典型依赖：

```sh
sudo apt install build-essential clang lld llvm libclang-rt-dev \
  bison flex pkg-config libncurses-dev gzip curl python3 git
```

工具链由 `configs/toolchains/llvm-x86_64.mk` 描述。GNU Make 以 `-rR` 运行原生
Linux 6.12 `Makefile.build`、`Makefile.host` 和 Kconfig；构建参数适配位于
`scripts/Makefile.reliefnt`，导入源与许可证清单位于 `scripts/kbuild-source.json`。

## 构建

先取回锁定的构建输入（下载缓存 `cache/downloads/` 不入库；编译前只校验缓存）：

```sh
make O=$PWD/out/x86_64/release fetch
```

`configs/dependencies.lock.json` 以 SHA-256 钉住 GNU Unifont 16.0.04。离线构建时，
把锁定输入放入 `cache/downloads/` 即可；校验不通过或输入缺失时构建会停止。

然后构建并安装：

```sh
make O=$PWD/out/x86_64/release ARCH=x86_64 PROFILE=release all
make O=$PWD/out/x86_64/release install
```

`O=` 是独立输出目录（默认 `out/<arch>/<profile>`，相对路径会规范化为绝对路径），
`ARCH=x86_64`，`PROFILE=release|debug`。release 使用 `-O3`，debug 使用 `-O0 -g`。
基础版本来自 `configs/build-version`；`EXTRAVERSION=-perf` 与
`LOCALVERSION=-test` 会生成 `5.1.1-perf-test`。`TOOLCHAIN=` 可选其他工具链描述，
`CC=`, `LD=`, `AR=`, `OBJCOPY=`, `STRIP=`, `NM=`, `HOSTCC=` 可在命令行覆盖相应工具。

内核使用原生 `conf` / `mconf`、`syncconfig`、`.config`、`include/config/auto.conf`
和 `include/generated/autoconf.h`。当前内核没有可配置的构建功能项；镜像、产品和
用户态选项属于父仓库，不会生成未被内核消费的伪配置。`PROFILE`、版本后缀与工具链
仍由独立内核 Makefile 提供。

| 目标 | 作用 |
| --- | --- |
| `make O=... all` | 构建 `kernel.sys`、`kernel.debug`、`kerneldebug.sys` |
| `make O=... vdso` | 构建 vDSO ELF 和嵌入对象 |
| `make O=... defconfig` / `olddefconfig` / `menuconfig` | 使用原生 Linux Kconfig 工具 |
| `make O=... headers_install` | 按白名单导出 UAPI 头到 `$(O)/kernel-export/include` |
| `make O=... install DESTDIR=...` | 安装三个产品和带校验和的 `manifest.txt` |
| `make O=... test-vdso` | 检查 vDSO ELF、嵌入字节及运行时 ABI |
| `make O=... test` | 宿主工具测试、ABI 布局、头文件导出和 vDSO 检查 |
| `make O=... clean` | 清理构建输出并保留 `.config` 与下载缓存 |
| `make O=... distclean` | 清理构建输出和 `.config`；下载缓存保留 |
| `make help` | 显示目标和变量 |

## 内核日志

内核诊断统一为 `[启动后秒数] [等级] [子系统] 消息`，等级为 `DEBUG`、`INFO`、
`WARN`、`ERROR` 和 `FATAL`。`DEBUG` 用于寄存器、映射和事件跟踪，`INFO` 用于
正常状态，`WARN` 用于降级或恢复，`ERROR` 用于操作失败，`FATAL` 用于无法继续
启动或内核异常。全部等级默认保留，早期时钟尚未初始化时启动时间为零。

内核代码使用 `console_debug/info/warn/error/fatal`，或显式传入等级的
`console_log`；旧 `console_printf` 兼容入口按 `INFO` 输出。内核和驱动共享
`<reliefos/kernel_log.h>` 中的等级枚举；所有内置驱动通过 ABI 的 `console_log`
回调显式传递等级和格式参数，不再自行拼接等级、时间戳或颜色。该回调为 ABI v1
末尾的大小门控扩展，使用前检查 `RELIEFOS_DRIVER_LOG_API_SIZE`。旧版 ABI 的
`console_write` 仅保留兼容：已知等级标记由内核转换，未标记消息按 `INFO` 输出。
同一消息的多行均带等级，格式化输出支持整数宽度、补零、`l`、`ll` 和 `z`。

等级标签默认使用低饱和度 ANSI 真彩色，不加粗、不闪烁；时间戳和正文不染色。
在 GRUB 的 `multiboot2` 行追加 `logcolor=0` 可从第一条启动日志起关闭内核日志
ANSI 配色，`logcolor=1` 显式开启（默认）。参数按完整 token 匹配，可用空格或
制表符分隔；重复指定时最后一个有效值生效。串口和 framebuffer 使用相同格式，
VGA 文本回退过滤 ANSI 控制序列，不显示转义乱码。该选项不改变用户态程序或
交互式内核调试器自己的 ANSI 输出。

普通日志按 CPU 归属和中断状态串行输出；致命日志不等待其他 CPU 持有的日志锁。
启动回放使用 64 KiB 环形缓冲区，满时保留最新完整日志行。用户态终端、PTY、
`/dev/console` 和串口写入继续使用原始输出接口，不添加内核日志等级；内核
调试器的交互屏幕也保持原有显示。

启动日志输出到最后的 `boot complete` 后才交给 tty1；接管时保留已有文字和
光标位置，PID 1/OpenRC 输出直接接在后面，不再清屏。启动日志也纳入 tty1
文本历史，切换到其他 VT 或离开图形模式后可以恢复。重复激活当前文本 VT
不会重置画面；真正切换 VT、图形程序接管，以及程序主动清屏仍正常生效。
PID 1 的标准输入、输出和错误共享一个以读写模式打开的 `/dev/console`
描述；它在打开时绑定当前 VT，提供终端读写、termios、窗口尺寸和 VT ioctl，
不会自动成为控制终端。OpenRC 通过正常终端检测选择 ANSI 输出。系统控制台
写入额外镜像到诊断串口，不作为内核日志记录；`/dev/kmsg` 独立写入内核日志
缓冲区和串口，不注入任意 tty。终端绘制仍只发生在对应 VT 的可见文本模式。

启动日志记录构建身份、固件及模块、CPU 型号和特征、物理内存及页缓存容量、
PCI 清单、显示后端与 3D 探测结果、根文件系统选择、驱动加载摘要、虚拟终端、
PID 1 以及在线 CPU 数量。设备不可用、回退和初始化失败保留具体状态或错误码。

## 许可证

本仓库自有源码以 Apache License 2.0 发布（[LICENSE](LICENSE)）；GNU Unifont、
zlib puff 与导入的 Linux 6.12 Kbuild/Kconfig 文件维持各自许可，详见
[NOTICE](NOTICE)、[resources/licenses/](resources/licenses/) 和
[`LICENSES/kbuild/COPYING`](LICENSES/kbuild/COPYING)。
