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
递归绑定、文件绑定和 bind remount 尚不支持，返回明确错误。
