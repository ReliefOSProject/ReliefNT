====================
存储中断与读取接续
====================

范围与接口
==========

NVMe 为 I/O CQ 申请 MSI-X entry 0，失败时尝试单消息 MSI；AHCI 使用
单消息 MSI。能力缺失或 PCI 向量预算耗尽时保留有界轮询。没有猜测共享
INTx 路由，也没有假定 AHCI 支持通用 MSI-X 端口映射。

用户态继续使用原有 Linux syscall ABI 的 read/pread、write/pwrite、
fsync、mount 和模块管理接口。内部 -EAGAIN 由 syscall 返回路径重试，
不把硬件等待变成普通阻塞文件操作可见的 EAGAIN。此处的“异步”指内核
读取接续，不表示已经实现 Linux AIO、io_uring 或新的 POSIX 接口。

完成与等待
==========

``block/core/storage_wait.c`` 持有永久存活的等待队列和完成代数。
syscall 在检查存储条件前取代数快照；驱动在返回 -EAGAIN 时提供命令的
绝对超时。所有驱动、transport 和文件系统调用栈退出后，syscall 尾部
才原子检查代数、链接任务并发布阻塞状态。先到的完成使代数变化，后到的
完成唤醒已登记任务，因此不依赖下一次 PIT tick 恰好重新轮询。
无中断能力的设备仍使用一 tick 回退，硬件超时和信号仍可结束等待。

锁顺序为 completion event lock、wait queue lock、scheduler lock。
NVMe 的短 completion lock 与任务侧 controller lock 分离；IRQ 不取
controller、transport 或全局 execution writer，不复制数据或恢复设备。
AHCI IRQ 在 W1C 前锁存端口状态，任务上下文同时检查实时及锁存错误，
避免 TFES/主机总线错误在状态确认后丢失。

SMP、事务与 DMA
==================

SMP 与单 CPU 使用相同的 execution writer 和磁盘事务 PID owner：
后续任务不能借用其他任务尚未完成的共享卷、缓存或 DMA 请求。重试重新
选择卷，后台内核 I/O 保存/恢复调用方的 replay 状态。此实现允许等待
设备时运行其他任务，并未取消文件系统共享状态的串行化。

NVMe 当前仍为每控制器一个 I/O SQ/CQ、一个在途请求和独立于调用者
地址空间的 DMA/PRP backing。AHCI 仍使用串行 pending 命令和各端口的
驱动 DMA。保存的 CPU 缓冲区指针只用于比较重试身份；完成拷贝只写当前
调用提供的有效内存。请求变化时排空并丢弃旧结果，退出时取消/排空，
不会回写退出任务的旧指针。

不可重放的写入、刷盘、启动及 reclaim/swap 路径保持同步。错误恢复先
证明控制器/端口 STOP，再重置队列或重用 DMA；STOP 失败保留 backing、
BAR 和模块。卸载先关闭新用户并禁用/排空 IRQ，再 STOP、释放资源。
没有在自旋锁或共享文件系统事务内保留 C 栈休眠。

尚未实现多请求 NVMe 并行队列、AHCI NCQ、多队列亲和性，以及可释放
文件系统事务的异步写回。这些扩展需要每请求 DMA、独立完成状态和
逐请求的引用/取消协议，不能仅增加 queue depth 或删除 owner 检查。

验证
====

ReliefNT 的宿主定向回归：

* ``tools/test_nvme_dma.py``：PRP64、跨页、异步重试、CQ phase/CID、
  旧缓冲区丢弃、取消和失败 STOP 保留。
* ``tools/test_ahci_completion.py`` 与 ``tools/test_ahci_dma.py``：真实
  ATA/ATAPI/IDENTIFY 命令、DMA、W1C 后错误证据、取消与 STOP。
* ``tools/test_wait_queue_race.py``：完成先到/后到、任务唤醒、超时提示、
  轮询回退及等待链接清理；使用真实核心等待队列。
* ``tools/test_kthread.py``：后台上下文隔离和四 CPU 配置下竞争 PID owner。

``tools/tests/storage_irq_guest.c`` 是针对真实 Linux/POSIX 用户态调用
编译的原生探针。ReliefOS 的 ``tools/test_storage_irq_qemu.py`` 消费
本仓 install manifest，创建私有 NVMe/AHCI ext4 磁盘，限速读取以触发
等待，并验证并发读写、fsync、终止读取、卸载重载后的内容及真实 MSI
计数；关机后由宿主 debugfs 再提取落盘文件并检查 SHA-256。限速用于
正确性验证，运行时间不能当成存储性能基准。

VMware 验证未完成；真实 NVMe/SATA 硬件、长时间压力和故障注入也不由
QEMU 通过结果代替。
