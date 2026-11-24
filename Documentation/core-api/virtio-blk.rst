============
VirtIO-Block
============

设备与构建
==========

``drivers/storage/virtio_blk/`` 匹配 PCI ``1af4:1042``，也匹配具有现代
PCI capability 的过渡设备 ``1af4:1001``。两者均需协商 ``VIRTIO_F_VERSION_1``。
仅暴露旧式寄存器的设备会被跳过，不影响其他控制器上的根分区。
不支持 VirtIO MMIO、VirtIO SCSI 或 VMware PVSCSI。

``CONFIG_VIRTIO_BLK=y`` 是 defconfig 的默认值；选择 ``m`` 可构建
``virtio_blk.ko``。PCI transport ``CONFIG_VIRTIO_PCI`` 保持内置。若驱动
用于访问根分区，模块及依赖须在 initramfs 中提前加载。已经打开或挂载的
块设备持有实际模块引用，阻止其卸载。

块设备接口
==========

每个磁盘有一个 split virtqueue，支持读取、写入、只读设备和缓存刷新。
优先使用 MSI-X，其次 MSI；无法建立中断时可轮询完成。协商的逻辑块大小
为 512 至 4096 字节（2 的幂），未提供块大小特性时为 512 字节。
请求中的 ``sector`` 始终以 512 字节为单位；驱动遵守协商的段大小限制，
每条请求至多传输 128 KiB。

磁盘节点为 ``/dev/vda``、``/dev/vdb`` 等，分区名例如 ``/dev/vda1``。
设备允许常规文件描述符 I/O，只读盘拒绝写入。对已协商缓存刷新的盘，
``fsync()`` 通过 ``VIRTIO_BLK_T_FLUSH`` 同步设备的写缓存，并返回设备错误。
``BLKSSZGET`` 和 ``queue/logical_block_size`` 报告逻辑块的字节大小；
``BLKGETSIZE64`` 报告字节容量，``BLKGETSIZE`` 以及 sysfs 分区的
``start`` / ``size`` 以 512 字节为单位。GPT/MBR 按实际逻辑块解析。

完成与故障
==========

队列、请求头、状态和数据缓冲由驱动持有。读请求可以在允许重试的调用中
等待完成，不保留调用方缓冲区；写入和刷新不自动重放。发现无法确认的完成
或遇到超时后，驱动停止接收请求并复位设备；需要等到设备确认停止、IRQ
退出后才能释放 DMA。若复位失败，则保留相关资源，避免设备向已释放的
内存写入。

当前不提供在线扩容、热插拔、多队列、packed ring、discard/TRIM、
write-zeroes、分区格式化器的 4Kn 支持或 VirtIO-Block swap。
块设备几何变化时，旧实例停止 I/O，不把变化后的容量作为原设备继续使用。

验证与参考
==========

``tools/test_virtio_blk.py`` 用生产 split ring 覆盖请求格式、只读、短完成、
取消、计时回退、超时以及复位失败时的资源保留。QEMU/KVM 验证包括内置驱动
的原始块读写、刷新、过渡 PCI 设备、无 MSI-X 的只读设备和 4Kn GPT/ext4，
以及 initramfs 从模块发现并
切换到 4Kn ext4 根。VMware 验证未完成。

参考 `OASIS VirtIO 1.3 <https://docs.oasis-open.org/virtio/virtio/v1.3/virtio-v1.3.html>`_
第 2.7、4.1、5.2 节与 Linux 块设备 UAPI。实现代码采用 Apache-2.0。
