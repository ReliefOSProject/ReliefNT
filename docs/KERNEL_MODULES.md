# 内核模块与构建归属

ReliefNT 按职责组织静态链接的内核部件。目录模块只划分源码和私有接口，
不改变启动顺序、调度策略、锁、磁盘格式或 Linux syscall/UAPI，也不引入
可加载内核模块。所有生产 C 和汇编文件独立编译。

## 目录边界

| 路径 | 职责 |
| --- | --- |
| `arch/x86_64/boot/` | Multiboot2、入口与早期引导。 |
| `arch/x86_64/cpu/` | CPU 特性、GDT、SMP、任务切换汇编。 |
| `arch/x86_64/interrupt/` | IDT、IRQ、APIC 与 PCI 中断。 |
| `arch/x86_64/mm/`、`platform/` | x86 页表、PCI、平台电源与平台接口。 |
| `kernel/init/`、`exec/` | 内核初始化、版本和诊断；ELF 与用户地址空间装载。 |
| `kernel/sched/` | 任务表、EEVDF 公平调度、clone/exec、等待、会话、计时与统计。 |
| `kernel/syscall/` | syscall 入口/分发；`fd/`、`io/`、`fs/`、`event/`、`device/`、`process/`、`ipc/`、`system/`、`memory/`、`time/` 各自实现所属接口。 |
| `kernel/{device,signal,time,ipc,sync,random}/` | 驱动注册和资源、信号、时钟/vDSO 数据、IPC、同步、随机源。 |
| `mm/{heap,physical,cache,user}/` | 堆、物理页、页缓存、用户内存访问。 |
| `block/{core,partition,device,install}/` | 块 I/O 与共享存储状态、分区/标识、设备链接、安装操作。 |
| `fs/vfs/`、`security/` | 路径/挂载、文件和目录操作、设备节点、权限与旁路元数据。 |
| `fs/{ext2,ext4,exfat,fat,iso9660,tmpfs,proc,sysfs}/` | 各文件系统的独立实现；ext4 内部继续区分分配、缓存、目录、extent、journal 和挂载等职责。 |
| `net/{interface,link,ipv4,tcp,services,socket}/` | 接口与配置、ARP/以太网、IP/UDP/ICMP、TCP 状态和收发、DHCP/DNS/NTP/HTTP、socket 生命周期和操作。 |
| `drivers/console/` | 文本控制台核心、绘制、ANSI、日志、VT 和 panic 输出。 |
| `drivers/tty/{input,pty,serial}/` | 输入路由、PTY/行规程/终端会话、串口。 |
| `drivers/video/framebuffer/` | 像素/字形、绘图、呈现、VBE/VMware、panic 与旧桌面绘制。 |
| `drivers/video/drm/vmwgfx/` | SVGA 资源/能力及 execbuf 验证、翻译、fence、retire、continuation。 |
| `drivers/sound/hda/` | 控制器 I/O、rings、恢复、verbs；codec 探测、路由和控制；stream 实现。 |
| `drivers/{storage,net,input,usb}/` | 按设备系列组织的硬件驱动。 |
| `sound/core/` | 音频设备、PCM 状态/缓冲/格式/映射/传输、mixer、ALSA/OSS 接口。 |
| `lib/`、`firmware/` | 内核内部基础工具、panic/bugcheck、文本与固件支持。 |
| `vdso/`、`debug/` | 独立 vDSO ELF 及嵌入对象；独立 kerneldebug 载荷。 |

## 私有接口与共享状态

`include/uapi/` 保持公开协议；`configs/header-export.list` 是用户态头导出的
唯一白名单。内核私有公共头位于 `include/reliefnt/`，任务结构和访问器分别在
`task/types.h`、`task/accessors.h`，不向 SDK 导出。

一个模块内的跨文件声明放在本模块的 `internal.h` 或具体职责的私有头中。
原先文件内共享的可变状态只有一个定义，其余文件通过私有声明访问；拆分不改变
调用方原有的锁、IRQ 状态、所有权或生命周期要求。跨文件可见性不构成用户 ABI。
调度器共享任务状态仍由原调度锁和 CPU 所有权规则管理；网络、存储和显示代码也
继续遵守原有执行上下文与锁顺序，不通过新增回调层改变调用顺序。

`fs/fs_internal.h` 保存文件系统族共享的磁盘类型与接口，
`block/core/storage_internal.h` 保存块层共享接口；文件系统和块设备的
结构类型由私有存储类型头共享。协议结构布局及 exFAT upcase 数据保持原样。

兼容 syscall 分发按原条件和顺序转交 `legacy_*.c`；多个 ioctl 前缀的处理保留
在同一处理函数内，保持原先允许继续匹配后续分支的行为。

## 本地 Kbuild

根 `Kbuild` 只列顶层子目录，并包含独立的产物规则。每个生产代码目录的
`Kbuild` 显式列出本目录对象及直接子目录，不通配扫描、不维护全局源码清单。
`scripts/Makefile.module` 提供共享的工具链身份依赖，
`scripts/Makefile.products` 管理生成头、最终链接、调试载荷和 UAPI 导出。
导入的 Linux 6.12 Kbuild/Kconfig 引擎保持原样。

vDSO 使用其专用 PIC/链接参数，只有 `vdso_blob.o` 进入内核归档；
`debug/kerneldebug.o` 单独生成 `kerneldebug.sys`，不进入内核归档。
宿主工具使用 `tools/host/Kbuild`；运行时宿主 fixture 由
`tools/tests/Kbuild` 一次构建，避免并行目标重复写共享对象。

产物路径保持为 `O/generated/system/{kernel.sys,kernel.debug,kerneldebug.sys}`。
父仓继续通过原有 `make kernel`、`make image-vmdk` 等目标集成。

```sh
make O=/absolute/output PROFILE=release -j8 all
make O=/absolute/output -j8 test-tools test-header-export test-vdso \
    test-boot-logo test-panic-console test-module-layout
make O=/absolute/output PROFILE=debug -j8 all
```

## 迁移追溯与检查

`docs/kernel-source-map.json` 记录本次迁移基线及旧文件到新文件的对应关系，
仅用于历史审计，生产构建不读取它。旧 `kernel/reliefnt/` 实现与私有头目录已
移除；历史报告中的旧文件行号属于相应历史版本，应结合该映射查阅。

`make test-module-layout` 检查各级本地 Kbuild、重复/遗漏对象、生产实现包含、
特殊 debug/vDSO 源文件覆盖和上游引擎哈希。它也属于默认 `make test`。
父仓 `tools/test_build_incremental.py` 从实际目录 Kbuild 读取预期归档顺序，
用隔离源码及输出验证源码/头/参数/工具身份和缺失产物的重建。

`tools/tests/modules/` 仅供旧白盒宿主测试组合真实实现，不进入生产构建；
父仓 `tools/kernel_test_sources.py` 为这些测试定位实际源码，避免测试复制的旧实现。
ABI golden 和历史测试失败均应单独报告，不通过改期望值掩盖。
