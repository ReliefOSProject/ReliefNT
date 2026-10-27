=============
构建 ReliefNT
=============

准备与构建
==========

使用 Linux、GNU Make 4.3+、Clang/LLVM、compiler-rt、bison、flex、
pkg-config 和 ncurses 开发输入。从本仓根目录执行::

    make fetch
    make O=out/release PROFILE=release defconfig
    make O=out/release PROFILE=release -j4 all

fetch 获取锁定 Unifont，正常构建不隐式联网。配置在 O/.config；menuconfig
编辑，olddefconfig 保留并补充。工具链由 configs/toolchains 指定，显式命令行
覆盖目标工具；HOSTCC 独立。原版引擎摘要见 scripts/kbuild-source.json。

配置归属
========

Kconfig 只提供内核实际消费的选项。生产 defconfig 默认将驱动内建，
已完成生命周期支持的部件可选择 m，构建为针对 ReliefNT 的原生 .ko。
调度、内存、VFS 和启动基础保持内置；具体边界见 :doc:`../core-api/drivers`。
模块开发测试默认关闭；模块构建和 prepared SDK 见 :doc:`modules`。
编译策略仍由 PROFILE=debug|release 控制，工具链、版本后缀和
SOURCE_DATE_EPOCH 使用独立 Make 参数。

应用、系统镜像、激活策略和 QEMU 参数只属于 ReliefOS 主仓，内核不 source
主仓 Kconfig。从旧输出复制配置后运行 olddefconfig，会由原版 conf 移除外来符号。
``make test-kconfig`` 使用隔离输出验证该边界与重复同步的稳定性，并属于默认 test。
保留的 Linux Kconfig 引擎及其测试夹具是上游构建输入，不是本内核提供的配置项。

产物与版本
==========

产物在 O/generated/system：kernel.sys、kernel.debug、kerneldebug.sys。
vDSO 有独立 PIC/链接参数，仅嵌入对象进入内核。headers_install 按白名单导出，
install 发布带摘要的清单，不消费父仓私有实现。

版本来自 configs/build-version，数字宏与字符串须匹配，EXTRAVERSION、
LOCALVERSION 依次追加，再附加原生模块 ABI 身份后缀，完整版本最多 31 字节。
ABI 身份由源码、配置、工具链和编译策略产生，不使用构建时钟或输出路径。
SOURCE_DATE_EPOCH 默认为提交时间。
htmldocs 只生成 O/Documentation/html，不需要编译内核或初始化配置。
