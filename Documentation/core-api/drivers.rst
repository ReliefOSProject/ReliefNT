==============
驱动与存储部件
==============

职责
====

drivers/ 按 console、tty、video、storage、net、input、usb、sound 等设备族
组织；sound/core 实现 PCM、mixer 及 ALSA/OSS 路线。block/ 负责块 I/O、GPT 与
设备身份，fs/ 各文件系统负责自己的元数据、缓存和同步。
目录模块静态链接进内核，不是通用 Linux 可加载模块接口。

磁盘与显示
==========

原始块写入遵守打开时 DAC、描述符模式与权限；重读分区表要求 CAP_SYS_ADMIN。
FAT 目录按需有界分配，长文件名按 UTF-16 wire 数据处理，不能使用未对齐主机读取。
默认 root 的 PARTUUID 选择与磁盘编号分开。

framebuffer 提供基本显示；DRM/vmwgfx 实现有界 SVGA-II/VGPU9 能力、资源、
命令验证与 fence，不承诺通用跨设备 DMA-BUF、atomic modesetting 或硬件 vblank。
未得到设备能力时不能伪造 render 节点或成功状态。
