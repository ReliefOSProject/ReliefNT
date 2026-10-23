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
总测试中的已知 ABI golden 差异不得静默更新期望值。

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

``tools/tests/abi_layout_golden.json`` 是当前布局校验的基线；
``tools/tests/fixtures/legacy-abi-layout.json`` 保存旧导出面的原始布局记录，
迁移时逐字保留，两份均不能通过重生成来掩盖布局变化。
音频 ioctl 支持清单为 ``configs/audio-abi-support.json``。
验证范围和标志保持原值，断言与清单的既有分歧必须作为失败报告。


目标运行
========

启动检查固件与内核完成、PID 1；系统调用探针必须实际进入来宾。
panic 验证还需真实异常与画面，徽标需检查数量、留距和移除。
主仓提供集成 QEMU 工具，但它们不是本仓新增产品；每次记录源码与二进制
摘要，保留源镜像。VMware 与物理硬件未验证不能记作通过。
