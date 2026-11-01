========
内核验证
========

定向检查
========

从本仓运行受影响目标；作为父仓子模块时 TMPDIR 使用父仓 out/tmp::

    make O=out/check -j4 all
    make O=out/check test-tools test-header-export test-vdso
    make O=out/check test-boot test-boot-logo test-panic-console test-module-layout

module-layout 检查本地 Kbuild、重复/遗漏对象、生产实现包含和上游引擎摘要。
宿主工具/ASan/UBSan、头导出、自包含 C/C++、ELF 和真实运行是不同层次。
ABI 变化必须明确审查，不通过重生成历史 golden 消除差异。

测试归属
========

``tools/test_*.py``、``tools/tests/`` 与 ``tests/host/`` 保存内核实现、
驱动、文件系统、调度、内存管理和 Linux UAPI 的宿主测试、原生系统调用探针、
夹具及参考头。它们使用本仓源码与头文件，不依赖 ReliefOS 父仓的用户态或
构建目录。固定 syscall 表及其维护生成器也由本仓持有。

独立 checkout 或源码归档中可运行::

    mkdir -p out/tmp
    TMPDIR="$PWD/out/tmp" make O=out/check test-ownership test-host-smoke
    TMPDIR="$PWD/out/tmp" make O=out/check test-host
    python3 tools/test_host.py --list
    python3 tools/test_host.py --suite drm --suite svga --output out/drm-tests

``tools/tests/host-suites.list`` 显式登记宿主套件和必要参数；runner 保留各套件
日志及 ``results.json``，任何失败均返回非零，后续套件仍会执行以形成完整报告。
``test-host-smoke`` 是 CI 中的定向门禁；完整 ``test`` 还包括所有宿主套件、
ABI golden、导出头、启动、vDSO、徽标、panic、Kconfig 和模块布局检查。
仅有 Linux 参考探针通过，不表示相同接口已在 ReliefNT 来宾运行通过。

``service_roles`` 套件检查旧内核桌面实现未恢复、旧窗口服务权限标志未恢复，
以及 exec 只保留允许的任务标志；这些内核源码检查由本仓持有，父仓不再重复。

``syscall_io`` 套件覆盖真实设备节点标志组合下的 ``/dev/null`` 写入、访问模式、
``O_PATH`` 和 Linux ``MAX_RW_COUNT`` 上限，避免通用设备分片截短空设备写入。
其原生探针 ``tools/tests/syscall_io_runtime_probe.c`` 编译后可用 ``--null``
单独检查大块写入、原始 write syscall、dup 和 writev；默认模式继续运行综合
POSIX I/O 检查。宿主 Linux 参考运行与 ReliefNT 来宾执行分别记录。

``test-smp-time``、``test-page-cache-growth``、``test-user-return``、
``test-syscall-io``、``test-vdso-host`` 和 ``test-version`` 提供定向入口。
``make test-ext4-reference LINUX_REFERENCE=<目录>`` 检查 ext4 schema，
需要已有 Linux 参考树；
它不会下载参考源码。互操作检查需要宿主 ``e2fsprogs``。
音频探针 ``--build-guest`` 需要显式传入由操作系统提供的 ``--sdk``。

兼容性基线
==========

``tools/tests/abi_layout_golden.json`` 保留原始布局基线；
``tools/tests/fixtures/legacy-abi-layout.json`` 保存旧导出面的原始布局记录，
迁移时逐字保留，两份均不能通过重生成来掩盖布局变化。
驱动服务表版本 2 的 17 个追加字段在
``tools/tests/fixtures/driver-api-v2.json`` 单独冻结：168 字节旧前缀的类型与
偏移仍与原始基线比较，当前服务表总长为 304 字节、对齐为 8。
``make test-abi`` 同时检查旧前缀与追加字段，版本、大小、偏移和类型变化
均需显式审查。``abi_layout_checks`` 套件包含对应的失败反例；驱动管理器的
``driver_modules`` 套件验证旧版本、未知版本和错误描述符大小在 init 前被拒绝。
这不表示版本 1 驱动二进制能在版本 2 内核运行。

音频 ioctl 支持清单为 ``configs/audio-abi-support.json``。
宿主门禁检查标志类型、构建前提、证据路径和验证范围；来宾、上游 ALSA 与
硬件声明必须有对应说明，缺少证据声明的反例应失败。清单保留已有的验证标志
和未解决项；这些静态检查不重新执行历史来宾测试，不构成新的运行验收。
``linux_audio`` 的 ASan/UBSan 探针遇到未定义行为立即失败，不将带有
sanitizer 错误的零退出状态计为通过；PCM 回归覆盖尚无 DMA 缓冲区时的 DROP。


目标运行
========

启动检查固件与内核完成、PID 1；系统调用探针必须实际进入来宾。
panic 验证还需真实异常与画面，徽标需检查数量、留距和移除。
主仓提供集成 QEMU 工具，但它们不是本仓新增产品；每次记录源码与二进制
摘要，保留源镜像。VMware 与物理硬件未验证不能记作通过。
