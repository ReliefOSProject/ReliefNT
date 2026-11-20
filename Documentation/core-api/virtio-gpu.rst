=====================
VirtIO-GPU 与 DRM/KMS
=====================

设备与构建
==========

``drivers/virtio/`` 实现 VirtIO 1.x 现代 PCI transport 与 split virtqueue；
``drivers/video/drm/virtio/`` 实现 PCI ``1af4:1050`` 的 controlq、cursorq、
2D 资源、VirGL 上下文及 DRM/KMS。适用于 QEMU 的 ``virtio-vga``、
``virtio-gpu-pci`` 及其 GL 变体；不实现 legacy-only、MMIO 或 packed ring。

``VIRTIO_PCI``、``DRM`` 和 ``DRM_VIRTIO_GPU`` 在 defconfig 中默认启用。
``DRM=m`` 时，VirtIO-GPU 后端随 DRM 核心编入 ``drm.ko``，PCI transport 保持内置。
使用匹配构建的 ``modules_install`` 结果加载，不兼容 Linux 二进制模块。

用户态接口
==========

主设备为 ``/dev/dri/card0``，使用标准 major 226。只在 host 协商了 VirGL
且返回受支持 capset 时提供 ``/dev/dri/renderD128``。GETPARAM、GET_CAPS、
RESOURCE_CREATE、MAP、RESOURCE_INFO、TRANSFER、EXECBUFFER、WAIT
使用 Linux VirtGPU ioctl；每个 DRM 文件具有独立 GEM handle 与 VirGL 上下文。

KMS 提供连接器、EDID、模式、CRTC、线性 XRGB8888/ARGB8888 framebuffer、
DIRTYFB、legacy modeset/page flip、DPMS、primary plane 及 64×64 硬件光标。
连接器数量来自设备，最多 16 个；实际模式和连接状态来自显示信息，
``/sys/class/drm/card0-Virtual-N/`` 发布对应 status、enabled、modes 和 EDID。
翻页事件在 host fence 完成后发送，时间戳使用 CLOCK_MONOTONIC；这不是物理
vblank 同步保证，不报告硬件 vblank 或 async page flip 能力。

普通 2D framebuffer 的 DIRTYFB 先校验整批区域。只有 2 至 4 个互不重叠的
矩形，且每多提交一块能节省至少 256 KiB 的像素传输时，才逐块上传；其他
情况提交一个包围矩形。VirGL 仍使用原有的合并刷新路径。

同设备 PRIME 导入导出使用 canonical DMA-BUF OFD，支持 mmap 和
DMA_BUF_IOCTL_SYNC。EXECBUFFER 支持输入和输出 sync_file fence；
poll、SYNC_IOC_FILE_INFO、SYNC_IOC_MERGE 和 deadline hint 使用 Linux UAPI。
跨设备 DMA-BUF、atomic KMS、syncobj、resource blob、host-visible mapping、
context-init、Venus/Vulkan 与视频编解码加速尚未实现，能力查询不宣称支持。

生命周期与错误
==============

每条命令使用独立 coherent DMA、唯一 fence 和经校验的回复长度。IRQ 只消费
used ring 和发布完成状态，资源回收在任务上下文执行。描述符支持乱序完成及
16 位环回绕；MSI-X 不可用时尝试 MSI，最终保留有界轮询。用户指针在提交
之前复制，BO、映射、PRIME、sync_file 和在途请求保留实际 provider owner。

关闭文件、释放 master、切换 VT 或卸载不能提前释放 host 正在使用的页面。
请求超时先 reset，设备状态确认归零后才能释放 DMA；reset 失败保留页面与
模块引用。退出 Xorg 时将 scanout 重新绑定独立的文本控制台资源。

处于可见文本 VT 时，控制台只复制和上传已绘制的矩形（包括光标），传输
偏移按该矩形在 2D backing 中的首像素计算。失败的刷新保留区域供设备仍
可用时重试；从图形 VT 恢复文本扫描输出时重新上传整屏。

panic 通道在初始化时预留命令 DMA 和队列描述符，只供停止其他 CPU 后的
fatal owner 使用，不获取普通 GPU 锁或执行分配。GUI panic 只上传已绘制文字
所在矩形，保留其他桌面像素；TTY 使用保留的控制台资源。设备不响应时放弃
该显示通道，串口诊断仍继续。panic 后不恢复普通队列执行。

验证与上游来源
==============

``tools/test_virtio.py`` 在 ASan/UBSan 下运行生产 ring 实现，覆盖乱序、
131080 次环回绕、背压、非法 used entry 及 fatal 预留链。原生用户态探针
``tools/tests/virtgpu_guest.c`` 可用 musl 编译，执行实际 DRM、PRIME、
sync_file、sysfs、翻页和光标操作；用 ``--no-pause`` 跳过截图握手。
``tools/test_drm.py`` 覆盖局部文本上传的偏移、失败重试、VT 整屏恢复，
以及 2D DIRTYFB 的区域拆分、合并和校验。
完整系统、Mesa/EGL、Xorg、BIOS/UEFI 及模块重载的 QEMU 编排由 ReliefOS 持有。
离屏 draw 速率不能替代桌面帧率或实体显卡性能。VMware 验证未完成。

协议参考 OASIS VirtIO 1.3 第 4.1、2.7 和 5.7 节。VirtGPU、VirtIO-GPU
与 sync_file UAPI 原样取自 Linux v6.12，commit
``adc218676eef25575469234709c2d87185ca223a``。实现代码独立编写，
采用 Apache-2.0；头文件保留 BSD、MIT 或带 Linux-syscall-note 的原有许可，
具体版权与许可文本见仓库 NOTICE，不导入 Linux DRM 实现。
