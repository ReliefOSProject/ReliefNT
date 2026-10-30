=======================
USB 存储与基本输入
=======================

配置与范围
==========

``CONFIG_USB_EHCI_HCD`` 选择 PCI EHCI 主机驱动，``y`` 内置，``m`` 生成
``drivers/usb/ehci/ehci.ko``。defconfig 默认内置。USB 的通用注册、延迟维护、
boot HID 解码和 BOT/SCSI 协议代码内置；控制器、设备绑定与 DMA 属于 HCD
owner，不产生无法卸载的自引用。

``CONFIG_USB_XHCI_HCD`` 选择 PCI xHCI 主机驱动，``y`` 内置、``m`` 生成
``drivers/usb/xhci/xhci.ko``，defconfig 默认内置。xHCI 支持直接连接根端口的
SuperSpeed/SuperSpeedPlus BOT 存储，以及 USB 2.0/1.x BOT 存储和 boot HID
键盘/相对鼠标。xHCI 根据控制器的 Supported Protocol 能力和端口报告的原始
Protocol Speed ID 选择速度，不能由控制器存在推断设备速度。``speed=SUPER``
对应支持的 5 Gbit/s SuperSpeed；``speed=SUPER_PLUS`` 对应受支持的 USB 3.1
和 3.2 SuperSpeedPlus 协议速率。两者使用相同的 512 字节 EP0 和已支持的
SuperSpeed companion、bulk 和 interrupt 端点规则。

显式 PSI 要求已支持的 USB 协议版本、对称全双工链路及有效 LP/速率组合：
USB 3.1 的 LP=1 接受 10 Gbit/s，USB 3.2 的 LP=1 接受 10/20 Gbit/s；其他
速率和保留组合不接入。PSIC=0 时仅按 xHCI 1.2c 表 7-13 和 7.2.2.1.2 的
版本默认值映射：USB 3.0 ID 4，USB 3.1 ID 4/5，USB 3.2 ID 4/5/6/7。
按规范忽略 PSI 中的 RsvdP 字段，同时保留原始 dword。非对称 Rx/Tx PSI 成对
检查但不接入；未知版本和未支持的速率不按 ID 猜测。

设备日志的 ``psi`` 保留端口报告的原始 ID，``bps`` 是控制器提供的 PSI
速率或规范默认值，``psi_raw`` 保留显式 PSI 原始 dword；``psi_source=implied``
表示没有实际 PSI dword，此时 ``psi_raw=00000000``。USB 3.2 默认 ID 6/7
对应的等效 PSI 分别为 10/20 Gbit/s（表 7-13 的单通道描述分别为 5/10
Gbit/s）。这些值是协议能力声明，不是实测吞吐量；驱动不计算通道乘积。
目前未完成 10/20 Gbit/s 物理链路验证。

高速设备由 EHCI 处理。低速/全速设备直接接在 EHCI 根端口时，交给 companion
控制器；VMware USB 2.0 配置应同时启用 ``CONFIG_USB_UHCI_HCD``。USB 2.0 Hub
后的低速/全速设备使用 transaction translator。USB 端口归 xHCI 控制器所有
时不能使用 EHCI 驱动。xHCI 当前不支持 Hub 路由；不支持的 Hub、类或端点被
明确拒绝，不影响控制器继续枚举其他根端口。这项功能不包含 OHCI、UAS、
streams、等时传输或任意 HID report descriptor 解释器，也不涵盖 USB4、Type-C
供电/角色切换或 USB 电气认证。

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

xHCI 使用独立的有界控制器映射，不通过破坏性 BAR 大小探测访问正在运行的
固件控制器。交接前以规范强制的 Operational、根端口、所有 doorbell 和所有
interrupter 寄存器范围，按 PCI BAR 大小规则推导最小 BAR 长度。读取保留位时
按规范屏蔽 DBOFF 和 RTSOFF 的保留位。只有最小范围以内且不重叠活动寄存器的
扩展能力可供访问；映射页大小、xECP、链中偏移或记录长度均不能扩大该下界。
推导依赖控制器如实提供符合规范的 CAPLENGTH、MaxPorts、MaxSlots、MaxIntrs、
DBOFF 和 RTSOFF；这些字段不能独立证明故障或恶意控制器的实际 BAR 大小。
若扩展能力位于保守下界之外，目前会安全拒绝该控制器，即使实际 BAR 可能更大。
交接后停止控制器并关闭 bus mastering，才探测实际 BAR 并验证全部强制寄存器和
使用的扩展能力范围。扩展能力链和映射有上限；BAR 上限当前为 16 MiB。
扩展能力记录还必须避开已知 Operational、根端口、doorbell 和 runtime 中断
寄存器范围；越界或重叠时不会执行 legacy 寄存器写入。

xHCI 按控制器的 AC64 能力限制 DMA 地址，支持 32/64 字节 context、4 KiB 控制器
页面及至多 512 个 scratchpad；不支持的页面大小或更多 scratchpad 会被拒绝。
在停止状态下初始化并发布 DMA 后，先启用并确认 bus mastering，然后设置事件环
大小和 dequeue pointer，最后写入会启动事件环状态机的 ERSTBA，之后再运行控制器。
传输和事件环、context、scratchpad 和 bounce buffer 均由 HCD 拥有。事件必须与
在途命令或传输的 slot、端点及 TRB 地址匹配；短控制输入还须等待状态阶段。
无法确认的完成、停止或恢复会保留可能仍被硬件访问的 DMA，而不是将其重用。
枚举、设备回收和恢复在维护任务运行；输入报告完成由无等待的轮询路径处理。
拔出后的块设备若仍被旧文件引用，维护任务会按有界轮询间隔重试回收；主机
控制器错误会释放 HID 按键/按钮并撤销块设备 I/O，但仍保留可能由硬件访问的
DMA，直到停止和关闭 bus mastering 得到确认。
明确不支持的根端口设备在当前连接期间只枚举一次；连接状态变化后允许重新
枚举。非终止故障最多尝试三次，两次重试分别等待 25、50 个内核 tick；耗尽后
记录错误并等待连接状态变化。重置完成事件本身不会重新开启重试预算。

下列调度和 Hub 细节适用于 EHCI：

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
``rmmod ehci`` 或 ``rmmod xhci``，卸载不得强制绕过引用。

验证入口与依据
==============

内核宿主套件由 ``tools/tests/host-suites.list`` 管理，相关入口包括
``tools/test_usb_storage.py``、``tools/test_usb_ehci.py``、``tools/test_usb_xhci.py``、
``tools/test_usb_dma.py``、``tools/test_module_usb_owner.py``、
``tools/test_block_registry.py`` 与 ``tools/test_mbr_partitions.py``。
``tools/tests/usb_input_probe.c`` 是使用 Linux evdev/POSIX 文件操作的来宾探针。
完整镜像与虚拟机编排由操作系统仓库提供，不能用宿主脚本通过代替硬件验证。

协议依据为 `Intel xHCI 1.2c
<https://cdrdv2.intel.com/v1/dl/getContent/868295>`_、`Intel EHCI 1.0
<https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf>`_、
`USB-IF Bulk-Only Transport 1.0
<https://www.usb.org/sites/default/files/usbmassbulk_10.pdf>`_ 和
`USB-IF HID 1.11 <https://www.usb.org/sites/default/files/hid1_11.pdf>`_。
