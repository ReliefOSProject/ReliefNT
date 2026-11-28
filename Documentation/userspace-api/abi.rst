===============
系统调用与 UAPI
===============

约定
====

x86_64 使用 Linux syscall 编号与寄存器约定，未知接口返回 ENOSYS。
公开线协议位于 include/uapi，headers_install 由 configs/header-export.list
控制；私有 task、页表和驱动状态不导出为用户 ABI。固定宽度、长度、版本、
边界和负 errno 都是接口契约的一部分。

vDSO 的 ELF 符号版本、辅助向量、vvar 邻接映射及时间读取均有专用测试。
不支持的时钟或条件继续走真实 syscall，不返回猜测值。

兼容
====

公开头保留实际支持的兼容名称，旧用户库不能伪装成新 SONAME。
只实现的接口才可视为可用；头文件存在不能代替完整运行认证。
接口修改同步内核、导出头、用户态包装、消费者和必要验证，不以更新 golden
替代布局或语义审查。

按任务选择 uname 系统名
========================

``<reliefos/prctl.h>`` 提供以下不需要特权的 ``prctl`` 扩展：

* ``RELIEFOS_PR_SET_UNAME_LINUX``（``0x52550001``）：arg2 为 0 或 1，
  关闭或开启当前任务的 Linux 系统名；arg3、arg4、arg5 必须为零。
* ``RELIEFOS_PR_GET_UNAME_LINUX``（``0x52550002``）：所有参数为零，
  返回当前选择 0 或 1。

非法值及非零保留参数返回 EINVAL，状态不变。新任务默认使用 ReliefNT；
fork、vfork、clone 复制调用任务的选择，exec 保留选择。已经存在的其他任务
不受影响，线程也可独立设置该值。该开关仅将 ``uname(2)`` 返回的
``sysname`` 改为 Linux，其余字段、全局 UTS 状态及 procfs 均不改变。
使用标准 libc prctl 包装或原始系统调用均可；不改变 personality 数值语义。

目录 bind mount
===============

``mount(source, target, NULL, MS_BIND, NULL)`` 支持非递归目录绑定。
新挂载保留源目录的实际 inode、父链和文件系统生命周期，继承源挂载的
只读、nosuid、nodev 等策略。``/proc/self/mountinfo`` 的 root 字段报告源目录
在文件系统内的路径；命名空间路径、``..`` 和 chroot 边界按新挂载计算。
源文件系统在绑定仍存在时不能卸载，解绑不销毁源文件系统。

``MS_BIND | MS_REC`` 还会复制源目录下的挂载拓扑。子挂载在目的树中拥有新的
挂载 ID 和独立策略，但仍共享各自的源 inode 和文件系统。源目录之外的挂载不复制，
后续拓扑变更也不会传播。准备失败时目的树和现有挂载保持原样。

``mount(NULL, target, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL)``
只更新目标挂载点的策略，不读取源、类型或文件系统选项。源挂载、子挂载和共享
文件系统保持原样。省略 atime 选项时保留原策略；共享文件系统只读时，清除
挂载点的只读标志不会使其可写。

只读转换在目标挂载上仍有以写入模式打开的普通文件时返回 EBUSY，且不改变策略。
dup、fork 和文件映射持有的同一打开文件描述也会延续这项限制。其他挂载上的写者
或仅持有 O_PATH 不会阻止此转换。

文件绑定尚不支持，返回明确错误。

glibc 线程与 CPU 状态兼容
=========================

以下行为按 x86_64 Linux ABI 实现。``rseq``、futex PI 操作和扩展信号帧使用
Linux 的接口及布局。POSIX 线程、互斥锁和 CPU 时间接口由用户态 libc 提供。

rseq
----

注册后向用户区域发布实际运行 CPU，线程迁移后在返回用户态前更新
``cpu_id_start``、``cpu_id``、``node_id`` 和 ``mm_cid``。当前 NUMA 节点为 0，
``mm_cid`` 使用执行 CPU 的逻辑索引。抢占或信号中断活动临界区时，内核验证
描述符、地址范围及 abort 签名，清除 ``rseq_cs``，并跳转至 abort 地址。
无效描述符或不可访问的注册区域触发同步 SIGSEGV。

``fork`` 复制注册信息，共享地址空间的新线程独立注册，``exec`` 清除注册。
注册保留用户提供的临界区指针及 flags。glibc 可直接读取其 TLS rseq 区域以
实现 ``sched_getcpu``，并可按其自身配置关闭 rseq 使用。

PI futex
--------

实现 ``FUTEX_LOCK_PI``、``FUTEX_TRYLOCK_PI``、``FUTEX_UNLOCK_PI`` 和
``FUTEX_LOCK_PI2``，支持私有及共享 futex、竞争交接、死锁检查、超时和
robust owner death。共享等待队列保留物理页，退出时释放等待关系。
普通 futex 与 PI 等待队列混用返回 EINVAL。

``LOCK_PI`` 使用绝对 CLOCK_REALTIME 超时。``LOCK_PI2`` 默认使用绝对
CLOCK_MONOTONIC，可通过 ``FUTEX_CLOCK_REALTIME`` 选择实时钟。
PI 等待被信号打断后按 Linux 规则重启，包括未设置 ``SA_RESTART`` 的处理器。
已完成的锁所有权交接先结束该次等待，再交付待处理信号。

当前调度器仅接纳 SCHED_OTHER，PI 队列按相同有效优先级的 FIFO 顺序交接。
SCHED_FIFO、SCHED_RR 的实时调度与优先级提升仍需对应调度策略支持。
``FUTEX_WAIT_REQUEUE_PI`` 和 ``FUTEX_CMP_REQUEUE_PI`` 尚未实现，返回 ENOSYS。

CPU 时间时钟
------------

除当前进程和线程的固定 clock ID 外，支持 Linux 编码的进程及线程 CPU 时钟，
供 ``clock_getcpuclockid`` 和 ``pthread_getcpuclockid`` 使用。PROF 和 SCHED
报告用户态与内核态累计时间，VIRT 报告用户态时间。进程时钟汇总线程组。
线程目标遵守 Linux 的线程组限制。``clock_gettime`` 和 ``clock_getres``
按各自的目标验证规则处理编码 ID，非法 ID 返回 EINVAL。
当前时间统计及报告分辨率为 10 ms。

XSAVE 与信号帧
--------------

支持 AVX 的 CPU 启用统一的 x87、SSE、AVX XSAVE 状态，任务保存区保持
64 字节对齐，调度切换保留全部启用状态。缺少所需 CPU 能力时使用
512 字节 FXSAVE 状态。当前 XSAVE mask 为 0x7，保存区为 832 字节。

信号帧包含 Linux 的 ``_fpx_sw_bytes``、XSAVE bitmap 和末尾 magic。
``rt_sigreturn`` 验证对齐、长度、feature 位、保留字段和 MXCSR，并恢复
x87、SSE、YMM 状态。传统 fpstate 及空 fpstate 按其 Linux 语义处理。

定向验证
--------

``tools/test_linux_threads.py`` 覆盖 rseq 生命周期、PI 等待队列与信号恢复。
CPU accounting 和用户态返回路径另有宿主回归测试。
``tools/tests/glibc_runtime_probe.c`` 可由原版 glibc 工具链编译，覆盖
编码 CPU 时钟、rseq 迁移与临界区中断、PI mutex 竞争、信号、双时钟超时、
robust 恢复、AVX 调度切换和扩展信号帧。

此次使用未修改的 Debian glibc 2.41，在双核 QEMU/KVM 中分别验证启用
AVX/XSAVE 和关闭 AVX/XSAVE 的 CPU 配置。动态 ELF、pthread/TLS、
``posix_spawn``、``dlopen/dlsym``、localhost NSS 和普通 robust mutex
也已运行。关闭 AVX 时跳过 AVX 专项。该覆盖范围以外的应用及 glibc 版本
仍需各自验证。VMware 验证未完成，实体硬件未验证。
