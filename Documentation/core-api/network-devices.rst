========================
Multiple network devices
========================

ReliefNT 为每个受支持的 Ethernet PCI 功能分配独立网络接口。e1000、e1000e
和 r8169 可以同时注册，多张相同型号网卡也分别拥有设备状态和 DMA 环。

Device identity
===============

``lo`` 的 ifindex 为 1。硬件接口从 ifindex 2 开始递增，名称为
``eth<ifindex-2>``。接口在一次启动内保持名称和索引，移除后保留索引空洞。
例如卸载拥有 eth0、eth1 的驱动后，仍存在的 eth2 继续使用原来的编号。
重新加载驱动时，新设备获得新的索引和名称。

私有 ``include/reliefnt/net_device.h`` 的 ``driver_register_netdev`` 接收
版本化回调表、每设备上下文和索引输出。管理器复制回调表，将其与原驱动 owner
绑定，回调前固定 owner，释放服务锁后执行回调。初始化成功后发布接口。
驱动关闭回调接入、排空引用并停止自身全部设备后，管理器移除对应接口。
停止失败的设备保留 DMA 和模块代码，后续卸载可以重试。

旧 ``e1000_*`` 服务入口和 ReliefOS 旧网络管理请求查询首个存活接口。
旧驱动 API 表和公开用户结构继续使用原布局。新设备接口属于 ReliefNT 私有
内核接口，原生模块通过精确构建 ABI 关联。

Configuration and routing
=========================

每张网卡拥有独立的行政状态、IPv4 地址、掩码、广播地址和默认网关。
IPv4 路由优先使用最长前缀，再比较 metric 和接口索引。绑定本机源地址和
``SO_BINDTODEVICE`` 的 socket 按其约束选路。ARP 缓存按接口和下一跳地址
区分。TCP 路由变化时重新检查下一跳 MAC，向本机 NIC 地址发送的 TCP、UDP
保留目标接口身份。

IPv6 地址、路由、邻居、多播成员、待发包、重组和 PMTU 携带 ifindex。
逐链路状态通过动态分配管理，RA、RS、DAD、MLD 和发现定时分别推进。
不同链路可以使用相同 link-local 地址，socket 的作用域与设备绑定须一致。
现有 IPv6 地址、路由和邻居等共享表仍有各自的资源容量，耗尽时返回错误。

Linux 网络管理入口覆盖按名称、索引查询，IPv4/IPv6 地址和路由操作，以及
行政状态开关。接口配置修改需要 ``CAP_NET_ADMIN``。首次设置
``SO_BINDTODEVICE`` 允许普通进程，更改已有设备绑定需要 ``CAP_NET_RAW``。
AF_PACKET 和 raw socket 的创建权限继续由对应协议入口检查。

设备直接接收软件提交的帧，发送队列交由各设备的有界 DMA 环管理。
``SIOCGIFTXQLEN`` 报告的软件队列长度为 0。硬件 MTU 为 1500。

.. code-block:: sh

   ip link show
   ip addr show dev eth1
   ip link set dev eth1 up
   ip addr add 192.0.2.10/24 dev eth1
   ip route add default via 192.0.2.1 dev eth1 metric 100
   ip -6 addr add 2001:db8:1::10/64 dev eth1
   ip -6 route add default via fe80::1 dev eth1
   udhcpc -i eth1
   cat /proc/net/dev /proc/net/route /proc/net/arp /proc/net/if_inet6

``/proc/net/dev`` 列举所有已登记接口，包括未分配 IPv4 地址或行政关闭的设备。
``SIOCGIFCONF`` 按 Linux 的 IPv4 地址列表语义输出 loopback 和有地址的接口。
枚举硬件接口可使用 rtnetlink 或 ``/proc/net/dev``，并按名称查询 ifindex。

Validation
==========

``tools/test_net_device.py`` 验证多个 owner、独立回调、计数和索引生命周期。
``tools/test_net_multi_interface.py`` 验证 ioctl、rtnetlink 和逐接口选路。
``tools/test_net_multinic_data.py`` 验证 ARP、UDP 输出和 AF_PACKET 隔离。
既有 TCP、UDP 和 IPv6 定向套件覆盖对应传输的设备绑定、作用域与资源清理。

``tools/tests/multinic_guest.c`` 是标准 socket 用户程序探针，由 ReliefOS 的
``tools/test_multi_nic_qemu.py`` 放入私有来宾执行。来宾使用两张 e1000 和
一张 e1000e，验证独立 DHCP、IPv4/IPv6、并发 TCP/UDP，以及模块卸载期间
另一 owner 的活动 TCP 连接。完整桌面镜像与实体硬件的运行验证分别记录。
VMware 验证未完成。

References
==========

* `Linux network device socket interface <https://man7.org/linux/man-pages/man7/netdevice.7.html>`_
* `Linux packet sockets <https://man7.org/linux/man-pages/man7/packet.7.html>`_
* `IPv6 scoped address architecture, RFC 4007 <https://www.rfc-editor.org/rfc/rfc4007>`_
