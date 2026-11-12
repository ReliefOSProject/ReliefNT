==========================
Realtek PCIe 以太网驱动
==========================

设备与配置
==========

``drivers/net/r8169/`` 提供 ReliefNT 原生网卡驱动。``CONFIG_R8169`` 为
tristate，``make defconfig`` 默认选择 ``y``。选择 ``m`` 生成
``drivers/net/r8169/r8169.ko``，由普通 ``modprobe`` 根据 PCI 别名加载。

设备表包含 ``10ec:8168`` 和 ``10ec:8125``。驱动进一步读取 TxConfig
中的硬件版本，分别选择初始化、PHY 和固件流程。

.. list-table:: 已实现的芯片版本路径
   :header-rows: 1
   :widths: 30 70

   * - PCI ID
     - MAC 版本
   * - ``10ec:8168``
     - RTL8111/8168 B、C、CP、D、DP、E、E-VL、F、G、GU、H、EP、FP。
       按 OpenBSD 的版本掩码区分家族与 B/C 的 spin。
   * - ``10ec:8125``
     - RTL8125A ``0x60900000``、B ``0x64100000``、D-1 ``0x68800000``、
       D-2 ``0x68900000``。

未知 MAC 家族返回 ``-ENODEV``。驱动枚举全部匹配的 PCI 功能，为每张网卡
分配独立 DMA 环和上下文，通过私有 ``driver_register_netdev`` 注册
``eth0``、``eth1`` 等接口。网络用户程序使用 Linux socket、ioctl、netlink
和 proc 接口。接口生命周期和多网卡选路见 :doc:`network-devices`。

.. code-block:: sh

   modinfo r8169
   modprobe r8169
   ip link show eth0
   ip addr show dev eth0
   cat /proc/net/dev
   rmmod r8169

无网线时接口仍可查询和配置，载波状态来自硬件。PHY 自动协商覆盖
10/100/1000 Mb/s，RTL8125 另行通告 2500BASE-T 全双工。协商异步完成。
PHY、EPHY、CSI、固件握手和复位均有界等待，失败返回具体 errno。

硬件初始化中的微秒级稳定等待使用启动时间采样；短于 1 ms 的等待最多
轮询 4096 次，且采样期间关闭本地中断以避免 CPU 迁移。时钟停滞、回退或
达到轮询上限时改用 tick 等待；较长等待也使用 tick 等待，并额外加入一整个
tick，以覆盖请求恰好接近下一个 tick 的情况。等待返回后恢复原中断状态，
调用方不得持有自旋锁。短等待依赖内核启动时钟的精度；该时钟突然向前跳变
可能缩短等待。

收发和生命周期
==============

驱动优先映射 PCI BAR2，再尝试其他有效内存 BAR，支持 64 位地址。
PCI 电源管理使用常规 capability 链请求 D0。原始 MAC 在复位前读取并保留，
无效地址导致初始化失败。
早期 RTL8168 的控制 BAR 可能只有 256 字节，此时可通过该设备的 I/O BAR
访问同一寄存器组。PCI 内存映射仍要求完整页面位于有效 BAR 内。

RX/TX 各使用 256 个 16 字节经典描述符和 2048 字节缓冲区。RTL8125
显式选择经典布局。描述符保留物理 DMA 地址，CPU 通过独立映射访问数据。
所有权交接使用 acquire/release 屏障，发送与接收有独立的短临界区。
当前使用单队列轮询，MTU 为 1500，IP/传输层校验和由现有网络栈计算。

发送帧补零到最小 Ethernet 长度，再由硬件附加 FCS。队列满返回
``-EAGAIN``。接收检查错误、缓冲区与调用者容量，移除一个 FCS，并完整
丢弃跨描述符的异常分片链。接收过滤器接受本站单播、广播和多播，供
IPv4 ARP、DHCP 与 IPv6 邻居发现使用。

初始化先禁用 PCI bus master 并确认控制器复位，再分配和发布 DMA 队列。
卸载由模块 owner 关闭回调准入、排空在途调用，随后停止收发和复位。
停止失败时保留 DMA、MMIO 和模块代码以供重试。释放内存后 PCI bus master
保持关闭。

固件和许可
==========

RTL8125 的 MAC/PHY 固件以未经修改的常量寄存器对嵌入内核或模块，启动时
按芯片版本选择。构建消费仓库内数据。运行时无需网络下载。
固件在完成写入与握手后才更新版本标记，并回读确认。

硬件初始化改编自 OpenBSD src 提交
``7779d12194498048b288afda664dd6271a2ec016``。
RTL8111/8168 派生代码保留 BSD-4-Clause，RTL8125 派生代码保留 ISC，
固件保留 Realtek 的再分发许可。项目新增的 PCI/DMA/模块连接代码使用
Apache-2.0。完整声明见 ``NOTICE`` 和 ``LICENSES/realtek/``。
``modules_install`` 将声明放入版本化模块目录的 ``LICENSES/``，连同
模块加入安装所有权和校验清单，并随 ReliefOS 的内核载荷分发。

This product includes software developed by Bill Paul.

* `OpenBSD re.c <https://github.com/openbsd/src/blob/7779d12194498048b288afda664dd6271a2ec016/sys/dev/ic/re.c>`_
* `OpenBSD if_rge.c <https://github.com/openbsd/src/blob/7779d12194498048b288afda664dd6271a2ec016/sys/dev/pci/if_rge.c>`_
* `Realtek 固件数据与许可 <https://github.com/openbsd/src/blob/7779d12194498048b288afda664dd6271a2ec016/sys/dev/microcode/rge/rgemcode.h>`_
* `Realtek 提交的 RTL8125 DPDK 寄存器与描述符定义 <https://mails.dpdk.org/archives/dev/2024-June/295259.html>`_

验证范围
========

``tools/test_r8169.py`` 编译真实驱动对象并模拟 PCI/MMIO/DMA 边界，另行编译
真实寄存器与等待实现，检查时钟边界、中断状态和不同寄存器访问方式。
它覆盖 4 GiB 以上地址、收发回绕、长度与错误检查、补零、无载波、资源分配
失败、owner 冲突、停止失败保留与重试。芯片模型覆盖四个 RTL8125 版本和
RTL8168 家族路径。真实 PHY 协商、DMA 时序和实体网线收发仍需对应硬件验证。

本次 QEMU 回归覆盖默认内置启动，以及无匹配 Realtek 硬件时三次
``r8169.ko`` 加载与卸载。既有 e1000 的 IPv4、IPv6 ping 均通过。
测试使用当前新编译内核和私有 initramfs，BusyBox 来自已有 ReliefOS 构建。

本地 QEMU 只提供 RTL8139 Realtek 模型，真实 RTL8111/8168、RTL8125
需要实体网卡或正确的 PCI 直通验证。实体网卡验证未完成。VMware 验证未完成。
