==================
进程文件描述符
==================

ReliefNT 在原生 x86-64 Linux 系统调用 ABI 中支持 ``pidfd_open(2)`` 和
``pidfd_send_signal(2)``。PID 文件描述符是 Linux 扩展，不是 POSIX 接口；
关闭、复制、继承以及 ``poll(2)`` / ``select(2)`` 使用普通文件描述符规则。

``pidfd_open(pid, 0)`` 打开进程组内的线程组首领。``PIDFD_THREAD`` 可指定
单个线程，``PIDFD_NONBLOCK`` 可用于非阻塞等待。新描述符带有
``FD_CLOEXEC``。同一进程的多次打开具有相同的 inode 号；持有描述符不阻止
回收进程，也不会让旧对象在 PID 被复用时指向新进程。

进程描述符在整个线程组退出后报告可读；线程描述符只观察该线程。回收后
会同时报告 ``POLLHUP``，即使应用未订阅挂断。``epoll(7)`` 也支持退出
和挂断，不能用 ``read(2)`` 读取事件数据。``/proc/<pid>/fdinfo/<fd>`` 的
``Pid`` 在目标被回收后显示为 ``-1``。

发信号时按当前调用者与目标的凭据重新检查权限；成功打开描述符不授予
发送权限。默认作用域由描述符决定，可用 ``PIDFD_SIGNAL_THREAD`` 或
``PIDFD_SIGNAL_THREAD_GROUP`` 明确指定。``sig == 0`` 只检查权限和目标
是否存在；目标已回收时返回 ``ESRCH``。非空 ``siginfo_t`` 的信号号应与
信号参数相同，且不得对其他进程伪造内核产生的信号信息。

``waitid(P_PIDFD, ...)`` 只等待调用者有权等待的子进程，支持
``WEXITED``、``WSTOPPED``、``WCONTINUED`` 和 ``WNOWAIT``。默认也可等待同一
线程组内其他线程创建的子进程；``__WNOTHREAD`` 将范围限制到调用线程名下的子进程。
未观察到事件时，``WNOHANG`` 返回零和空的信号信息；没有显式指定 ``WNOHANG``
且描述符为非阻塞时返回 ``EAGAIN``。非子进程依然可以通过 poll 观察生命周期。

原生 x86-64 ``waitid`` 系统调用的第五个参数可指向 ``struct rusage``。
有事件时返回内核为该子进程线程组记录的用户态和内核态 CPU 时间；其他资源计数
目前置零，且不累计已由该子进程回收的后代的用量。没有事件时不写入该结构。

当前不支持 ``pidfd_getfd``、``CLONE_PIDFD``、``PIDFD_SIGNAL_PROCESS_GROUP``
或 PID 命名空间相关的 pidfs ioctl。不要将头文件中已有的系统调用号或
clone 标志视为这些功能已实现。
