====================
资源容量与 Linux ABI
====================

本章说明资源分配策略与已实现接口的边界，不表示 ReliefNT 已完整实现
Linux 或 POSIX。内存耗尽、资源配额和硬件地址范围仍会限制资源数量。

容量与生命周期
==============

任务枚举直接遍历调度器的任务表。``/proc`` 的进程列表、线程统计以及
memfd 的可写共享映射检查不截断为前 64 个任务。描述符目录使用实际文件表
和 PTY 别名，不截断为前 1024 个描述符。

内部对象注册表按需扩容，扩容不移动对象，分配在自旋锁外完成。句柄保留
16 位槽号和 16 位代际编码，最多表示 65535 个同时存在的槽；此编码上限
仍然存在。查找句柄只返回借用引用，调用方必须按所属子系统的锁和引用规则
保证对象存活。

等待队列使用任务内嵌链表节点。IRQ 唤醒不分配内存；任务退出和取消等待
必须先脱链。队列本身的存储必须活到所有等待者脱离后才能释放。

epoll 监听表按需扩容；注册键由描述符编号和打开文件描述共同确定。
关闭一个编号后，``dup``、``fork``、排队中的 ``SCM_RIGHTS`` 或在途 I/O
引用仍可维持原对象的监听；重新使用该编号不会把旧监听转向新对象。
监听项只借用稳定的对象身份，最终引用释放时反向清除监听，不延长对象生命周期。
当前不支持嵌套 epoll，向 epoll 实例注册另一个 epoll 实例时返回 ``EPERM``；
直接 ``poll`` epoll 描述符只查看已就绪的监听项，不消费 ET/ONESHOT 状态。
原始 ``select``/``pselect6`` 系统调用按 ``nfds`` 复制位图，并检查
``RLIMIT_NOFILE``、溢出、无效描述符和用户指针。libc 的 ``fd_set`` 大小
由 libc 决定，不能用 ``FD_SET`` 操作其表示范围以外的描述符。

管道
====

管道对象与环形缓冲区分别分配，不再使用固定 256 项全局池。
``PIPE_BUF`` 保持 4096 字节；不超过此值的非阻塞写入具有全写或
``EAGAIN`` 的原子语义，较大写入允许短写。

``F_GETPIPE_SZ`` 查询可用容量；``F_SETPIPE_SZ`` 向上取整到至少一页的
二次幂容量。缩小到不足以容纳现有数据时返回 ``EBUSY``，失败不改变数据
顺序或容量。

默认容量为 16 页。当前每个 UID 的软预算为 16384 页；达到预算后，新建
管道使用两页。无资源管理能力的调用者不能把单管道扩大到 1 MiB 以上，
也不能通过扩容超出软预算。``CAP_SYS_RESOURCE`` 或 ``CAP_SYS_ADMIN``
可绕过这项资源策略；分配失败仍会返回错误。这些是内核策略常量，当前未
通过 sysctl 暴露，不能将其误称为已支持 Linux 的全部管道 sysctl。

参考：`pipe(7) <https://man7.org/linux/man-pages/man7/pipe.7.html>`_ 和
`F_GETPIPE_SZ(2const)
<https://man7.org/linux/man-pages/man2/F_GETPIPE_SZ.2const.html>`_。

匿名共享文件
============

memfd 使用不挂载的内部 tmpfs 实例。它不占用用户挂载卷槽，也不需要一个
与文件逻辑大小相同的连续物理内存块。逻辑扩展是稀疏的，实际页面按访问
分配，空洞读为零；不再使用 32 项专用池或桌面分辨率作为文件大小上限。

打开文件描述与 VMA 各自持有 inode 引用，关闭最后一个描述符不会提前释放
仍被映射的页面。``MAP_SHARED`` 保持别名一致性，``MAP_PRIVATE`` 使用 COW。
缩小文件先撤销受影响映射，再释放页面，重新扩展不能暴露旧数据。

seal 状态随共享文件描述持有。增加 ``F_SEAL_WRITE`` 时检查所有任务中可能
写入的共享 VMA，存在这类映射时返回 ``EBUSY``；私有 COW 映射不阻止封印。
封印后的新共享映射不能借助后续 ``mprotect`` 绕过写保护。

参考：`memfd_create(2)
<https://man7.org/linux/man-pages/man2/memfd_create.2.html>`_ 和
`F_GET_SEALS(2const)
<https://man7.org/linux/man-pages/man2/F_GET_SEALS.2const.html>`_。

终端与网络
==========

PTY 会话分块分配并保持稳定标识，任务的 PTY 描述符表按需扩容。
``dup``、``fork``、描述符传递和关闭仍遵守端点引用与
``RLIMIT_NOFILE``。六个虚拟控制台属于显示配置，不等于用户 PTY 数量上限。

UNIX、UDP、TCP 套接字注册表与 UNIX 监听队列按需分配。旧控制接口中的
定长连接快照只限制一次快照的展示数量，不限制实际存活连接数量。
单连接收发缓冲区仍有界，缓冲区不足按阻塞或非阻塞语义处理。

TCP 写入先排队，ACK 与重传由网络服务推进，阻塞调用经调度器停车。
非阻塞调用返回进展、短写或 ``EAGAIN``，不能等待远端 ACK。
本地 FIN 占用一个序列号并跟踪 ACK、RTO 重试和最终协议失败；FIN 未确认时
套接字不会被回收。本地写半关闭后仍可读取对端数据；收到对端 FIN 后读完
缓冲区才返回 EOF，本地发送方向仍可发送回复。``POLLRDHUP`` 表示对端
关闭发送方向，单向关闭不产生 ``POLLHUP``；双方关闭或连接终止才产生 HUP。
``SO_SNDTIMEO``、``SO_RCVTIMEO`` 默认零表示无调用超时；协议重传耗尽
仍可能使连接失败。旧专用网络控制请求的显式毫秒超时与标准 socket 调用
分别处理。正超时向上取整到调度 tick；Linux 兼容的负 ``tv_sec`` 表示
立即超时，查询仍归一为零值，与设置全零表示无限等待的行为不同。

系统调用入口、用户态定时器返回和空闲服务路径在执行事务内推进有待处理的
TCP 工作；硬件 IRQ 中不执行会分配内存的网络处理。单次维护预算有界，
空队列不扫描所有套接字。最后一个描述符关闭或进程退出后，已接受但尚未
确认的数据仍由网络栈持有并继续发送。

这些改动不等同于完整 TCP 标准实现。当前保留旧控制接口的连接状态编号，
完整关闭状态及 2MSL 策略、RTT 自适应 RTO、SACK、快速重传和窗口缩放仍
未实现；旧 DNS 和专用同步连接路径仍存在有界轮询。

参考：`epoll(7) <https://man7.org/linux/man-pages/man7/epoll.7.html>`_、
`RFC 9293 <https://www.rfc-editor.org/rfc/rfc9293.html>`_ 和
`RFC 6298 <https://www.rfc-editor.org/rfc/rfc6298.html>`_。

地址空间与驱动
==============

用户页表按四级层次稀疏分配，支持低 canonical 半区，独占上界为
``0x0000800000000000``。当前内核仍在低地址运行，保留
``[0x08000000, 0x0c000000)`` 的 supervisor 区间；用户最小地址、堆区和
栈增长窗口也仍有实现边界。这不是无条件可用的完整 128 TiB 连续区间。
``RLIMIT_AS`` 和页权限检查继续有效。

普通匿名及文件 ``mmap``/``mprotect`` 按请求分别设置读、写和执行权限，
允许显式 ``PROT_WRITE | PROT_EXEC``，以兼容 HotSpot 等 JIT 运行时。
没有请求执行权限的数据页保持 NX；再次移除 ``PROT_EXEC`` 会恢复 NX。
懒分配、驻留匿名页及私有 COW 保持同一权限语义。文件访问模式、只读挂载、
``noexec``、设备映射限制及 VMA 的 ``max_prot`` 上限继续生效，不能通过
后续 ``mprotect`` 增加被禁止的权限。

这对应 Linux 默认权限组合行为，不表示已实现 Linux 的 MDWE/LSM 策略接口；
SysV SHM 等独立接口的限制也不因此改变。参考
`Linux 6.12 权限及 MDWE 规则
<https://github.com/torvalds/linux/blob/v6.12/include/linux/mman.h>`_。
``tools/tests/jit_memory_runtime_probe.c`` 可在 Linux 和 ReliefOS 中运行，
检查 W→WX、RWX 映射、撤销执行权限的 NX 效果及 fork COW 隔离。

跨核屏障和用户 TLB 刷新在内部串行发布请求，使请求代际与操作类型在
所有 CPU 确认前保持稳定。等待锁或确认的 CPU 即使关闭本地中断，也会
处理其他 CPU 已发布的请求；返回时恢复调用前的中断状态。并行缺页中的
刷新也使用同一发布与确认路径。

懒加载 ELF 段只登记 VMA，较大的 BSS 不会提前分配覆盖全段的空页表。
实际访问时才建立页表和数据页。``fork`` 同时保护父子进程的私有可写页面，
写入后分别进行 COW；物理页引用计数超过 65535 仍保持完整所有权。
计数达到最终表示上限时页面保持固定，避免未计入引用导致提前回收。

内核访问物理页面使用 supervisor 直映射。当前 x86 PCI 后端采用直接 DMA，
设备描述符提交可寻址的物理地址，CPU 访问使用 ``mm_phys_to_virt`` 或驱动
API 回调；不能将两者混用。当前没有 IOMMU 或总线地址重映射实现，不能把
总线地址与物理地址相同的假设推广到其他平台。
当前物理分配器和直映射仍覆盖前 16 GiB，超出范围的 RAM 会产生诊断并
被排除；本次扩展的是稀疏用户虚拟映射，没有解除这项物理地址范围限制。

内部驱动 API 版本为 2，末尾增加 ``phys_to_virt`` 回调。驱动必须检查
版本、``struct_size`` 和回调存在后才使用。版本 1 的驱动需要重新构建，
内核在初始化前拒绝不匹配的模块；Linux 系统调用 ABI 不因此改变。
当前内置驱动随内核一起构建。

fbdev 的 ``smem_start`` 与用户设备映射的 PFN 保持物理地址语义，不能包含
内核直映射指针。显存与 FIFO 的物理保留范围也不能直接与 CPU 指针比较。
安装器根镜像的入口接收 Multiboot 模块物理地址，只在挂载实现内部转换。

参考：`Linux DMA 地址说明
<https://docs.kernel.org/core-api/dma-api-howto.html#cpu-and-dma-addresses>`_ 和
`Linux 6.12 fbdev UAPI
<https://github.com/torvalds/linux/blob/v6.12/include/uapi/linux/fb.h>`_。

验证入口
========

内核宿主套件由 ``tools/tests/host-suites.list`` 显式登记：

.. code-block:: sh

   make test-ownership test-host-smoke
   make test-host
   make test-header-export test-abi

``tools/tests/resource_capacity_runtime_probe.c`` 是原生 Linux ABI 容量探针，
``tools/tests/tcp_runtime_probe.c`` 连接测试编排提供的本地 TCP 对端。
二者可用 musl 静态构建。完整镜像、私有磁盘和 QEMU 编排归 ReliefOS 持有，
宿主 Linux 上通过只证明参考行为，不能代替 ReliefOS 来宾验证。
