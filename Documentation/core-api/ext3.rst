=================
ext3 文件系统
=================

范围与配置
==========

``CONFIG_EXT3_FS`` 默认内置，选择 ``m`` 可生成 ``fs/ext3/ext3.ko``。ext3 前端
通过 ``CONFIG_EXT_FS_COMMON`` 选择共享的 ext 引擎；若其他 ext 前端已内置，
共享引擎也保持内置。模块使用 ``fs-ext3`` 别名，挂载和在途操作持有前端与
共享引擎的所有权。

ext3 使用经典直接、间接块映射和内部日志。识别要求 ``HAS_JOURNAL`` 及兼容的
经典 ext 特性；带 ext4 特性的卷不会被显式的 ext3 挂载当作 ext3。外部日志
和未支持的磁盘特性不在此范围内。日志损坏或恢复失败时，卷不能继续以可写
状态使用。

使用
====

以下命令使用未挂载的目标分区。ReliefOS 的用户环境提供 e2fsprogs 的
``mkfs.ext3``；内核磁盘管理格式化使用原生 ext3 格式化器。

.. code-block:: sh

   mkfs.ext3 /dev/sdX1
   mount -t ext3 -o data=ordered /dev/sdX1 /mnt

不指定数据模式时，内核使用 ordered 日志语义；显式 ``data=ordered`` 也可用。
事务保护元数据，提交前先刷新相关数据和日志；该机制不保证崩溃后所有文件
数据都恢复到应用期待的内容。

日志与格式化
============

原生格式化器使用 512 字节扇区、4 KiB 文件系统块和 inode 8 的内部 JBD 日志。
日志大小采用 e2fsprogs 的默认分档，最小文件系统为 8 MiB。格式化器初始化
日志和间接块、位图、组描述符及备份超级块，并在发布主超级块之前执行设备
刷新。写入或刷新失败会返回错误，部分写入可能已经发生。

挂载时共享引擎检查特性和日志状态，按需重放已提交事务；同步时先提交和检查点
日志，再刷新设备。支持的日志特性和介质布局由实际解析与校验决定，不代表
所有 Linux ext3 或 ext4 扩展均可写入。

验证
====

``tools/test_ext3_format.py`` 使用原生格式化器和宿主 e2fsprogs 检查最小卷、
跨块组日志及组描述符、ext2/ext4 回归和写入、刷新失败。它还通过 20 组写入
或刷新故障注入模拟断电，使用原生日志代码恢复，再以 ``e2fsck`` 检查；独立的
``e2fsck`` 也会重放已提交的日志。这些宿主测试不替代虚拟机或实际硬件验证。

磁盘结构参考 `Linux ext4 日志文档 <https://docs.kernel.org/filesystems/ext4/journal.html>`_
和 `Linux ext4 索引节点文档 <https://docs.kernel.org/filesystems/ext4/dynamic.html>`_。
