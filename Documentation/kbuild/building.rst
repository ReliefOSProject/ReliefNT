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

产物与版本
==========

产物在 O/generated/system：kernel.sys、kernel.debug、kerneldebug.sys。
vDSO 有独立 PIC/链接参数，仅嵌入对象进入内核。headers_install 按白名单导出，
install 发布带摘要的清单，不消费父仓私有实现。

版本来自 configs/build-version，数字宏与字符串须匹配，EXTRAVERSION、
LOCALVERSION 依次追加，完整版本最多 31 字节。SOURCE_DATE_EPOCH 默认为提交时间。
htmldocs 只生成 O/Documentation/html，不需要编译内核或初始化配置。
