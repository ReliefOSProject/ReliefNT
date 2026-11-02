======
procfs
======

procfs 由内核生成，通过 VFS 挂载在 ``/proc``；不需要用户态守护进程。
文件描述已有内核对象和策略。未实现的 Linux 子系统不会因为增加一个同名文件
而获得相应功能。

系统视图
========

.. list-table::
   :header-rows: 1
   :widths: 30 70

   * - 路径
     - 内容与边界
   * - ``loadavg``
     - 调度器的 1/5/15 分钟 Q16 负载、可运行/存活任务数、最近分配的 PID。
   * - ``stat``
     - 总计及每 CPU 的 user/system/idle、外部中断总数、上下文切换、启动时间、
       创建任务数和当前可运行任务数。
   * - ``meminfo``
     - 物理内存总量/空闲量、页缓存、tmpfs、Dirty 和已启用 swap 容量。
       ``MemAvailable`` 扣除应急保留并计入可证明能回收的缓存，不包含 swap。
   * - ``vmstat``
     - 空闲/缓存/tmpfs 页数、实际物理页分配/释放、逻辑用户 fault、换入/换出、
       direct/background 回收及 OOM kill 信号发布计数。
   * - ``swaps``
     - 已发布 swap 文件/分区的保留路径、KiB 容量/使用量及优先级，包含正在
       drain 的区域；每次打开捕获固定视图。激活条件见 :doc:`../admin-guide/swap`。
   * - ``partitions``
     - 当前注册整盘、GPT/MBR 分区和光驱的设备号、KiB 容量及设备名称。
   * - ``diskstats``
     - 整盘和光驱的实际块传输统计，采用 Linux 原始的 11 个统计字段。
   * - ``interrupts``
     - 每 CPU 实际收到的非零外部中断向量；不输出处理函数地址。
   * - ``net/dev``
     - 核心网卡服务观察到的收发字节/包/错误，以及本地 UDP 的 loopback 统计。
   * - ``net/route``、``net/arp``
     - loopback、已配置直连子网/默认网关，以及现存 ARP 缓存；读取不发起探测。
   * - ``net/tcp``、``net/udp``、``net/unix``
     - 现有端点、协议状态、实际队列、所有者和数值 inode；UNIX 地址支持抽象名。

CPU 时间以 100 Hz 调度 tick 结算，在 syscall、用户页错误和返回用户态的边界
区分 user/system。它不是高精度的 IRQ、softirq 或 iowait 计时；尚无独立记账的
CPU 列为零。当前阻塞任务均为可中断等待，``procs_blocked`` 不将它们冒充为
Linux 的 D 状态。任务和创建计数包含线程。

``diskstats`` 的扇区单位始终为 512 字节，包括 2048 字节块的光驱。
成功完成的传输才增加读写次数和扇区数；错误不算成功，``EAGAIN`` 重试不重复
累计成功传输。进行中请求、读写耗时、忙碌毫秒及加权毫秒来自真实请求生命周期；
取消会移除进行中请求。没有请求合并层，合并计数为零；没有分区、discard 或
flush 的独立统计行/扩展字段。同步统计不分配内存；异步记账记录分配失败时
不改变 I/O 返回值，但该请求的等待时间无法完整保留。
``partitions`` 解析未缓存的分区表时可能进行元数据 I/O，并返回真实传输错误。

进程与线程
==========

``/proc/self`` 指向调用者 TGID，``/proc/thread-self`` 指向
``TGID/task/TID``。根目录枚举线程组首领；存活线程也可直接通过 ``/proc/TID``
访问。``/proc/PID/task`` 仅枚举该线程组，拒绝外组 TID、数字溢出以及递归
``task/TID/task``。显式线程视图不再列出 ``task`` 子目录。

下面的叶节点同样适用于 ``/proc/PID/task/TID`` 和 ``/proc/thread-self``：

.. list-table::
   :header-rows: 1
   :widths: 28 72

   * - 叶节点
     - 内容
   * - ``status``
     - 名称、状态、四组 UID/GID、补充组、umask、虚拟/RSS/代码/数据/栈/锁定内存、
       线程数、信号掩码、capability、NoNewPrivs 和 CPU 亲和性。
   * - ``stat``
     - Linux 顺序的 52 个字段；user/system tick 分开，进程视图汇总存活组成员，
       显式线程视图只读一个线程；保留原有敏感地址字段的权限过滤。
   * - ``statm``
     - 以 4096 字节页为单位的虚拟量、RSS、共享映射、代码与数据/栈。
   * - ``maps``
     - 按地址排序的 VMA、权限、完整文件偏移、实际设备号/inode，以及持有对象的
       当前路径或 deleted 后缀；匿名区域标注 heap/stack/vDSO/vvar。
   * - ``smaps``、``smaps_rollup``
     - 逐区域或汇总的 RSS/PSS、共享/私有 clean/dirty、Referenced、Anonymous、
       Locked；swap 和未支持的匿名大页统计为零。
   * - ``limits``
     - 内核支持的七项资源限制：文件数、地址空间、待处理信号、栈、进程数、
       core 大小和锁定内存；不虚构其他 rlimit。
   * - ``auxv``
     - exec 构造的二进制 ``uint64_t type, value`` 数组，包括 ``AT_NULL``；
       不是文本，也不重新读取可能被用户修改的栈。
   * - ``io``
     - 已终结的 read/pread/readv/preadv/preadv2 及对应 write 调用数、成功字符数，
       以及调用上下文中成功块传输的字节数。
   * - ``fdinfo/FD``
     - 共享 OFD 的位置、用户可见打开标志、该描述符的 CLOEXEC、mount ID/inode，
       以及 eventfd/timerfd/epoll 的专项元数据；读取不消费事件或推进定时器。

PSS 通过遍历实际用户 PTE，按物理页的用户映射数分摊；共享同一地址空间的
``CLONE_VM`` 线程只统计一次，缓存引用和内核引用不增加分母。设备映射不计入
普通驻留页。Dirty/Referenced 来自页表标志。读取 smaps 需要扫描各地址空间，
并为映射计数分配临时存储，可能返回 ``ENOMEM``。

进程 ``io`` 汇总存活组成员，显式线程路径只显示相应线程。阻塞重试不会重复
计为用户调用；终结错误计为一次调用但不增加字符数。当前不包含 send/recv、
sendfile 等全部 Linux I/O 记账，也不保留已回收线程的历史组内累计值。
``cancelled_write_bytes`` 为零，因为当前没有延迟脏页写回抵消记账。

网络表、socket ``fdinfo``、``fd`` 链接及 ``fstat`` 使用一致的数值 inode。
这些标识来自对象序号/句柄，不是内核指针，也不保证跨启动稳定。

权限与读取语义
==============

``maps``、``smaps``、``smaps_rollup``、``auxv``、``io`` 和 ``fdinfo`` 复用
现有的 proc FSCREDS/dumpable/``CAP_SYS_PTRACE`` 检查。仅 UID 0 不构成绕过。
PID inode 保留任务身份令牌，PID 被复用后不会自动指向新任务。

新增文本文件及二进制 auxv 在成功打开时完整生成一份 OFD 快照；权限也在该时点
检查。随后短读、seek、EOF 均基于这份快照，进程退出或数据变化不拼接两代输出。
dup 共享已打开的视图，重新打开会生成新视图。快照不采用固定 4 KiB 截断；
分配失败返回 ``ENOMEM``。这与 Linux 各 proc 文件的实时/seq_file 行为存在差异。
原有 ``cmdline``、``environ``、mount view 和诊断文件仍遵守各自既有读取规则。
``/proc/sys/fs/inotify`` 下的控制值在读取时取当前策略，不使用打开时快照。

策略节点
========

以下节点是现行内核策略的只读快照，不支持通过写文件调整：

* ``/proc/sys/kernel/pid_max``：非零 32 位 PID 命名空间上限。
* ``/proc/sys/fs/nr_open``：描述符实现上限。
* ``/proc/sys/fs/file-nr``：实际已分配 OFD、零个闲置预分配 OFD、全局上限。
  dup/fork 别名不重复计数，排队、映射或进行中 I/O 保留的 OFD 直到最终释放才减计数。
* ``/proc/sys/fs/file-max``：没有额外全局 OFD 数量配额，以 ``UINT64_MAX`` 表示；
  仍受内存和每进程 rlimit 限制。
* ``/proc/sys/fs/pipe-max-size``、``pipe-user-pages-soft``、``pipe-user-pages-hard``：
  来自管道实现的现行容量、软限制及零值的未设置硬限制。

``/proc/sys/fs/inotify/{max_user_instances,max_user_watches,max_queued_events}``
是可写策略，root 可调整实例、监听和新队列的容量，普通用户只读。
默认值、超限错误及事件丢失恢复见 :doc:`inotify`。

验证入口
========

``tools/test_procfs.py`` 在 ASan/UBSan 下运行真实 proc/VFS 和 fdinfo 实现，覆盖
枚举、线程成员关系、权限、PSS、auxv、长快照、seek/EOF 与分配失败。
CPU、块设备、UDP/TCP/UNIX 和描述符生命周期由各自宿主测试覆盖。
``tools/tests/procfs_probe.c`` 可在原生 Linux 上作为 ABI 参考运行，也可由操作系统
使用其 SDK 编译后放入 ReliefNT 来宾执行；Linux 参考通过不能视为来宾验收。

``tools/tests/procfs_raw_probe.c`` 是无需 libc 的 x86-64 syscall 探针，编译参数
见文件头。它可作为小型 initramfs 的 ``/init``，通过 ``rdinit=/init`` 启动，
在串口输出节点短读、seek/EOF、maps/PSS、auxv、线程目录、克隆后的
``thread-self``、fdinfo 和真实 UDP 接收队列的验收结果。作为 PID 1 时，探针
输出结果后等待，不退出触发内核 panic。相同源码也由宿主 ``procfs`` 套件在
Linux 上运行；队列检查兼容 Linux 缓冲区占用与 ReliefNT 有效载荷字节的差异。
