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
简体中文是源语言及默认入口，英文位于输出的 en/，日文位于 ja/；每页的
语言导航切换到另一语言的同名页面。既有中文地址保持不变。

翻译维护
========

英文和日文 PO 按页面完整路径存放在
``Documentation/locale/{en,ja}/LC_MESSAGES/<docname>.po``，例如
``Documentation/locale/ja/LC_MESSAGES/process/documentation.po``。
配置使用 ``gettext_compact = False``，每个页面独立提取消息。
HTML 构建复制 locale 到临时输出后编译 MO，不修改源码中的翻译目录。

本仓独立提供以下入口，只运行文档工具，不编译内核::

    make O=out/docs gettext
    make O=out/docs gettext-check
    python3 scripts/update-doc-translations.py --output out/docs-gettext
    python3 scripts/update-doc-translations.py --update --output out/docs-gettext
    python3 scripts/update-doc-translations.py --check --output out/docs-gettext

Make 目标将 POT 输出到 ``O/Documentation/gettext/``。脚本默认只提取模板，
显式 ``--update`` 才初始化或合并英文、日文 PO，保留译文与译者注释，近似匹配
标记为 fuzzy。维护工具使用 Sphinx 已有的 Babel，无需新增依赖。
``--check`` 按当前源码重新提取模板，只读检查全部 PO；缺页、缺消息、缺译、
fuzzy、过期消息及含中文原文整段照抄均失败，不能以中文回退充当译文。
每次提取替换旧 POT，已删除页面不会遗留模板。

纯英文命令和路径允许与原文相同。需要保留原文的中文技术词或日文同形词，
在本仓 ``Documentation/locale/identical.json`` 按语言登记完整 msgid，例如::

    {"en": [], "ja": ["注意", "内容"]}

白名单必须恰好包含 ``en``、``ja`` 两个键，值为不重复的非空字符串数组。
按完整 msgid 精确匹配，不支持模糊匹配或通配；日文例外不用于英文。
``gettext-check`` 与 ``htmldocs`` 预检默认读取同一文件，删除例外后同形译文
会再次检查失败。``--allow-identical '术语'`` 仅为本次 CLI 检查临时添加两种语言
共用的例外，不修改持久白名单。行号与提取时间变化不算译文过期。

``htmldocs``、``gettext``、``gettext-check`` 通过 ``scripts/docs-python.sh``
共用解释器选择：显式非空 ``DOCS_PYTHON`` 优先，否则依次选择 ``SPHINXBUILD``
所在目录的 ``python``、``python3``，均不存在时才使用 PATH 中的 ``python3``。
只设置 ``SPHINXBUILD=/path/to/venv/bin/sphinx-build`` 即可使用该虚拟环境的
Babel；可用 ``DOCS_PYTHON=/path/to/python`` 显式覆盖。直接运行 Python 维护
脚本时，仍需激活相同环境或显式选择该环境的 Python。

格式参考 `Linux 文档指南 <https://docs.kernel.org/doc-guide/sphinx.html>`_。
完整测试与平台范围见 :doc:`testing` 及 :doc:`../admin-guide/limitations`。
