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

ext 普通文件的连续完整数据块以至多 32 块的暂存批次提交，避免逐块重复进入
设备层；大暂存分配失败时退回单块。块层继续按设备的最大请求拆分，运输层
仍拥有 DMA backing。写前失效覆盖整个批次，只有成功批次更新缓存；失败可能
已经部分写盘，保留错误且不提交后续批次。此优化不改变 journal 顺序、刷盘
或同步写入语义。``tools/test_ext4_write_batch.py`` 检查请求次数、低内存回退、
设备拆分及部分失败后的缓存失效，次数下降不等同于同倍数吞吐提升。

AHCI 的单命令上限为 128 扇区（64KiB），使用原有每端口 DMA 缓冲。NVMe 在 128KiB backing 上限内进一步遵守 Identify Controller 的 MDTS；MDTS=0 表示没有额外协议上限。每个 PRP 请求只覆盖设备实际会读取的有效表项，未使用尾部不再重复清零。请求所有权、取消与 STOP 协议不变。

公共块传输在一次读取超过设备最大请求时，临时进入同步上下文完成全部分块，成功或失败后都恢复调用方原有的异步与写入标志，并保持 PID owner。否则第二条命令等待后从头重试，会丢弃其未完成身份。此保护同样适用于 FAT32 整簇目录、查找缓存、FAT 表缓存以及其他共享读取入口；合法簇大小不受 NVMe MDTS 限制。只需一条原生命令的读取继续保留异步接续；没有新增跨调用进度或缓冲区所有权。

FAT32 大于预读缓存的合法簇通过现有簇缓冲区逐簇读取，保留碎片链游标、部分读取和错误语义，不扩大预读缓存。

原始磁盘读写使用稳定的 64KiB CPU 暂存区，并按设备的最大请求继续限长；暂存区由现有 execution/PID owner 保护。非对齐写仅预读需要保留的首尾扇区。块传输保留逐命令缓存失效，写入前保留全局缓存失效与 swap 冲突检查；读取接续不保留临时分配或旧用户指针。

ext 连续读取在单次调用内复用已验证的逻辑/物理映射值，跨映射时重新验证；不保留可能被驱逐的元数据缓存指针，也不跨调用缓存映射。块与 inode 分配搜索使用有界位图字，保留部分字边界、预留窗口及原来的分配选择规则。页缓存的 inode 驻留查询复用身份索引，避免扫描所有固定和溢出槽；已退休但仍被引用的物理页继续保持不可见。

批量读取只有在全部未缓存数据能由一条原生请求覆盖时保留异步接续。超过设备上限、超过 32 块工作区或混合缓存命中/缺失的范围，临时使用同步上下文，返回后恢复调用方原状态。这样重试前的 inode/extent 缓存驱逐或低内存不会导致已完成前缀反复重读，也不依赖跨 syscall 保留调用者缓冲。真实块层回归覆盖小请求上限、缓存耗尽、脏页、错误及上下文恢复。

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

``tools/test_ext4_io_search.py`` 使用独立位图与读取模型检查搜索和映射边界；``tools/test_disk_raw_batch.py`` 链接真实 installer、块传输与 registry，检查设备上限、边缘保留、部分错误、第二命令等待与取消。``tools/test_page_cache_growth.py`` 覆盖稀疏和溢出 inode 索引查询。局部扫描、映射或请求次数的下降不能直接换算为整机吞吐提升。

``tools/test_fat_read_split.py`` 使用真实 FAT 目录、块传输、registry 和上下文实现，检查设备上限小于簇大小时的读取进展、单命令异步接续，以及错误后的上下文恢复。

``tools/tests/storage_integrity_guest.c`` 使用独立字节模型验证不同大小的对齐及非对齐读写、稀疏文件、覆盖、EOF、截短再增长、fsync 与关闭后重开。block 模式只允许对至少 2229761 字节、初始测试范围为零的专用临时盘运行，会覆盖测试范围；不能用于已有用户数据的磁盘。

exFAT 启动区的 OEM 参数扇区整体作为不透明数据接受，允许标准或未知 OEM 参数；末尾保留字节按读取端规范不解释、不修改，整扇区继续由启动区校验和保护。独立保留扇区保持原有校验。扩展启动扇区检查完整的 32 位签名，卷标项仅校验字符字段，不将尾部保留字节当作字符填充。``tools/test_exfat_boot.py`` 覆盖主/备份启动区、OEM 参数、损坏签名和保留区、校验和、读取错误及卷标字段边界。

VMware 验证未完成；真实 NVMe/SATA 硬件、长时间压力和故障注入也不由
QEMU 通过结果代替。
