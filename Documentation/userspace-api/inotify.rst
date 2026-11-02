============
文件事件通知
============

inotify 是 Linux 文件事件接口，不属于 POSIX。应用通过 libc 的
``<sys/inotify.h>`` 调用；内核的 ``<linux/inotify.h>`` 由
``headers_install`` 导出，不需要修改 BusyBox 或 musl。

接口与使用
==========

x86-64 syscall 编号与 Linux 一致：``inotify_init`` 为 253，
``inotify_add_watch`` 为 254，``inotify_rm_watch`` 为 255，
``inotify_init1`` 为 294。常见事件循环先以
``IN_NONBLOCK | IN_CLOEXEC`` 创建实例，为目录安装监听，再读取初始列表，
通过 ``poll`` 或 ``epoll`` 等待并读取通知。监听前发生的变化没有历史重放。

``read`` 返回完整的 ``struct inotify_event`` 记录；固定头为 16 字节，
可选名称包含 NUL 和对齐填充。容量小于下一条记录时返回 ``EINVAL``，
不拆分记录；非阻塞空队列返回 ``EAGAIN``。阻塞等待可被信号中断。
``readv`` 同样以每个 iovec 的容量容纳完整记录，不把一条记录拆到两个
iovec；``FIONREAD`` 返回队列内包含填充的总字节数。

``IN_CREATE``、``IN_DELETE``、``IN_MODIFY``、``IN_ATTRIB``、
``IN_OPEN``、``IN_CLOSE_WRITE``、``IN_CLOSE_NOWRITE``、
``IN_ACCESS``、``IN_MOVE_SELF``、``IN_DELETE_SELF`` 及移动父目录事件
来自实际 VFS 或 syscall 完成路径。重命名的 ``IN_MOVED_FROM`` 和
``IN_MOVED_TO`` 共用非零 cookie。目录事件按 Linux 语义附带 ``IN_ISDIR``。
内部页缓存读取和 ``mmap`` 缺页不产生用户读取事件。

监听以 inode 身份关联：重命名不把监听转移给同名新文件，硬链接指向同一个
监听对象。``IN_MASK_ADD`` 合并掩码，``IN_MASK_CREATE`` 拒绝覆盖已有监听，
``IN_ONLYDIR``、``IN_DONT_FOLLOW``、``IN_EXCL_UNLINK`` 和
``IN_ONESHOT`` 按各自语义处理。删除、显式移除和文件系统终止会生成
``IN_IGNORED``；卸载还生成 ``IN_UNMOUNT``。监听本身不保留路径引用，
不会仅因存在监听而阻止卸载。

``dup``、``fork`` 和传递的描述符共享同一个打开文件描述及队列；最后一个
引用释放时撤销全部监听。``CLOEXEC`` 属于描述符，``O_NONBLOCK`` 属于
共享打开文件描述。对该 fd 写入返回 ``EBADF``，定位读取返回 ``ESPIPE``。
``lseek`` 遵守 Linux 匿名 inode 的空操作行为，对支持的 whence 返回零。

容量与恢复
==========

root 可以通过下列 ``0644`` 控制文件读取或修改非负整数策略，普通用户只读：

.. list-table::
   :header-rows: 1

   * - ``/proc/sys/fs/inotify/`` 下的节点
     - 默认值及含义
   * - ``max_user_instances``
     - 128；每个真实 UID 的实例数，超限返回 ``EMFILE``。
   * - ``max_user_watches``
     - 8192；每个真实 UID 的监听数，超限返回 ``ENOSPC``。
   * - ``max_queued_events``
     - 16384；新实例的事件队列容量，已有实例保留创建时的容量。

零值可阻止后续资源创建；降低策略不会撤销已有实例或监听。实际内存分配仍可能
失败。通知分配失败或队列已满时，原文件操作保持其结果，队列提供
``wd == -1`` 的 ``IN_Q_OVERFLOW``。应用收到溢出后必须重新扫描，不能继续
把旧缓存当作完整状态。相邻相同的未读事件可合并，通知数量不是操作计数器。

使用边界
========

目录监听只覆盖直接子项；递归监听需要应用逐目录安装监听并处理竞态。
通知不能作为审计日志或保证每次写入单独报告，也不能保证读取通知时旧文件名
仍指向同一对象。读事件到无效用户地址时按 Linux 语义返回 ``EFAULT``，
该次已出队的记录不会恢复；应用应使用有效、足够大的用户缓冲区。

当前不提供 fanotify、网络文件系统远端事件或可选调试 ioctl
``INOTIFY_IOC_SETNEXTWD`` 的实现；不支持的 ioctl 返回错误，不伪造成功。

验证入口
========

``tools/test_inotify_core.py``、``tools/test_inotify_vfs.py``、
``tools/test_inotify_adapter.py`` 和 ``tools/test_inotify_proc.py`` 分别检查
实际队列/监听、VFS 生命周期、用户复制/等待及 sysctl 配额。
``tools/test_inotify_abi.py`` 在 Linux 上运行同一份 ABI 对照探针；
``tools/tests/inotify_runtime_probe.c`` 也可静态编译后在来宾执行。
系统镜像及 Fileman 验证脚本由 ReliefOS 仓库持有。

接口依据见 `Linux inotify 手册
<https://man7.org/linux/man-pages/man7/inotify.7.html>`_。
