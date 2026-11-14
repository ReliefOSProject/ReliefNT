========
ReliefNT
========

`简体中文 <README.rst>`_ | English | `日本語 <README.ja.rst>`_

ReliefNT is the standalone x86_64 kernel of ReliefOS and supports the Linux
syscall/UAPI interfaces it implements. This repository does not include
userspace applications, APK packages or installation images. The source manual
starts at `Documentation/index.rst <Documentation/index.rst>`_.

The build uses GNU Make 4.3+, Clang/LLVM and a pinned Linux Kbuild/Kconfig::

    make fetch
    make defconfig
    make -j4 all
    make O=out/docs htmldocs

Kernel and debug payloads are under O/generated/system; HTML manuals are under
O/Documentation/html. Manuals are generated in Simplified Chinese, English
and Japanese. Existing Chinese URLs remain the default; English is under en/
and Japanese under ja/ in the output. Every page links to the same page in the
other languages.

See the `build manual <Documentation/kbuild/building.rst>`_ for detailed
requirements and the `supported scope <Documentation/admin-guide/limitations.rst>`_
for known limitations. VMware validation is incomplete; physical hardware has
not been validated.
Read AGENT.md before contributing. Licenses are in LICENSE and LICENSES/;
do not modify the upstream build engine to bypass its rules.
