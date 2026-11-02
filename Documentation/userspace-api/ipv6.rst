==========
IPv6 双栈
==========

接口与报文
==========

IPv6 与 IPv4 共用描述符、等待队列、权限和网络执行上下文。应用使用 Linux
x86-64 socket ABI 和 POSIX socket 调用，不需要 ReliefNT 私有 IPv6 调用。
``AF_INET6`` 为 10，``sockaddr_in6`` 大小为 28 字节；地址和端口为网络字节序，
``sin6_scope_id`` 是接口索引。当前 ``lo`` 为 1，``eth0`` 为 2。
链路本地地址需要正确的区域标识。

实现包含 UDP、TCP、ICMPv6 raw socket、回环、以太网 IPv6、邻居发现、重复地址
检测、路由器发现和 SLAAC。地址与默认路由按通告有效期维护；SLAAC 地址遵守
RFC 4862 的两小时有效期保护。处理 IPv6 校验和、扩展首部、源端分片、乱序重组、
重叠分片拒绝及 Packet Too Big。IPv6 路由器不应在转发路径代替发送端分片。
协议维护在可调度的任务上下文执行，不在原始中断中进行阻塞内存回收。

IPv6 socket 默认允许 IPv4 映射地址；应用可在绑定前使用 ``IPV6_V6ONLY``
限制为 IPv6。``IPV6_RECVPKTINFO`` 和 ``IPV6_RECVHOPLIMIT`` 提供接收接口、
目标地址和 hop limit，便于实现标准协议客户端。保留 BusyBox 使用的旧版
hop-limit 辅助数据及 raw ICMPv6 checksum/filter 选项。未知选项返回错误，
不能将未实现功能报告为设置成功。

配置与观测
==========

``AF_NETLINK`` / ``NETLINK_ROUTE`` 提供链路、地址和路由查询，IPv6 地址和路由
修改需要 ``CAP_NET_ADMIN``。支持链路、IPv6 地址和路由变更订阅；接收队列
溢出报告 ``ENOBUFS``，调用者应重新查询。``getifaddrs()`` 可以通过标准查询
获取 IPv6 地址。保留 Linux IPv6 地址、路由 ioctl，并根据 socket family
选择相应参数布局。

``/proc/net/if_inet6``、``ipv6_route``、``tcp6``、``udp6``、``raw6`` 使用
Linux 文本布局；其中地址 scope 的编码与 rtnetlink 的 scope 值不同。
快照复制出内部状态，不借出跨等待有效的内核指针。

范围与标准
==========

这是 IPv6 主机实现，不提供 IPv6 路由转发、IPsec、MLDv2 源特定组播过滤或
Redirect 路由更新。DNS 策略、RDNSS 和 DHCPv6 客户端属于用户态。接收和发送
缓存有资源上限，资源不足返回错误；并不宣称覆盖 Linux 的全部网络功能或全部
IPv6 扩展。

依据 RFC 8200、4291、4443、4861、4862、8201、3493、3542 以及 Linux
``ipv6(7)``、``rtnetlink(7)``。POSIX 规定应用接口，线上 IPv6 协议细节由
相应 IETF 标准规定。宿主定向测试与完整系统 VM 测试是不同证据；VMware 和
真实硬件结果不能由 QEMU 结果替代。
