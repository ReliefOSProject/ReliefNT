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

目标运行
========

启动检查固件与内核完成、PID 1；系统调用探针必须实际进入来宾。
panic 验证还需真实异常与画面，徽标需检查数量、留距和移除。
主仓提供集成 QEMU 工具，但它们不是本仓新增产品；每次记录源码与二进制
摘要，保留源镜像。VMware 与物理硬件未验证不能记作通过。
