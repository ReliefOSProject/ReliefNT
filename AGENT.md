# ReliefNT Agent Guide

本文件适用于整个 ReliefNT 仓库。用户明确指令优先；先检查真实源码、工作树、
构建图和差异，再修改和验证。保留用户已有修改，不重置、覆盖或结束用户进程。

## 构建与验证

- 根 Makefile 是生产入口；产物为 `kernel.sys`、`kernel.debug` 和 `kerneldebug.sys`。
  本仓不包含 ReliefOS 用户态、系统镜像或 APK 打包。
- C/汇编、宿主工具、依赖追踪和配置由 `scripts/kbuild-source.json` 中校验过的
  Linux 6.12 Kbuild/Kconfig 文件提供；适配只放在 `Kbuild`、`scripts/Makefile.reliefnt`
  和 `scripts/Makefile.port`。不得改写导入的 `scripts/Makefile.*`、`basic/`、
  `kconfig/`、`include/` 或许可证文件来绕过上游规则。
- 每个自研代码目录用自己的 `Kbuild` 显式列出本目录对象和直接子目录；根
  `Kbuild` 只遍历子系统并集成顶层产物。不得恢复全局源码清单、通配扫描或
  通过包含实现 `.c`/`.inc` 文件代替独立编译。目录与迁移对应见
  [模块说明](docs/KERNEL_MODULES.md) 和历史源码映射；审计清单不参与构建。
- `.config` 只承载内核实际消费的 Kconfig 符号；镜像/产品配置留在 ReliefOS 父仓。
  `PROFILE`、`EXTRAVERSION`、`LOCALVERSION` 和工具链覆盖仍由独立入口提供。
- `configs/build-version` 是版本输入，生成文件只写输出目录。更改版本时同步
  当前文档示例，保留数字 ABI 语义与最长 31 字节的完整内核版本限制。
- 按改动范围执行内核编译、宿主工具测试、UAPI 布局和头文件导出检查。
  不改 ABI golden 或音频 manifest 来掩盖失败；记录并报告真实结果。
- 编译、打包、QEMU、VMware 和硬件验证分别报告；未运行不是通过。
  VMware 验证未完成时必须明示。作为父仓子模块工作时，TMPDIR 使用父仓 `out/tmp`。
- 仅提交任务文件；不提交 `out/`、`build/`、缓存、密钥和无关产物。
  源码修改后运行 `git diff --check`。

## 版本发布的长期规则

- 发布前必须阅读并遵循 [发布规范](docs/RELEASE_PROCESS.md)。Release 更新范围是
  `(上一版本号提交, 本次版本号提交]`，不是发布日期之间或任意最近 tag 之间。
  例如 5.1.0 从“改为 5.0.0”的提交之后统计到“改为 5.1.0”的提交。
- ReliefNT 仅写内核仓自身的有效更新；ReliefOS 仅写主仓自身更新，不包含内核
  实现和纯 gitlink 更新。两份说明章节格式相同、内容独立，合并去重，不把已撤销
  功能或计划当交付，不列“更新版本号到 x.x.x”等机械条目。
- 说明必须包括：更新内容、是否破坏性更新、更新与迁移注意事项、开发者提示；
  归档到 `docs/releases/<版本>.md`。
- 只有用户明确授权才提交、推送、建立分支或发布；detached HEAD 是正常的
  子模块状态，不因此创建分支，不强推、不擅自重写标签或 Release。
- 先推送内核版本提交，再让 ReliefOS 父仓更新 gitlink 并推送。两仓标签和
  Release 各自固定到精确版本提交；使用 `gh` 与审阅后的正文文件发布并回读核对。
- 预发布必须标记 prerelease。源码发布只提供源码；二进制仅在明确要求时从本版
  重建并附校验和和归属文件，不上传旧工作区产物。
