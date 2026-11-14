========
ReliefNT
========

简体中文 | `English <README.en.rst>`_ | `日本語 <README.ja.rst>`_

ReliefNT 是 ReliefOS 的独立 x86_64 内核，支持已实现的 Linux syscall/UAPI。
不包含用户态应用、APK 或安装镜像。手册入口为
`Documentation/index.rst <Documentation/index.rst>`_。

构建使用 GNU Make 4.3+、Clang/LLVM 和固定的 Linux Kbuild/Kconfig：

::

    make fetch
    make defconfig
    make -j4 all
    make O=out/docs htmldocs

内核与调试载荷在 O/generated/system，HTML 手册在 O/Documentation/html。
HTML 手册同时生成简体中文、英文和日文；默认中文入口保留原地址，英文位于
输出的 en/，日文位于 ja/，每页可切换到另一语言的同一页面。
详细条件见 `构建手册 <Documentation/kbuild/building.rst>`_。
已知限制见 `支持范围 <Documentation/admin-guide/limitations.rst>`_。
VMware 验证未完成；物理硬件未验证。
贡献前阅读 AGENT.md，许可见 LICENSE 与 LICENSES/；不修改上游引擎绕过规则。
