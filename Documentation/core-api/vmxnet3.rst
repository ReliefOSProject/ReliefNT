VMXNET3 网络驱动
================

``drivers/net/vmxnet3/`` 为 PCI ``15ad:07b0`` 提供原生 VMXNET3 网卡
驱动。``CONFIG_VMXNET3`` 为 tristate，默认内置；设置为 ``m`` 时生成
``drivers/net/vmxnet3/vmxnet3.ko``，带有匹配该 PCI 设备的模块别名。
每个匹配的 PCI 功能独立分配 DMA 环、控制状态并注册一个网络接口；它们
使用既有网络设备接口和 IPv4/IPv6 协议栈。

驱动选择 VMXNET3 revision 1 和 UPT 1，使用一条发送队列和一条接收
队列，不启用校验和、分段、VLAN 剥离、RSS 或 LRO 等硬件卸载。接口
使用 1500 字节 MTU、软件协议校验和及轮询完成队列；中断保持屏蔽。
目的地址过滤允许本站单播、广播和所有多播，以支持 IPv6 邻居发现。

共享配置和环采用设备约定的 DMA 地址及代际位，发送完成须对应最早
尚未回收的描述符；接收完成须指向本队列预期的缓冲区。异常索引或
完成类型使该设备停止接收新的报文，避免复用不可信的 DMA 缓冲区。
QEMU 的 VMXNET3 模型将接收完成类型字段保留为零，驱动同时兼容
该取值与设备协议的接收完成类型。驱动不接受分片报文，并在错误或
长度超出缓冲区时丢弃对应完成。

设备停止时先同步 quiesce 和 reset，再关闭 PCI bus master 并回读；
未能证明停止时保留 DMA/MMIO 和模块所有权，以便管理器重试，而不
释放仍可能被访问的内存。设备命令是同步 MMIO 操作，队列满时发送
返回 ``EAGAIN``，不会无限等待硬件进度。

``tools/test_vmxnet3.py`` 编译真实驱动主体并模拟 PCI、MMIO 与 DMA
边界，覆盖版本协商、链路、收发环回绕、错误完成、资源回滚、失败
STOP 保留和多设备隔离。QEMU/KVM 测试使用两张 VMXNET3 和一张
e1000e，验证 DHCP、IPv4/IPv6、并发 TCP/UDP 数据和模块卸载、重载。
宿主模型和 QEMU 不能代替 VMware 实际环境验证；物理硬件也未验证。

协议字段根据 VMware Linux 驱动定义及 QEMU 的 VMXNET3 设备实现
核对：`Linux vmxnet3_defs.h <https://github.com/torvalds/linux/blob/master/drivers/net/vmxnet3/vmxnet3_defs.h>`_、
`QEMU vmxnet3.c <https://github.com/qemu/qemu/blob/master/hw/net/vmxnet3.c>`_。
本目录的实现为 ReliefNT 原创代码，遵循 Apache-2.0；不包含上述
GPL 源文件的实现代码。
