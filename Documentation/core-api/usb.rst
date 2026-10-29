=======================
USB 2.0 存储与基本输入
=======================

配置与范围
==========

``CONFIG_USB_EHCI_HCD`` 选择 PCI EHCI 主机驱动，``y`` 内置，``m`` 生成
``drivers/usb/ehci/ehci.ko``。defconfig 默认内置。USB 的通用注册、延迟维护、
boot HID 解码和 BOT/SCSI 协议代码内置；控制器、设备绑定与 DMA 属于 HCD
owner，不产生无法卸载的自引用。

高速设备由 EHCI 处理。低速/全速设备直接接在 EHCI 根端口时，交给 companion
控制器；VMware USB 2.0 配置应同时启用 ``CONFIG_USB_UHCI_HCD``。USB 2.0 Hub
后的低速/全速设备使用 transaction translator。USB 端口归 xHCI 控制器所有
时不能使用此 EHCI 驱动；这项功能不包含 xHCI、OHCI、UAS、等时传输或任意
HID report descriptor 解释器。

HID 支持 boot subclass 的键盘和相对鼠标，选择 boot protocol 后使用标准
8 字节键盘报告和至少 3 字节鼠标报告。鼠标 Y 方向为 USB 的向下为正，
移除设备时释放仍按下的键和按钮，输出使用已有 Linux evdev 输入 ABI。
非 boot/NKRO 键盘、仅支持绝对坐标的数位板不属于当前支持范围。

存储协议
========

支持接口 ``08/06/50``：SCSI transparent command set、Bulk-Only Transport。
CBW 固定 31 字节，CSW 固定 13 字节；签名、tag、状态、residue 与实际传输
字节数都必须一致。端点 STALL 按 BOT 规则清除，phase error 执行 class reset、
清除 IN halt、清除 OUT halt。已部分执行的写请求不会自动重放。

每个接口最多接纳 16 个 LUN，支持 READ/WRITE(10/16)、READ CAPACITY(10/16)、
REQUEST SENSE、MODE SENSE 与 SYNCHRONIZE CACHE。每次块请求最多 128 个
512 字节扇区。当前文件系统/块分区路径要求逻辑扇区为 512 字节；其他几何
返回 ``EOPNOTSUPP``，不会伪装容量或扇区大小。写保护设备可查询分区并只读
挂载；块层写入返回 ``EROFS``。

驱动查询当前 caching mode page 08，必要时扫描完整 all-pages 响应，校验
MODE SENSE(6/10) 的实际响应长度、块描述符和完整页面。仅当设备明确报告
WCE=0、该 LUN 从注册起只读，或本驱动从未尝试向该 LUN 写入时，
flush 才无需发送缓存同步命令；写回缓存或未知状态仍执行 SYNCHRONIZE CACHE，
并保留其失败。仅在设备明确以当前 sense 拒绝 10 字节命令格式时，尝试相同
全盘同步语义的 16 字节命令。传输恢复、复位或无法确认的 sense 使缓存模式证明失效，
不会通过忽略错误宣称数据已持久化。
首次写请求提交前就锁存写入尝试状态，失败、复位和成功同步均不清除它；
该优化只免除没有本驱动写入需要提交的同步，不证明未知缓存的写入可靠性。

GPT、DOS MBR/EBR 分区及不分区的文件系统均可读取。MBR/EBR 的范围、重叠、
扩展链循环和元数据占用先完成校验；损坏 GPT 的 protective MBR 不用于
绕过 GPT 校验。现有磁盘分区编辑仍要求 GPT，不会隐式转换 MBR。

没有介质的读卡器和介质更换需要重新拔插。运行中报告介质身份/几何变化或
无法确认的命令完成结果时，旧 LUN 被撤销，旧打开不会转向新介质。

DMA、执行与移除
==================

固件接管先只读取已分配 BAR0 的 capability header，通过标准 legacy support
semaphore 交接，然后停止控制器与 bus mastering，最后探测完整 BAR 范围。
映射前不会为了探测大小覆盖仍被固件使用的 BAR。

所有 QH、qTD、帧表、setup 与数据缓冲来自 HCD 保留的连续页，使用 32 位 DMA
mask。普通 CPU/栈指针不会直接交给设备；控制和 bulk 使用独占 bounce buffer。
更换异步/周期调度前等待对应 schedule status 清零，STOP 无法确认时保留
backing 和模块代码。等待使用独立 ACPI PM timer，不依赖 PIT 中断或其他 CPU
获取内核执行锁；没有可用 PM timer 的平台当前拒绝启用 EHCI。

IRQ 只检查已完成 HID 报告、端口变化并安排维护。枚举、hub 请求、端点恢复和
设备回收在任务上下文运行。系统调用入口、调度器空闲唤醒和用户定时器返回
均可处理待办；后两者使用调度器专用内核栈，避免维护依赖新的用户系统调用。
主机 gate 覆盖一条完整 SCSI 事务；忙时返回
``EAGAIN``，不会交叉使用另一条请求的端点 toggle、CSW 或 bounce buffer。

周期端点在发布前分配帧/微帧 phase，按最坏 bit stuffing、主机开销和 hub
think time 估算带宽。高速每微帧最多使用 100 微秒；同一 TT 的下游工作
使用不跨帧的保守窗口，资源不足返回 ``ENOSPC``。Hub 默认 alternate setting
作为 single-TT 使用。分离事务发生传输错误或中止后，使用实际高速 Hub 的
``CLEAR_TT_BUFFER`` 清理；失败状态保留，在该 TT 接收下一条分离事务前重试。
调度预算中的 10 微秒主机延迟及低速 setup 余量是当前控制器假设；USB 规范
没有为所有硬件给出同一延迟上界，物理控制器与 Hub 的时序仍需分别验证。

物理移除先关闭块设备新引用，已有 OFD/挂载保留原 dev_t、回调上下文和 owner，
后续 I/O 返回 ``ENODEV``。最后引用关闭后才注销并回收；重新插入获得新的
设备身份和动态 sd 名称，不承诺跨拔插的固定名称。打开或挂载的 U 盘阻止
``rmmod ehci``，卸载不得强制绕过引用。

验证入口与依据
==============

内核宿主套件由 ``tools/tests/host-suites.list`` 管理，相关入口包括
``tools/test_usb_storage.py``、``tools/test_usb_ehci.py``、
``tools/test_usb_dma.py``、``tools/test_module_usb_owner.py``、
``tools/test_block_registry.py`` 与 ``tools/test_mbr_partitions.py``。
``tools/tests/usb_input_probe.c`` 是使用 Linux evdev/POSIX 文件操作的来宾探针。
完整镜像与虚拟机编排由操作系统仓库提供，不能用宿主脚本通过代替硬件验证。

协议依据为 `Intel EHCI 1.0
<https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf>`_、
`USB-IF Bulk-Only Transport 1.0
<https://www.usb.org/sites/default/files/usbmassbulk_10.pdf>`_ 和
`USB-IF HID 1.11 <https://www.usb.org/sites/default/files/hid1_11.pdf>`_。
