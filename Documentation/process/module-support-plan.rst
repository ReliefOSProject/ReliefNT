============================
可加载内核模块设计与实施计划
============================

状态：设计于 2026-10-24，已获准执行，实施中。以下核对项是完整交付范围，
不代表当前已完成；编译、宿主回归和来宾验证分别记录。源码调查基线为
``f0498b6e05d45adb14723127f8549fe564a48cc8``。

目标与约束
==========

用户已选择 Linux 风格机制，模块针对 ReliefNT 编译，不要求直接加载 Linux
原版二进制模块。为适合独立加载的生产部件提供 ``n/y/m`` 配置：``y`` 静态
链接，``m`` 生成可加载 ELF64 ``ET_REL`` 文件，``n`` 不构建。defconfig 将
目前内置的生产组件全部保持为 ``y``；测试模块默认关闭。

``CONFIG_MODULES=y`` 默认提供加载能力，并不意味着已有组件默认设为 ``m``。
``CONFIG_MODULE_UNLOAD=y`` 提供受引用计数保护的卸载。首版不提供强制加载、
强制卸载、模块压缩或签名验证配置；不得显示尚未实现的配置项。
导出的系统调用遵循 x86_64 Linux ABI；内核模块机制本身不是 POSIX 接口。

本计划只描述内核实现。ReliefOS 的配置委派、模块工具、initramfs 内容、
镜像、安装器和 RPR 计划位于父仓
``Documentation/process/kernel-modules-plan.rst``，不得移入内核构建图。
实施时使用 executing-plans 逐项验证；若获准委派，遵守用户指定的
GPT-6-Luna / Max。当前已授权实现；提交、推送或建立分支须另获明确授权。

方案与边界
==========

只让普通设备驱动生成外部文件，无法支持根磁盘和根文件系统设为 ``m``。
把驱动硬编码为不可模块化同样不能覆盖用户要求。因此推荐完整的
Kbuild、模块运行时、子系统注册和 initramfs 启动链。

保持内置的启动基础包括 CPU/中断、调度、内存、ELF 执行、syscall、同步、
VFS/块设备注册、PCI/设备注册、基础控制台、初始内存根以及模块加载器。
procfs/sysfs、memfd 必需的 tmpfs 基础及通用 socket/TTY 分发也保留在核心。
这些不是可以把自身也放到磁盘后再加载的部件。

拟提供 tristate 的边界：

* AHCI、IDE/ATAPI、NVMe，e1000，PS/2/USB 设备驱动，串口完整驱动。
* AC97、ES1371、HDA，以及完成通用设备分发与 owner 引用改造后的 sound core。
* VMware SVGA/DRM 及 DRM core；基础 framebuffer/启动日志保持内置。
* ext2、ext4、FAT、exFAT、ISO9660；公共 ext 辅助代码作为显式依赖。
* 具有独立注册边界的可选服务，例如内核 HTTP 服务。IPv4/TCP/UDP 核心不为
  追求选项数量强行拆为模块；必须先有真实注册和生命周期边界。

``allmodconfig`` 表示所有具备 tristate 能力的生产组件为 ``m``，不是每个
内核源文件都成为模块。交付前逐个目录列明 ``y-only / y-m-n`` 与理由。
尚未转换的目录不得提前显示可用的 ``m`` 选项。

调查基线与主要依赖
==================

以下描述设计调查时的基线，不能用作当前实现状态；阶段验收另行记录。

* ``Kconfig`` 当前没有功能符号，各目录 Kbuild 全部为 ``obj-y``。
* ``kernel/device/registry.c`` 硬编码六个内置驱动；运行时控制返回不支持。
* 只有上游 ``Makefile.modpost/modfinal/modinst`` 等规则，缺少
  ``scripts/mod``、``scripts/genksyms`` 和本地模块运行时。
* 根挂载先于普通驱动初始化；``fs/vfs/mount.c`` 直接调用 AHCI/IDE/NVMe
  及各文件系统。把对应对象改成 ``obj-m`` 会产生缺失符号或无法启动。
* 内核位于低地址，物理直映射在高地址。现有 ``-mcmodel=kernel`` 与任意高
  地址模块放置不能直接混用；必须检查重定位范围，不能截断到 32 位。

地址空间与构建格式
==================

首版提议为模块使用 ``-mcmodel=large -fno-pic -fno-pie``，其余 ABI 与核心
一致，在共享 supervisor 高地址区分配模块。这样不为此次功能整体搬迁
内核，不扩大低地址内核洞，也不侵占用户映射。该选择属于本计划的实现建议。
模块窗口拟为 ``[0xffffff9000000000, 0xffffffa000000000)``，与现有前
16 GiB 直映射分离；使用动态虚拟区间和非连续物理页，不新增小型槽池。

所有地址空间共享该内核页表分支。发布、权限变化与撤销必须同步在线 CPU
的 TLB；既存进程和后续 fork 的进程都必须看见一致映射。
模块映射的页表记录只用于装载器拥有页面、权限变化与释放；不把
``mm_virt_to_phys`` 放宽为接受模块静态区，更不接受任意高地址或用户地址。

模块静态区禁止直接充当 DMA 或用户 mmap backing：虚拟连续并不能证明
物理连续。此类缓存必须改用显式、带 owner 的内存对象，对 y/m 共用实现。
DMA API 建议为 ``dma_buffer_alloc(device, size, alignment, out)`` 与对应
free，返回 CPU 地址、独立设备总线句柄及实际长度；校验设备 DMA mask，
本平台以满足 mask 的物理连续页实现 coherent 分配，失败明确返回错误。
状态/control 等非 DMA mmap 页使用独立页对象；VMA 逐页持有 backing 与
模块 owner，不以“首地址转换加偏移”推导跨页物理地址。32 位 DMA mask、
跨页映射和设备停止前释放的失败路径必须有测试。

实现工具链实际产生的 x86_64 RELA，包括 NONE、64、32、32S、PC32、PLT32、
PC64；未知类型和范围溢出明确拒绝。模块的架构汇编也必须遵守此地址模型。
先在小模块上验证实际编译结果，再转换设备，禁止用忽略重定位掩盖错误。

加载期间的映射为 RW/NX；校验和重定位完成后分别设为 RX、RO/NX、RW/NX。
模块 text 的其他内核别名不得遗留可写执行通路。首版将所有已分配段保留到
模块卸载，不提供 Linux 内部 ``__init`` API 或初始化段提前回收选项。注册的
回调可能仍引用初始化代码，不能仅因 init 返回就回收。此项是相对于最初设计中
“证明无持久引用后提前释放”的保守实现选择，并非已实现提前回收。失败撤销
映射、依赖和注册，释放前先完成执行者排空。

接口与生命周期
==============

增加 ``include/reliefnt/module.h``、``moduleparam.h``、``export.h``，提供
``module_init/module_exit``、``MODULE_INFO/MODULE_LICENSE/MODULE_ALIAS``、
参数和 ``EXPORT_SYMBOL``。内置与外置使用同一初始化/注册实现；不维护两份
硬编码名单。为上游生成的 ``.mod.c`` 提供最小构建适配头，不宣称兼容完整
Linux 内核头文件或驱动源码 API。许可证元数据必须真实。

运行时按职责拆为 ``kernel/module/{elf,layout,symbols,params,lifecycle,info}.c``
和架构侧 ``arch/x86_64/module/``，各目录持有 Kbuild。主要接口为：

.. code-block:: c

   int module_load_image(const void *image, size_t size,
                         const char *params, unsigned int flags);
   int module_load_file(struct task_file *file,
                        const char *params, unsigned int flags);
   int module_unload(const char *name, unsigned int flags);
   bool try_module_get(struct module *owner);
   void module_put(struct module *owner);

传入加载器的是内核持有的稳定镜像/文件引用和已复制参数，不能在解析期间
再次解引用用户指针。原子解析、错误回滚和所有权说明须写入相邻 Doxygen。
新框架 ABI 独立于现有 driver API v2；不扩大旧 callback 表来代替符号导出。
旧驱动信息查询可作为适配层保留，不能再保留一套独立加载/卸载引擎。

模块状态至少覆盖 COMING、LIVE、GOING。引用分三类：其他模块的依赖、打开
设备、挂载和 VMA 等外部使用引用会阻止卸载；已进入/待执行回调的执行 pin
必须在关闭准入后排空；模块自己登记的设备、IRQ、定时器和 DMA 对象属于
清理账本，不能因登记本身产生永不归零的自引用。账本不等于允许忽略回调：
mask/STOP、同步取消和 callback drain 完成前不能释放相关代码与数据。
全局表锁只保护短事务，不跨 ELF I/O、初始化、等待或设备回调持有。
禁止重复初始化、同名并发加载、依赖环和卸载中的新引用进入。
init 期间产生的组件注册必须携带 owner 并受状态准入控制；外部任务不能
在没有有效 pin 的情况下进入 COMING 模块。初始化失败时先关闭准入并排空
已进入的执行者，再撤销注册和代码，不能仅因 init 返回错误就立即释放。

卸载先拒绝新绑定、检查引用，再停设备、排空 IRQ/工作及移除注册，最后
撤销代码映射。根磁盘、已挂载文件系统、打开的音频/DRM 设备或保留的映射
应拒绝卸载；没有安全 exit 的模块也不能强制移除。设备 STOP 失败时必须
保留仍会执行的代码和 DMA 所有权，并显示真实错误，不能制造成功卸载。

实现 Linux 编号的 ``init_module``、``finit_module``、``delete_module``，
要求 ``CAP_SYS_MODULE``。参数、标志、错误码及权限/坏指针的检查顺序逐项
对照 Linux 6.12。未知或未实现的 flags 明确拒绝，不忽略校验。
提供 ``/proc/modules``、``/sys/module/<name>``、参数及引用信息；内置状态
与外置加载状态分别表示，不能把“模块已注册”误报成“发现并激活设备”。

产物与 ABI 身份
===============

使用原版 Linux 6.12 的 obj-m、复合对象、modpost 和 modfinal 规则，补齐
其 host 工具和头文件依赖闭包，记录上游来源、哈希和许可证。不改导入的
构建引擎来绕过错误，项目适配放在本地 Makefile 片段。

新增 ``modules``、``modules_install``、``modules_prepare``、
``allmodconfig``、``allyesconfig``；``all`` 同时产出所选模块。
生成 ``Module.symvers``、``modules.order``、``modules.builtin``、
``modules.builtin.modinfo`` 和 ``.modinfo``；默认提供导出符号类型 CRC
校验，不声称稳定的跨内核模块二进制 ABI。

同一 release 数字的不同源码/ABI 配置不能共用一个可覆盖的模块目录。
建议自动形成带 ABI 身份后缀的 ``KERNELRELEASE``，由 release、用户版本
后缀和源码/ABI 配置/工具链指纹共同决定；数字版本及 OS VERSION_ID 不变。
最长 31 字节约束继续有效，超长须报错。该 ABI 指纹不能依赖编译时间或
产生 build_info/符号 CRC 的循环依赖。模块必须匹配完整 KERNELRELEASE
和导出符号版本；默认不允许跳过检查。

安装到 ``/lib/modules/<KERNELRELEASE>/kernel/``，附模块元数据、配置及
ABI 清单。prepared SDK 为树外模块提供匹配的头文件、生成配置和 symvers。
切换 ``m -> y/n`` 后，只清理本输出/安装清单拥有的陈旧模块，不动其他版本
和用户外置模块。关闭 MODULES 的全内置构建也必须成立。

实施顺序与可检查结果
====================

K1：模块契约与配置骨架
----------------------

文件：上述新头文件、``kernel/module/Kconfig``、根 Kconfig、各子系统
Kconfig、``arch/x86_64/configs/reliefnt_defconfig``。

* [x] 先增加 ``tools/test_module_config.py``：默认无生产 ``=m``、禁用
  MODULES 后 m 不可选、依赖不允许 y 引用 m，未知选项不伪装可用。
* [x] 定义参数、元数据、初始化和符号版本契约；先只接入测试模块开关。
* [x] 运行 ``make test-kconfig`` 和新配置测试，验证 defconfig 幂等。

K2：模块内存与 ELF/重定位
-------------------------

文件：``kernel/module/{elf,layout}.c``、``arch/x86_64/module/{memory,relocate}.c``、
``include/reliefnt/paging.h``、``arch/x86_64/mm/paging.c``、``mm/physical/mm.c``。

* [x] 先增加 ``tools/test_module_elf.py``：真实工具链 ET_REL、损坏节表、
  错误索引/长度、NOBITS、跨节 RELA、地址溢出、分配失败完整回滚。
* [x] 增加 ``tools/test_module_memory.py``：先创建进程再加载、fork 后可见、
  W^X 与直映射别名、跨 CPU TLB，并拒绝将模块静态区误用为连续 DMA/PFN。
* [x] 实现共享映射和严格重定位；验证 large code model，不给不支持的
  relocation 返回成功。旧低地址内核洞和用户空间边界保持原样。

K3：符号、状态机与 Linux syscall
--------------------------------

文件：``kernel/module/{symbols,params,lifecycle,info}.c``、
``kernel/syscall/system/modules.c``、syscall dispatch、procfs/sysfs。

* [x] 增加 ``tools/test_module_lifecycle.py`` 和
  ``tools/tests/kmodules/{provider,consumer,init_fail,busy}.c``。
* [x] 覆盖依赖引用、重复加载、未知符号、版本不符、并发 load/unload、
  初始化失败、初始化中嵌套请求，以及回调排空失败时保留代码；证明自有
  注册不会形成无法卸载的自引用，真正的外部使用仍正确返回 busy。
* [x] 接入三个 syscall、CAP_SYS_MODULE 和信息接口。宿主只跑隔离 fixture；
  真实模块装卸仅在私有 ReliefOS 来宾测试，不操作宿主 Linux 内核。

K4：完整 Kbuild 模块链
----------------------

文件：根 Makefile/Kbuild、``scripts/Makefile.{reliefnt,module,products}``、
新增原版 ``scripts/mod/``、``scripts/genksyms/`` 及已校验依赖，更新来源清单。

* [x] 增加 ``tools/test_kbuild_modules.py``：测试 provider/consumer 的 y、m、
  n 与复合模块、缺失导出及符号 CRC，先验证真实 modpost 失败诊断。
* [x] 实现 modules/modules_install/modules_prepare 和完整产品 manifest。
* [x] 覆盖 m-y-n 切换、源文件删除、清单陈旧项、O= 隔离、并发输出锁、
  空模块集和源代码归档构建；保留既有 kernel.sys/debug 输出地址。
* [x] 改造 ``tools/test_module_layout.py``，理解配置和复合对象，继续校验
  全部生产源码归属及导入引擎哈希，不能删去原有覆盖审计。

K5：设备注册与真实驱动模块化
----------------------------

文件：``kernel/device/``、PCI/块设备注册、``drivers/**/Kconfig`` 与各目录
Kbuild、相关 syscall 设备分发、``fs/vfs/mount.c``。

* [x] 建立带 owner 的设备/块设备/file-ops 注册；把启动阶段直接调用的
  AHCI/IDE/NVMe 扫描从通用根挂载中移回各驱动。
* [x] 先以 e1000 完成一个 y/m 双路径，再转换声卡、存储、输入、USB、
  串口和 GPU。缺少硬件时注册成功但不绑定设备，避免无硬件就让模块初始化失败。
* [x] 将静态 DMA、BDL/ring 和用户映射的 PCM status/control 迁移到显式
  backing 分配，验证 32 位 DMA mask、128 KiB ring、逐页 mmap 与 owner 保活。
* [x] 每个驱动先补对应 owner、IRQ/DMA、open/VMA 和错误撤销测试，再开放 m。
  DRM fence、音频 STOP、网络回调和 USB 定时工作必须有真实退出边界。
* [x] builtin 与 module 对相同硬件使用同一个 init/probe/注册入口；删除
  硬编码内置名单以及指向已去除静态实现的直接引用。

K6a：挂载树与任务路径引用
-------------------------

文件：新增 ``include/reliefnt/mount.h``、``fs/vfs/{mount_tree,path}.c``，
修改 ``fs/vfs/storage_backend.c``、公共路径解析、任务 root/cwd、dirfd 和挂载实现。

* [x] 先增加 ``tools/test_mount_tree.py``：父子挂载、覆盖挂载、MS_MOVE、
  任务 root/cwd、dirfd、跨挂载路径解析，以及仍持有旧根的路径/文件引用。
* [x] 选择真正的带引用挂载树：``struct path`` 绑定 mount 与节点，mount
  固定文件系统 owner；首版各任务共享一个挂载 namespace，但分别持有自己的
  root/cwd。暂不新增私有 mount namespace 功能开关。
* [x] 现有 volume 数据继续作为 FS 后端；不再以 mount_path 文本前缀管理
  挂载生命周期。迁移普通 mount、umount、chroot 与 proc 挂载视图；不保留
  一套供 initramfs 特用、无法解释普通进程路径的平行根路由。
* [x] 证明旧行为与权限保持，再开展 K6b/K7；任务退出、fork、openat 和
  MS_MOVE 失败回滚均须释放正确路径引用。

K6b：文件系统、sound/DRM core 与可选服务
----------------------------------------

文件：``fs/vfs/``、新增 ``include/reliefnt/fs_type.h``、各文件系统 Kconfig/
Kbuild、``block/install/``、``sound/``、``drivers/video/drm/``、可选服务。

* [x] 在 K6a 挂载树上建立 ``register_filesystem``/``unregister_filesystem``
  的 owner 注册，
  mount、inode/OFD、VMA 和在途 I/O 固定实现；格式化/缓存失效也经对应 ops。
* [x] 提取真实公共 ext 帮助代码并声明依赖，消除 VFS 对未启用文件系统的
  直接函数/全局数据引用。分别验证每种 FS 的 y/m/n 与挂载中卸载拒绝。
* [x] ALSA/DRM ioctl、poll 与 mmap 通过通用 file-ops 分发后，才允许其
  core 为 m；不得用一组永远返回成功的空函数代替缺失实现。
* [x] 公布最终模块化目录矩阵；没有独立安全边界的核心保留内置并说明原因。

K7：初始内存根与切换真实根
--------------------------

文件：新增 ``fs/initramfs/``、``kernel/init/``、Multiboot2 模块分类、
``fs/vfs/{mount_tree,path,initial}.c`` 与 mount/chroot/exec 路径、块设备 boot-module 后端。

* [x] 基于 K6a/K6b 增加 ``tools/test_initramfs.py`` 和 ``test_root_switch.py``：newc
  边界、路径与链接、设备节点、模块物理地址、截断档案、旧根引用与清理。
* [x] 将外置 newc 档案解包到内置初始根，执行 ``/init`` 作为 PID 1。
  实现并验证用户态 switch_root 所需 mount-move/chroot/exec 语义，不能只
  更换一个全局根指针。``init=`` 与 ``rdinit=`` 职责分开。
* [x] 安装器 ext4 大载荷保留为单独 Multiboot 模块，注册内存块设备后由
  早期用户态挂载；不得让 ext4 模块依赖先挂载它自己，或复制 1.5 GiB 载荷。
* [x] 全内置且无 initramfs 的独立内核启动仍走有效的直接根挂载路径；
  两种入口共用已注册设备/FS，不保留第二套旧硬编码扫描实现。

K8：内核验收与手册收尾
----------------------

* [x] 将新测试登记 ``tools/tests/host-suites.list``；内核探针归本仓，QEMU
  编排归父仓。分别验证 debug/release、全内置、混合、allmodconfig、MODULES=n。
* [x] 记录现有 10 个宿主失败及 driver ABI golden 未决状态，不重生成 golden
  或改音频 manifest 来制造通过；新失败必须修复并保留 RED/GREEN 证据。
* [ ] 验证 default 构建性能与原生资源/TCP 探针无回归；large code model
  下的模块路径单独记录性能，不因可加载而忽略明显退化。
* [x] 更新 core-api/drivers、modules、kbuild 手册；删除“所有组件永远静态
  链接”等已失效描述，不改写历史 release 事实。严格构建 RST 文档。

验收门槛
========

不能仅凭生成 .ko 或 modinfo 能读取信息宣布完成。必须由父仓测试证实：
默认全内置、混合模块，以及全部可模块化部件为 m 的镜像可启动；根控制器
与根文件系统均为 m 的组合能进入真实用户态和桌面；busy 卸载被拒绝，空闲
模块能循环装卸，损坏模块被拒绝且系统仍可继续工作。

“不因模块化出错”指合法依赖配置和受支持设备，不代表磁盘缺文件、OOM、
不匹配 ABI 或故障硬件也必须成功。这些情况须准确报错并保留恢复入口。
QEMU、VMware、真实硬件分别报告；VMware 验证未完成时必须明示。

规范参考
========

* `Kbuild 模块规则 <https://docs.kernel.org/kbuild/modules.html>`_
* `Kbuild Makefiles <https://docs.kernel.org/kbuild/makefiles.html>`_
* `Kconfig 语言 <https://docs.kernel.org/kbuild/kconfig-language.html>`_
* `Linux 内核内部接口边界 <https://docs.kernel.org/process/stable-api-nonsense.html>`_
* `initramfs <https://docs.kernel.org/filesystems/ramfs-rootfs-initramfs.html>`_
* `Linux 6.12 x86 模块重定位 <https://github.com/torvalds/linux/blob/v6.12/arch/x86/kernel/module.c>`_
