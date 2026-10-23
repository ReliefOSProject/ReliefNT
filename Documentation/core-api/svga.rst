========================
VMware SVGA 同步与资源
========================

提交和唤醒
==========

``drivers/video/svga/`` 实现 VMware FIFO/GB 协议；
``drivers/video/drm/vmwgfx/`` 负责 Linux vmwgfx ioctl 的验证、翻译与资源所有权。
公开 ioctl 编号和布局继续使用 ``include/uapi/drm/``，不新增私有加速协议。

FIFO 命令、内联 PPN 数组与 fence 完整写入后，以内存屏障发布 NEXT_CMD。
支持 BUSY 寄存器的 host 在 BUSY 为真时承诺睡眠前检查新命令，因此只在
空闲到忙碌时写 BUSY 和 SYNC；不支持该寄存器的旧 FIFO 保留 SYNC 唤醒。
查询已经完成的 fence 不重新唤醒 host。BUSY 仅表示处理活动，不能作为
命令完成或释放 DMA 页的依据。

借用 BO 后备页时，GMR2 每个映射包含至多 256 个 PPN，每次调用发布至多
四个包，不逐包等待 host。最新 fence 按 FIFO 顺序覆盖所有已发布映射；
完成映射及最终 fence 之前，MOB ID 不可用于提交。FIFO 压力导致重试时
保留已发布的完整前缀。拆除仍须等待最终 unmap/destroy fence，之后才能
释放 owner 引用和物理页。

损伤与呈现
==========

Surface-backed KMS PRESENT 只编码一个固定协议包和尾随 fence，经
``svga3d_present_try`` 原子发布；不为简单呈现分配通用 render-batch 的
shader/query/state 元数据。当前通用 sidecar 为 53008 字节，需要 13 个
物理页；这些分配和清零不再出现在每次 KMS PRESENT 的路径中。

该接口在同一个 SVGA 锁临界区检查设备、完整 surface handle、源/目标矩形
范围和 FIFO 容量。发布成功才更新 surface 的 last-use fence；FIFO 忙时
不发布部分命令，也不假定绘制完成。呈现 job 继续持有 framebuffer 和 surface
引用，直到真实 fence 完成或有明确 quiescence 证据，不能因关闭文件、释放
master 或切换 VT 就提前释放 DMA 后备页。

正常 DIRTYFB 仍立即尝试提交，只有背压才保留脏区域并合并重试，不按定时器
延迟成功提交。Linux UAPI 的 256 个 clip 上限不变。Xorg modesetting 在超出
上限时可能退回逐个 clip 提交，因此简单呈现的分配开销会被放大；离屏绘制
不经过这条扫描输出路径，不能用离屏速度代替桌面呈现速度。

EXECBUF 背压
============

在途工作受 32 个任务和 8 MiB 的全局预算约束。预算暂满时，先在释放
exec_lock 后执行一次有界回收，再原子重试预留；仍未腾出空间则返回 EBUSY。
不会扩大限额、循环持锁等待 host，也不会跳过尚未完成的 fence 来释放内存。
单次申请本身超过限额或真实 heap 分配失败仍返回 ENOMEM，并回滚预留计数。

Mesa 的 vmwgfx winsys 对 EXECBUF EBUSY 会短暂休眠后重试，其他错误可能使
Xorg/glamor 终止。把暂时的队列拥塞当作 ENOMEM 会在 GPU 落后于提交时导致
桌面退出，即使实际预算占用远低于物理内存容量。该行为参考
`Mesa 上游的 EBUSY 重试修复 <https://fuchsia.googlesource.com/third_party/mesa/+/20b78393920f138f6f521251df7f98768706089e%5E%21/>`_；
不修改 ioctl 编号或结构，也不把真实分配失败伪装成成功。

分配与锁
========

内部 ``svga_ops.alloc`` 返回独占、页对齐且已经清零的物理页；失败返回零。
生产实现使用 ``mm_alloc_pages``。GB 后备页和 shader staging 不重复清零。
页表中尚未写入的项和 shader 尾部填充仍由分配器的零页保证覆盖。

物理页分配器在 mm_lock 内认领完整页段、初始化引用并更新空闲计数，随后
解锁再清零，返回前不会向调用方发布物理地址。其他 CPU 不能分配已经认领
但仍在清零的页面。调用方已有的锁和 IRQ 状态保持原样；这不意味着所有
SVGA 资源分配都可抢占，也不能据此声称所有整机停顿已经消除。

SYNCCPU 兼容语义
================

Linux v6.12 的 ``drm_vmw_synccpu_allow_cs`` grab 等待先前 GPU 使用完成，
但不建立阻止后续命令提交的 CPU writer reservation。对应 release 对有效
handle 成功返回，不递减其他非 ALLOW_CS grab 的计数；无效 handle 和非法
flag 仍返回 EINVAL。非 ALLOW_CS grab 保留原来的 writer 排斥与关闭清理。

行为参考 `Linux v6.12 vmwgfx BO 接口 <https://github.com/torvalds/linux/blob/v6.12/drivers/gpu/drm/vmwgfx/vmwgfx_bo.c>`_；
此处按上游接口语义实现，不复制 Linux vmwgfx 实现代码。
协议头的原有许可证与归属说明保留；本次实现没有导入其他内核的 DRM 源码。

验证边界
========

宿主 fixture 需要覆盖旧 FIFO 唤醒、BUSY 状态、已完成 fence、未完成映射、
背压和拆除时的 owner 生命周期，并使用 ASan/UBSan 检查生产实现。
真实 VMware 验证还需检查 Xorg 日志确实启用 SVGA3D/glamor、正确像素、
重复会话、资源反复创建/销毁和并发负载。QEMU 的 VMware 型号设备不能替代
真实 VMware 3D 验证。未复现某次 panic 不等于已经证明其原因或修复。
