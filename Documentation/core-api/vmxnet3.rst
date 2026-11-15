VMXNET3 网络驱动
================

``drivers/net/vmxnet3/`` 为 PCI ``15ad:07b0`` 提供原生 VMXNET3 网卡
驱动。``CONFIG_VMXNET3`` 为 tristate，默认内置；设置为 ``m`` 时生成
``drivers/net/vmxnet3/vmxnet3.ko``，带有匹配该 PCI 设备的模块别名。
每个匹配的 PCI 功能独立分配 DMA 环、控制状态并注册一个网络接口；它们
使用既有网络设备接口和 IPv4/IPv6 协议栈。

驱动选择 VMXNET3 revision 1 和 UPT 1，使用一条发送队列和一条接收
队列，不启用校验和、分段、VLAN 剥离、RSS 或 LRO 等硬件卸载。接口
使用 1500 字节 MTU 和软件协议校验和。
目的地址过滤允许本站单播、广播和所有多播，以支持 IPv6 邻居发现。

中断与收发调度
==============

驱动查询设备的中断偏好，优先使用一个 MSI-X 表项，其次使用 MSI；设备
明确要求 MSI 时直接尝试 MSI。不支持消息中断、分配失败或设备要求 INTx
时保留轮询回退，并保持设备中断屏蔽，不猜测共享 INTx 路由。消息中断
路由到 BSP，发送、接收和控制事件共用一个向量。MSI 与 MSI-X 共用
16 个向量，每个向量在一次启动内不复用；耗尽时新设备回退到轮询。

IRQ 回调仅屏蔽设备中断、刷新 posted write 并通知核心，不解析协议包。
核心以不复用的 ifindex 合并通知，不在排队期间保存模块回调或设备指针。
任务上下文每轮最多检查 32 个接收描述符，每张网卡连续最多检查 8 个，
满预算时重新排队；空队列才重新打开中断，并在打开后复查代际位，避免
丢失检查空队列期间到达的数据。原有协议维护心跳和同步轮询路径继续保留。

任务上下文消费并确认设备事件，包括 carrier 变化和队列错误。通知路径
不以旧 carrier 状态阻挡设备回调，否则链路恢复事件本身无法被处理。
行政关闭的接口仍排空和重新启用中断，但不把收到的数据投递到协议层。
中断模式使用事件更新的 carrier 缓存，轮询模式继续查询设备；接收服务
也回收发送完成。补充接收描述符时，仅在设备设置 ``updateRxProd`` 时
写 producer doorbell，减少每个包的 MMIO；初次投递仍显式通知设备。

资源与错误处理
==============

共享配置和环采用设备约定的 DMA 地址及代际位，发送完成须对应最早
尚未回收的描述符；接收完成须指向本队列预期的缓冲区。异常索引或
完成类型使该设备停止接收新的报文，避免复用不可信的 DMA 缓冲区。
QEMU 的 VMXNET3 模型将接收完成类型字段保留为零，驱动同时兼容
该取值与设备协议的接收完成类型。驱动不接受分片报文，并在错误或
长度超出缓冲区时丢弃对应完成。

每次回调均保留实际设备 owner。设备停止时先关闭接入、屏蔽并排空 IRQ，
再同步 quiesce 和 reset，关闭 PCI bus master 并回读；
未能证明停止时保留 DMA/MMIO 和模块所有权，以便管理器重试，而不
释放仍可能被访问的内存。设备命令是同步 MMIO 操作，队列满时发送
返回 ``EAGAIN``，不会无限等待硬件进度。

验证
====

``tools/test_vmxnet3.py`` 编译真实驱动主体并模拟 PCI、MMIO 与 DMA
边界，覆盖版本协商、链路、收发环回绕、错误完成、资源回滚、失败
STOP 保留和多设备隔离，以及 MSI-X/MSI/轮询选择、中断重新打开竞态、
carrier 事件和 producer doorbell 条件。``tools/test_pci_msix.py`` 检查
MSI-X 表与 PBA 的 BAR 边界、重叠、映射失败、表项屏蔽和旧向量隔离；
``tools/test_net_rx_budget.py`` 检查描述符预算与网卡之间的公平调度。

ReliefOS 的 ``tools/test_vmxnet3_qemu.py`` 使用两张 VMXNET3 和一张
e1000e，验证 DHCP、IPv4/IPv6、并发 TCP/UDP、真实 MSI-X 计数及模块
卸载、重载。``tools/tests/vmxnet3_perf_guest.c`` 通过标准 socket 验证
全部 TCP 回显字节和 UDP 数据，供父仓性能编排比较耗时、CPU 统计和
往返延迟。吞吐按单方向的回显有效载荷计算，不是双向加总或网卡线速。

QEMU 性能、VMware 基础联网、模块生命周期和长时间负载须分别记录，
不能互相代替。宿主模型和 QEMU 不能代替 VMware 实际环境验证；
物理硬件也未验证。此内部驱动与调度接口不扩展 Linux/POSIX 用户态 ABI。

协议字段根据 VMware Linux 驱动定义及 QEMU 的 VMXNET3 设备实现
核对：`Linux vmxnet3_defs.h <https://github.com/torvalds/linux/blob/master/drivers/net/vmxnet3/vmxnet3_defs.h>`_、
`QEMU vmxnet3.c <https://github.com/qemu/qemu/blob/master/hw/net/vmxnet3.c>`_。
本目录的实现为 ReliefNT 原创代码，遵循 Apache-2.0；不包含上述
GPL 源文件的实现代码。
