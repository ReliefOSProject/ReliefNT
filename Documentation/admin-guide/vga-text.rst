===================
VGA Text Mode 后端
===================

``console=vga`` 显式请求字符平面控制台，默认不启用。
``console=framebuffer`` 可覆盖之前的 VGA 请求；匹配完整命令行 token。
只有硬件探测和模式切换均成功才发布 ``console_vga_text_mode``。
不支持的设备维持原后端，打印可诊断错误。

实现边界
========

``drivers/console/vga_terminal.c`` 和 ``vga_ansi.c`` 只维护固定大小的字符状态，
不访问硬件、不分配内存。每个 VT 有独立主屏、备用屏、UTF-8 与转义解析器。
输出通过原有执行事务串行化；启动输出在 BSP 上、内核日志输出在原日志序列化
规则内处理。进入用户态后内核诊断继续写日志和串口，不覆盖用户屏幕。

``drivers/video/vga/text_mode.c`` 独占兼容后端的文本显存、字体平面、VGA/DAC
寄存器和硬件光标访问。图形恢复调用现有后端模式设置接口，不修改 framebuffer
TTY 的渲染器与 ANSI 解析器。每次恢复使用当前 framebuffer 描述符，允许用户态
在 KD_GRAPHICS 之前先设置模式；下一个文本呈现会收回被提前模式设置改变的扫描输出。

Linux VT 控制流程在提交所有权之前调用可失败的 VGA 硬件准备函数。失败保持
原 VT/KD 状态；默认 framebuffer 路线不进入这个分支。固定 VT 的 winsize 为
80×25，普通 Unix98 PTY 的尺寸规则不变。终端查询通过 ``pty_vt_reply`` 直接
进入对应 VT 输入环，不走规范模式、信号转换或回显。

设备和降级
==========

支持兼容 VGA 的 Bochs VBE、VMware SVGA II 图形恢复，以及无 framebuffer
的 Legacy VGA 纯文本启动。纯 GOP 且无兼容平面的设备不进入该模式。
SVGA 转换使用现有设备停机与恢复事务；活跃模式租约导致拒绝转换，不能强制
释放其他 owner 的资源。panic 在停止其他 CPU 后通过静态屏幕和有界端口写入
强制取回文字扫描输出，不获取常规设备锁或分配内存。

固定 PSF 字库提供拉丁字符及部分线框；其他 Unicode 字符降级为 ``?``。
颜色量化为 16 色，字体样式、中文/emoji 字库、图片、任意像素、可变字号、
OSC 剪贴板/标题/超链接均不模拟。GUI 会话保留原有图形能力。

验证入口为 ``tools/test_vga_text.py``、``tools/test_console.py``、
``tools/test_linux_pty.py`` 和 ``tools/test_xorg_vt_handoff.py``。
硬件及完整 OS 验收由父仓的专用虚拟磁盘执行，不能从寄存器夹具通过推断为
真实 VMware 已验证。
