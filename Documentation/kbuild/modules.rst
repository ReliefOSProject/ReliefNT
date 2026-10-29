==================
原生内核模块与 SDK
==================

兼容边界
========

ReliefNT 使用 Linux 风格的 ELF64 .ko、Kbuild、模块元数据和符号版本机制。
模块必须针对匹配的 ReliefNT 内核重新编译；Linux 的内部内核 API 和原版
Linux 二进制 .ko 不属于兼容目标。POSIX 不定义内核模块格式或模块加载接口。

默认 defconfig 的生产驱动为 y；已开放的 tristate 可以在 menuconfig 中
选择 m。关闭 MODULES 时不能选择 m，内建配置仍可构建。测试模块由
MODULE_TESTS 显式开启，不进入默认产品。
完整生产部件及依赖矩阵见 :doc:`../core-api/drivers`。

构建与安装
==========

在 ReliefNT 源码根目录执行::

    make O=out/modules defconfig
    make O=out/modules menuconfig
    make O=out/modules -j8 modules
    make O=out/modules modules_install INSTALL_MOD_PATH="$PWD/out/module-stage"

modules 使用固定的 Linux 6.12 编译、modpost 和最终链接规则，生成
Module.symvers、modules.order、modules.builtin、modules.builtin.modinfo
与选中的 .ko。modules_prepare 准备宿主工具和生成头文件；它不替代生成
完整符号版本清单所需的内核构建。

完整原生 release 保存在 O/include/config/kernel.release，ABI 摘要在
O/include/config/module.abi。安装目录为 /lib/modules/<release>/kernel/，
树外模块使用 updates/。只清理由相应安装清单管理的旧产物，保留其他 release
和用户模块。覆盖已修改的受管文件会导致明确错误；不再生成的旧文件若被
用户修改则予以保留，不将它作为陈旧产物删除。

模块元数据
==========

默认构建的最终 ``.ko`` 导出表保留 Linux 风格的 ``__ksymtab_<符号名>``、
``__kstrtab_<符号名>`` 与 ``__kstrtabns_<符号名>`` 标签，供模块工具建立
符号索引。普通和 GPL 导出、函数和数据导出都使用这些标签；原生表项仍为
地址、名称和命名空间三个指针。``Module.symvers`` 记录类型 CRC，不能用
普通全局符号表代替实际导出清单。

依赖这些标签的模块工具需要保留本地符号。安装时 ``INSTALL_MOD_STRIP=1``
仅剥除调试信息，保留这些标签；自定义 ``--strip-unneeded`` 或
``--strip-all`` 会移除标签，不适用于依赖标签扫描的 ``depmod``。

``modules.builtin`` 列出内置模块路径；``modules.builtin.modinfo`` 保存
以模块名加点号为前缀、以 NUL 分隔的元数据。模块工具需要读取这些文件
才能查询内置模块及其别名。内置模块不写入动态 ``/proc/modules`` 列表，
也不生成用于冒充可加载模块的 ``.ko`` 文件。文件约定见
`Linux Kbuild 输出文件 <https://docs.kernel.org/kbuild/kbuild.html#output-files>`_。

树外模块
========

模块目录中的 Kbuild::

    obj-m += hello.o

hello.c 使用 reliefnt/module.h 的 module_init、module_exit 和
MODULE_LICENSE 等接口。导出符号须使用 reliefnt/export.h；外置模块采用
原生模块的地址模型，不能沿用内建内核的直接地址或静态 DMA 假设。

使用完整源码与已有输出时::

    make O=out/modules M=/absolute/path/to/module

也可以导出独立 prepared SDK::

    make O=out/modules modules_sdk INSTALL_MOD_PATH="$PWD/out/module-stage"
    release=$(cat out/modules/include/config/kernel.release)
    make -C "out/module-stage/usr/src/reliefnt-$release" M=/absolute/path/to/module

prepared SDK 带匹配头文件、生成配置、ABI、Module.symvers，以及原版
fixdep、genksyms、modpost 和 Kbuild 输入。它不带内核实现源码，也不通过
重新编译整个内核来构建树外模块。源码/输出树仍可用时直接调用 M=；部署后的
/lib/modules/<release>/build 指向同一前缀内的 prepared SDK。

该 SDK 支持与生成它的 Linux x86_64 宿主工具环境相匹配的构建。目标工具
必须具有匹配身份；允许同一工具搬到其他目录，并可显式传入 CC、AR、LD、
OBJCOPY。改变内核架构、PROFILE、编译策略或头文件应从完整源码重新构建并
导出新的 SDK，不能继续沿用旧 ABI 标记。SDK 本身可只读，构建产物与临时
文件写入 M= 目录；模块安装仍需指定可写的 INSTALL_MOD_PATH。
沿用 Linux Kbuild 的路径要求，SDK 和构建目录不能包含空白字符。

加载诊断
========

加载器校验完整 vermagic、类型 CRC、依赖和参数，并实现 Linux 编号的
init_module、finit_module、delete_module；调用者需要 CAP_SYS_MODULE。
finit_module 不支持的装载标志明确返回错误。delete_module 与 Linux 6.12
禁用强制卸载的配置一致，接受 BusyBox 使用的 O_EXCL，并忽略额外位；
O_TRUNC 也不会绕过引用、exit 或硬件停止检查。finit_module 使用独立文件快照，不移动共享
文件偏移；已有可写文件描述会阻止快照，快照期间也不能新增可写打开。

模块状态为 COMING、LIVE、GOING。LIVE 表示代码加载成功，是否发现并激活
硬件应检查驱动状态，不能把没有相应硬件的成功注册当作硬件验证。
打开设备、挂载、映射、依赖和执行中的回调会约束卸载；设备停止失败时保留
代码与 DMA 所有权以便诊断和重试。/proc/modules 与 /sys/module 提供状态视图。

模块参数遵循 Linux 风格的 ``module_param`` 类型、命令行解析和 sysfs 权限。
例如 DRM 的只读布尔参数 ``faildiag`` 可在内核命令行中写为
``drm.faildiag=1``，或在装载时使用 ``modprobe drm faildiag=1``。
内置驱动由内核参数框架处理，外置驱动由 modprobe 传入同一参数描述表；
诊断在初始化时设置。使用 ``0`` 或 ``N`` 可禁用，非法布尔值会拒绝装载。
只读或权限为零的参数不能从 sysfs 任意修改；卸载后的旧参数句柄不能指向
重新加载模块的存储。模块版本与导出符号类型 CRC 不匹配必须重新构建，
不提供忽略 vermagic 或 CRC 的强制装载选项。模块内存采用独立的代码、只读
和可写区域，页权限与直映射别名同时约束，所有 CPU 保持 WP/NXE。

未定义弱符号只有在确实没有对应导出时才解析为零。若同名导出存在，但类型
CRC、许可证或 namespace 不匹配，加载仍须失败；弱绑定不能绕过这些检查。
x86_64 加载器支持 NONE、64、32、32S、PC32、PLT32 与 PC64 重定位；
未知类型或不可表示的结果会被拒绝。32S 按符号扩展检查，合法负值不会仅因
其为负而被拒绝；越界时不写入目标位置。

初始化代码随模块保留到卸载，首版不提供 Linux 内部 ``__init`` API 或
初始化段提前回收。模块初始化结束并不能证明注册的回调已不再引用这些段。

根文件系统模块的启动加载、模块管理命令与镜像更新集成由 ReliefOS 构建链
负责；应分别验证编译、打包、BIOS/UEFI 启动和实际设备操作。
