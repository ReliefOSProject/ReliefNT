========
ReliefNT
========

`简体中文 <README.rst>`_ | `English <README.en.rst>`_ | 日本語

ReliefNT は ReliefOS の独立した x86_64 カーネルで、実装済みの Linux syscall/UAPI
インターフェースに対応します。本リポジトリにはユーザー空間アプリケーション、
APK パッケージ、インストールイメージは含まれません。原文マニュアルの入口は
`Documentation/index.rst <Documentation/index.rst>`_ です。

ビルドには GNU Make 4.3+、Clang/LLVM、固定した Linux Kbuild/Kconfig を使用します::

    make fetch
    make defconfig
    make -j4 all
    make O=out/docs htmldocs

カーネルとデバッグ用の生成物は O/generated/system、HTML マニュアルは
O/Documentation/html に出力されます。マニュアルは簡体字中国語、英語、日本語を
同時に生成します。既存の中国語 URL を既定の入口とし、出力の en/ に英語、
ja/ に日本語を配置します。各ページから他言語の同じページへ切り替えられます。

詳しい要件は `ビルドマニュアル <Documentation/kbuild/building.rst>`_、
既知の制限は `対応範囲 <Documentation/admin-guide/limitations.rst>`_ を参照してください。
VMware での検証は未完了で、物理ハードウェアでは未検証です。
貢献する前に AGENT.md をお読みください。ライセンスは LICENSE と LICENSES/ にあります。
上流のビルドエンジンを変更して規則を回避しないでください。
