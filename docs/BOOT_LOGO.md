# 内核 framebuffer 启动 Logo

内核启动时在 framebuffer 顶部绘制 ReliefOS 蓝色标志。默认每个标志为
80 × 80 像素，外边距和标志间隔均为 16 像素。启动日志在其下方正常滚动。
一行放不下时换行；核心较多或屏幕较小时依次缩小至 40、20、16 像素，
缩小时的间隔为标志尺寸的五分之一、至少 4 像素。始终保留至少四行文本，
极小屏幕无法容纳时跳过或裁减可见标志数量，不越过 framebuffer 边界。

数量采用 ACPI 拓扑发现的逻辑 CPU 数，由 `smp_cpu_count()` 提供。ReliefNT
在初始化末尾才启动 AP，不能在早期用在线 CPU 数代替发现数量；否则整个
启动过程几乎只会显示 BSP 的一个标志。这不是物理插槽数或关闭 SMT 后的物理核心数。

`boot complete` 日志写出后，BSP 调用 `console_boot_complete()`，清除标志、
重新布局并重放启动日志，然后切入 tty1 和用户态。后续 VT 切换、控制台重新
启用不会重新显示标志。没有 framebuffer 时保持原有 VGA/串口路径。

## 图像与构建

`drivers/video/framebuffer/boot_logo_data.h` 保存 6,400 字节索引像素和 128 色
RGB 调色板，共 6,912 字节只读图像数据，直接链接进 `kernel.sys`。来源为
ReliefOS 父仓根目录 `logo.png`，原图 SHA-256：

```text
eade7b94f444fd576bea2d412c27fdfc8c4cb27f1c9b08b758d1f965158bdfac
```

派生过程为裁出 alpha 可见边界、保持纵横比缩放并居中放入 80 × 80 方块、
以黑色合成 alpha、量化为 128 色。内核不读取外部图像、访问文件系统、解压
PNG 或为 Logo 分配堆内存。`boot_logo.o` 在本仓原生 Kbuild C 对象列表中登记，
图像头文件变化由 fixdep 追踪；原版 Kbuild 引擎未修改。

绘制复用 `framebuffer_rect_to()`，由现有实现处理 pitch、24/32 位像素和 RGB
位域；没有增加用户态接口或修改 Linux fbdev/VT ABI。布局概念参考
[Linux 6.12 fb_logo.c](https://github.com/torvalds/linux/blob/v6.12/drivers/video/fbdev/core/fb_logo.c)，
Linux 默认按在线 CPU 数绘制，默认标志间隔为 8 像素；本实现按上述 ReliefNT
启动顺序与布局约定执行。

## 验证

在父仓构建目录执行（`TMPDIR` 位于父仓 `out/tmp`）：

```sh
make O=out/kbuild-verified -j6 kernel
make -C kernel/reliefnt O="$PWD/out/boot-logo/kernel-test" test-boot-logo
make -C kernel/reliefnt O="$PWD/out/boot-logo/host-sanitised" \
  HOST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  HOST_LDFLAGS='-fsanitize=address,undefined' test-boot-logo
python3 tools/test_boot_logo_qemu.py --build-dir out/kbuild-verified \
  --output out/bl4a --cpus 4
```

宿主测试用真实 Logo 与 framebuffer 绘制代码操作带边界哨兵和 pitch 填充的
内存表面；覆盖 1、4、8、64 CPU、换行与缩小、24/32 位 RGB/BGR、无效表面
和保留文本区域。普通与 ASan/UBSan 测试各通过 9,696,607 项像素/边界检查。

QEMU 工具只克隆已有 RAW，向私有 ESP 注入本次内核并回读 SHA-256；工具执行
期间不改源镜像。硬件断点在早期图形初始化和 `console_boot_complete()` 前后抓取
真实画面，验证 Logo 数量、16 像素间隔、持续保留、自动清除和首行文本恢复。
断点仅用于观测，没有在生产启动流程增加停顿。

独立启动观察中仍出现既有的 AP 启动超时提示，随后在线数恢复为全部 CPU。
先前 ReliefPlayer/ReliefCapture 验收串口日志已有同样现象；本次保留其原日志，
没有修改 SMP 时序或把该项报告为已修复。

已通过原生 `image-vmdk` 更新 `out/kbuild-verified/images/reliefos.vmdk`，
`qemu-img compare` 确认其内容与 RAW 相同。ESP 内核回读 SHA-256 与编译产物、
QEMU 抓图时实际运行的内核均相同：

```text
c476b989b3d030e8232ce97de5c96398b157487d543f25499e61bd435af45f9d
```

真实截图位于父仓 `out/bl4a/boot-logos.png`（早期）、
`out/bl4a/boot-before-hide.png`（启动末尾）和
`out/bl4a/boot-complete.png`（清除后）。`out/bl1/` 另有单 CPU 显示/清除验证；
最终四核验收同时覆盖三个拍摄阶段，独立代码审查未发现需要修复的问题。

ISO 未重新打包。VMware 验证未完成，物理硬件未验证。
