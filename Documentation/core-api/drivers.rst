==============
驱动与存储部件
==============

职责
====

drivers/ 按 console、tty、video、storage、net、input、usb、sound 等设备族
组织；sound/core 实现 PCM、mixer 及 ALSA/OSS 路线。block/ 负责块 I/O、GPT 与
设备身份，fs/ 各文件系统负责自己的元数据、缓存和同步。
默认生产配置全部内置；下表中的部件可选择 ``m``，使用相同的原生注册、
探测和撤销入口。模块针对匹配的 ReliefNT 构建，不兼容 Linux 原版二进制
模块。构建、安装和树外模块 SDK 见 :doc:`../kbuild/modules`。

模块边界
========

.. list-table:: 可模块化生产部件
   :header-rows: 1
   :widths: 28 24 48

   * - Kconfig
     - 模块
     - 归属与约束
   * - ``SATA_AHCI`` / ``PATA_IDE`` / ``BLK_DEV_NVME``
     - ``ahci`` / ``ide`` / ``nvme``
     - 原生控制器、IRQ 和 DMA；根设备引用阻止卸载。
   * - ``E1000``
     - ``e1000``
     - PRO/1000 设备；网络协议栈和回调入口保留内置。
   * - ``MOUSE_PS2`` / ``USB_UHCI_HCD``
     - ``mouse`` / ``uhci``
     - PS/2、VMware 鼠标与 UHCI HID；键盘基础输入和分发入口内置。
   * - ``SERIAL_8250``
     - ``serial``
     - COM1 终端提供者；早期诊断与 panic 输出不依赖模块。
   * - ``SND``
     - ``snd``
     - ALSA/OSS、PCM 与 mixer；通用字符设备入口内置。
   * - ``SND_HDA_INTEL`` / ``SND_INTEL8X0`` / ``SND_ENS1371``
     - ``hda`` / ``ac97`` / ``es1371``
     - 依赖 sound core，Kconfig 限制驱动的链接等级不高于 core。
   * - ``DRM``
     - ``drm``
     - DRM/vmwgfx 服务、资源与映射；启动 framebuffer 和文本控制台内置。
   * - ``FAT_FS`` / ``EXFAT_FS`` / ``ISO9660_FS``
     - ``vfat`` / ``exfat`` / ``isofs``
     - 对应文件系统；ISO9660 只读，格式化仅在对应实现支持时提供。
   * - ``EXT2_FS`` / ``EXT4_FS``
     - ``ext2`` / ``ext4``，共同依赖 ``ext_common``
     - 各自注册文件系统类型，共用实际 ext 引擎。隐藏公共选项取所需最高链接等级。

所有表中生产选项在 defconfig 中为 ``y``。``MODULE_TESTS`` 的 provider、
consumer、参数和故障模块只供显式测试，不作为生产设备或默认加载策略。
``allmodconfig`` 会同时打开测试选项；生成了测试模块不表示它们应自动加载。

调度、物理内存、页表、模块加载器、VFS/挂载树、通用设备注册与设备映射、
IRQ、DMA 分配及网络协议栈保持内置。初始 newc 根、内存块设备、tmpfs、proc、
sysfs 和 devfs 为启动与模块管理提供基础，也保持内置。这些核心没有独立的
可卸载生命周期，不提供无法安全使用的 ``m`` 选项。

``net/services/`` 的 DHCP、DNS、NTP 和旧同步 HTTP 客户端辅助代码仍属于
内置网络栈。这里没有独立的 HTTP 服务注册入口；HTTP 文件还提供普通 TCP
连接使用的地址解析帮助函数，不能把整个文件改成 ``m`` 后留下内置调用者。
因此它不属于上述生产模块集合，也不显示尚不能安全装卸的配置选项。

生命周期与引用
==============

设备注册记录包含实现 owner，外部引用与正在执行的回调分别固定原始模块。
注册动作本身不建立永久自引用，未发现硬件时允许模块注册成功但不绑定设备。
是否激活硬件应查看驱动和设备状态，不能只看 ``/proc/modules`` 的 LIVE。

卸载先拒绝新的使用者，再停止 IRQ/DMA、排空回调并释放资源，最后释放代码。
硬件未确认停止时保留代码和 backing，报告失败并允许诊断或重试。打开文件、
挂载、用户映射、fork 后的映射、异步音频工作与 GPU fence 都必须保留对应
实现；不通过强制卸载绕过这些引用，也不将模块静态区直接当作连续 DMA 内存。

磁盘与显示
==========

原始块写入遵守打开时 DAC、描述符模式与权限；重读分区表要求 CAP_SYS_ADMIN。
FAT 目录按需有界分配，长文件名按 UTF-16 wire 数据处理，不能使用未对齐主机读取。
默认 root 的 PARTUUID 选择与磁盘编号分开。

framebuffer 提供基本显示；DRM/vmwgfx 实现有界 SVGA-II/VGPU9 能力、资源、
命令验证与 fence，不承诺通用跨设备 DMA-BUF、atomic modesetting 或硬件 vblank。
未得到设备能力时不能伪造 render 节点或成功状态。
