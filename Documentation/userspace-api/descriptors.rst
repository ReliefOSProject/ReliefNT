======================
描述符、块设备与时间戳
======================

可移动块设备身份
================

``/sys/class/block/<disk>/removable``、``ro`` 和 ``diskseq`` 提供 Linux 风格
的可移动标志、只读状态及全局递增介质注册序号，``uevent`` 同时包含 ``DISKSEQ``。
``BLKGETDISKSEQ`` 返回打开描述符持有的整盘序号，分区描述符返回其所属整盘序号。
拔出后旧描述符保留原序号。重新注册分配更大的序号。``BLKROGET`` 与
``BLKSSZGET`` 分别返回真实注册的只读标志和逻辑扇区大小。

FAT32/exFAT 的显式挂载支持 Linux ``uid``、``gid``、``umask``、``fmask`` 和
``dmask`` 选项。UID/GID 使用十进制，掩码使用八进制。完整解析成功后才提交。
显式选项决定该文件系统实例的合成所有权和模式，ext 文件系统继续使用介质上的
POSIX 元数据。共享实例已有挂载时，不同选项返回 ``EBUSY``。无显式选项的
现有调用继续使用原有元数据策略。

接口语义参考 Linux 的
`块设备 ioctl <https://github.com/torvalds/linux/blob/master/block/ioctl.c>`_、
`diskseq 属性 <https://www.kernel.org/doc/html/v5.16/admin-guide/abi-testing.html>`_。

授权与身份
==========

打开时执行 DAC，读写检查描述符访问模式，O_PATH 不能进行普通读写。
重读分区表 BLKRRPART 要求 CAP_SYS_ADMIN，不依赖旧桌面角色。
utimensat(fd, NULL, times, 0) 更新该 fd 持有的 inode，改名或 unlink 不把它
重定向到另一个同名文件；O_PATH 应拒绝。

Linux 请求值
============

BLKROGET 使用 Linux 历史 _IO(0x12,94) 编码，旧错误 _IOR 编码不作为新契约。
使用旧头固化请求值的应用需以当前 UAPI 重编译。内部 timerfd/eventfd 状态
不能与 O_SYNC/O_TMPFILE 位重叠；同步操作要实际完成刷新并传播失败。
独占块打开应正确检查真实挂载冲突，不把匿名 RAM/光盘挂载误认为其他设备占用。

evdev 与 epoll
==============

``EPOLLET`` 的输入就绪判断包含提供者事件进度。evdev 读取者排空队列后，
下一批事件即使在再次调用 epoll 之前到达，也会产生新的 ``EPOLLIN``；无需
先由 epoll 观察一次空队列。未读取且没有新输入时不重复生成边沿，
``EPOLLONESHOT`` 仍须通过 ``EPOLL_CTL_MOD`` 重新启用。对 epoll 描述符的
poll/select 就绪查询与实际 ``epoll_wait`` 使用同一规则，其他事件位保持
原来的逐位判断。

evdev 进度只取对应设备、VT 和 OFD grab 可见的事件；另一终端、键盘/鼠标
之间及独占 grab 的隐藏输入不会推进该读取者的进度。原有 ``SYN_DROPPED``
溢出恢复、独立读取游标和用户事件结构不变，不以全局 poll 唤醒次数冒充
某个描述符的新输入。EPOLL 属 Linux 接口，不将其称为 POSIX 接口。

ppoll 与信号掩码
================

``ppoll`` 接受可选的 64 位临时信号掩码；原始系统调用的第五个参数在掩码非空时
必须为 8 字节。等待期间使用临时掩码，``SIGKILL`` 与 ``SIGSTOP`` 始终不可屏蔽。
就绪、超时或最终错误返回时恢复原掩码；调度重试和交换页等待保留调用拥有的状态，
取消调用时释放并恢复。信号中断返回 ``EINTR``，信号帧保存进入 ``ppoll`` 前的掩码，
由 ``rt_sigreturn`` 恢复，``SA_RESTART`` 不自动重启这个等待。

非空掩码不再返回 ``EOPNOTSUPP``，支持 OpenSSH 等程序在事件循环中临时解除信号屏蔽。
超时仍向上换算为毫秒并按调度 tick 等待；这不表示已经实现高精度纳秒定时。

流接收与进程目录
================

TCP 的 ``recvfrom/recvmsg`` 支持 ``MSG_PEEK``：重复预读保留相同的未读字节，
不前移接收游标，也不为尚未消费的数据重新通告接收窗口。``MSG_DONTWAIT``
只影响当前调用，不修改共享 OFD 的 ``O_NONBLOCK``。无数据时返回 ``EAGAIN``，
有序关闭且队列排空时返回零。未实现的 TCP 接收标志，包括 ``MSG_WAITALL``、
``MSG_OOB`` 与 ``MSG_TRUNC``，明确返回 ``EOPNOTSUPP``；不能静默消耗数据
或把短读冒充所请求的完整语义。UDP、packet 和 UNIX 的标志支持由各协议处理。
标志定义参见 `POSIX socket 头文件 <https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/basedefs/sys_socket.h.html>`_。

``/proc`` 数字进程目录的枚举 cookie 按 PID 递增，独立于调度器内部数组下标。
一个进程退出并触发数组压缩，不应导致仍存活的较大 PID 从同一次枚举中丢失。
达到最大 PID 后保留 64 位 EOF cookie，重新查找从零开始，不回绕到固定目录项。

挂载视图
========

``/proc/self/mounts`` 与 ``/proc/self/mountinfo`` 从实际挂载图生成，反映
``MS_MOVE`` 后的位置和父挂载关系。可见父挂载先于其子挂载输出；移动旧挂载
到较新挂载之下时也保持此顺序，不能以 mount ID 的大小推断父子关系。
设备 major/minor 与同一挂载内 ``stat.st_dev`` 的编码一致。

初始根挂载与 ``mount(2)`` 的默认访问时间策略一致：未显式选择策略时采用
``relatime``。这包括直接挂载的物理根、安装器与 live 的内存块根，以及
早期用户态的初始 tmpfs 根。明确指定 ``MS_NOATIME`` 或 ``MS_STRICTATIME``
时保留该选择；同时出现时 ``MS_STRICTATIME`` 优先。该默认值只作用于根的
挂载实例，保留底层强制只读及既有其他标志，不修改共享卷、ESP 或光盘等
其他挂载角色的策略。

查看根之外的挂载根不出现在 chroot 视图中；可见记录保留实际 ID，因此其
parent ID 可能位于查看范围之外。路径字段转义空格、制表符、换行和反斜线。
分段读取使用同一图顺序和字节偏移，不依赖反向插入的内部链表布局。
