========
文档编写
========

现行手册位于 Documentation/，使用 UTF-8 RST、显式 toctree 与 Sphinx。
文档标题等号上下线，章用等号、节用横线、子节用波浪线。保持纯文本可读，
代码使用字面块，交叉引用使用 doc/ref。按管理员、构建、内部 API、用户 API
和开发流程组织，不把聊天、阶段日志和历史计划混入操作手册。

根 README.rst 是入口，AGENT 与许可文件按职责保留。公开声明以导出的 UAPI
和真实实现为依据，不为文档生成改变代码语义。
内核手册只在本仓维护，ReliefOS 的桌面、应用和系统集成手册由主仓维护。
ReliefOS 的 Pages/RPR 网站读取本仓文档生成独立的 docs/reliefnt/ 入口；
不用在主仓保存另一份内核源文档。

使用 Sphinx 5.3 至 9.x，在本仓运行 ``make O=out/docs htmldocs``；
输出为 O/Documentation/html。构建将警告作为错误，必须解决未引用页面、语法
和失效交叉引用。HTML 使用 Alabaster，可离线阅读，不加载 JavaScript。

格式参考 `Linux 文档指南 <https://docs.kernel.org/doc-guide/sphinx.html>`_。
完整测试与平台范围见 :doc:`testing` 及 :doc:`../admin-guide/limitations`。
