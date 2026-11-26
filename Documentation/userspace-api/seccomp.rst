========================
Seccomp 系统调用过滤
========================

接口范围
========

Seccomp-BPF 是 Linux 扩展，不属于 POSIX。ReliefNT 默认启用
``CONFIG_SECCOMP_FILTER``，使用 Linux 原生 x86-64 的 syscall 编号、
``sock_fprog``、``sock_filter`` 和 ``seccomp_data`` 布局。过滤器只读取
系统调用号、ABI、保存的用户指令地址和六个寄存器参数，不能解引用用户指针。
原生 ``syscall`` 指令和 ReliefNT 使用同一套寄存器约定的旧 ``int 0x80``
入口都受过滤；后者不是 Linux 的 IA32 兼容入口。纯用户态执行的 vDSO
函数不会进入内核，不经过该过滤器。

安装、查询与继承
================

* 使用 ``syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, flags, &prog)``
  安装，或通过不接受 flags 的 ``prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER,
  &prog)`` 安装。调用线程需要已设置 ``no_new_privs`` 或持有有效的
  ``CAP_SYS_ADMIN``。无效 BPF 不改变线程状态。
* 已安装的过滤链不可移除。允许的 fork、vfork 和 clone 继承调用线程的过滤链，
  成功 exec 保留调用线程的过滤链。每条程序最多 4096 条经典 BPF 指令；
  ReliefNT 按每个旧节点四条指令的附加开销限制整条路径的源指令预算为 32768。
* 通常只修改调用线程。``SECCOMP_FILTER_FLAG_TSYNC`` 原子更新线程组，
  同时向同组线程传递调用者的 ``no_new_privs``。不兼容的线程导致整体失败，
  返回该线程的 TID；同时指定 ``SECCOMP_FILTER_FLAG_TSYNC_ESRCH`` 时返回
  ``ESRCH``。不能把正 TID 当成安装成功。已进入内核的阻塞调用仍可完成；
  以后从用户态进入的调用按新过滤链检查。
* ``prctl(PR_GET_SECCOMP)`` 返回当前线程的模式：0（未启用）或 2（已安装）。
  ``/proc/<pid>/status`` 和 ``/proc/<pid>/task/<tid>/status`` 分别显示对应
  任务的 ``Seccomp`` 和 ``Seccomp_filters``。用
  ``SECCOMP_GET_ACTION_AVAIL`` 或只读的
  ``/proc/sys/kernel/seccomp/actions_avail`` 查询可用动作。
  禁用构建选项时，``seccomp(2)`` 返回 ``ENOSYS``，相应 proc 字段不出现。

动作与限制
==========

实际支持 ``KILL_PROCESS``、``KILL_THREAD``、``TRAP``、``ERRNO``、``LOG``、
``ALLOW``。组合过滤器时优先采用较严格的动作；同一优先级取最后安装的过滤器
提供的数据。``ERRNO`` 不执行原调用，即使选择的返回值为零；``TRAP`` 给当前
线程递送带原始调用号、架构、指令地址和过滤动作数据的 ``SIGSYS``。
``SECCOMP_FILTER_FLAG_LOG`` 请求记录被选中规则的非 ALLOW 动作，``RET_LOG``
记录后继续调用。内核日志限制频率，不记录调用参数，不是 Linux Audit 审计接口。

当前不支持严格模式、用户通知 FD、ptrace 的 seccomp 事件、``SPEC_ALLOW``、
``actions_logged`` 和核心转储。未实现的操作或安装 flags 返回 ``EINVAL``。
在过滤器中选中 ``USER_NOTIF`` 或 ``TRACE`` 时，按无监听者的语义返回
``ENOSYS``，不执行原调用；动作探测不把这两项报告为可用。

过滤器只限制进入内核的系统调用，不是完整沙箱，也不代表应用自身的沙箱已经启动。

参考：`Linux seccomp BPF 文档 <https://www.kernel.org/doc/html/v6.12/userspace-api/seccomp_filter.html>`__，
`seccomp(2) <https://man7.org/linux/man-pages/man2/seccomp.2.html>`__。
