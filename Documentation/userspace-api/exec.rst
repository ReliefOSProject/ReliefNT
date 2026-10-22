=================
exec 参数与初始栈
=================

``execve`` 与 ``execveat`` 使用 Linux x86_64 的参数容量规则，不能用较小的独立
环境项数量限制来代替字节预算。参考 Linux ``fs/exec.c:bprm_stack_limits()``
与 ``include/uapi/linux/binfmts.h``：

- 总预算为 ``max(128 KiB, min(6 MiB, RLIMIT_STACK.rlim_cur / 4))``。
  默认 8 MiB 软栈限制对应 2 MiB，软栈限制无限时仍有 6 MiB 上限。
- 预算包含字符串的 NUL、原始执行文件名和 argv/envp 的指针开销；空参数
  向量也为 Linux 补入的空 ``argv[0]`` 保留空间。
- 每个参数或环境字符串最多 32 个 4 KiB 页面，含末尾 NUL。
- argv 和 envp 各自的计数上限为 ``0x7fffffff``，实际可传数量先受总字节
  预算约束。超限返回 ``E2BIG``；不可读的用户地址返回 ``EFAULT``。

用户向量先计数，再在内核堆上分配暂存空间并按页复制字符串。并发改变向量
不能导致越过已分配容量；所有失败路径释放临时内存。任务保留的参数副本按
实际大小分配，fork 复制，exec 提交时替换，退出时释放。fork 不重新按照调用者
可能已调小的栈限制拒绝继承旧参数。

初始用户栈按实际布局补充页面，沿用栈增长的 ``RLIMIT_STACK``、``RLIMIT_AS``
检查和不可执行页面权限。新地址空间、ELF、栈和参数都准备完成后才提交
exec；失败保留旧映像、描述符和信号状态。内部 boot/service launch 请求仍
使用原有的小型固定请求结构，该结构不限制 Linux exec 系统调用。

ReliefOS 父仓中的 ``tools/test_exec_script.py`` 对生产参数复制和 shebang
重写运行 ASan/UBSan 测试；``tools/test_exec_limits.py`` 在 Linux 上运行边界
参照。对应的 ``tools/tests/exec_limits_runtime_probe.c`` 可以在 ReliefOS 内用
GCC 编译并运行同一组用例，以验证真实系统调用与初始栈布局。测试必须区分
宿主测试、内核编译和来宾运行，不以其中一项替代另一项。

参考：

- <https://github.com/torvalds/linux/blob/v6.17/fs/exec.c>
- <https://github.com/torvalds/linux/blob/v6.17/include/uapi/linux/binfmts.h>
- <https://man7.org/linux/man-pages/man2/execve.2.html>
