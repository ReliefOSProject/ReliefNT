========
SquashFS
========

ReliefNT 默认内建 ``CONFIG_SQUASHFS=y``。它提供 SquashFS 4.0 只读挂载，
按需解压元数据、文件数据块和多个文件共享的 fragment 尾块。压缩算法包括
gzip、XZ、Zstandard、LZ4、LZO 和旧版 LZMA，也可以读取未压缩的数据块。
目录、稀疏文件、软硬链接、文件权限及数字属主和属组使用镜像中保存的值。
Linux 路径和目录接口支持最长 255 字节的文件名。

制作和挂载镜像
==============

在宿主机生成兼容镜像（``staging`` 为要打包的目录）::

    mksquashfs staging payload.squashfs -noappend -comp zstd

将镜像放在来宾可访问的块设备或 GPT 分区上，再使用 ``mount -t squashfs -o ro``
挂载。引导器以 ``reliefos-installer-payload`` 为名称提供 Multiboot2 模块时，
内核将其注册为只读 ``/dev/ram1``。

.. code-block:: sh

    mkdir -p /mnt/payload
    mount -t squashfs -o ro,nosuid,nodev /dev/ram1 /mnt/payload

扩展属性
========

``getxattr(2)`` 和 ``listxattr(2)`` 以及它们的 ``l*``、``f*`` 变体可以读取
内联属性值和外置的共享属性值。支持 ``user.*``、``trusted.*`` 和 ``security.*``
命名空间。``user.*`` 属性读取遵循文件的读权限，``trusted.*`` 仅对具有
``CAP_SYS_ADMIN`` 的调用者可见。设置或删除属性返回 ``EROFS``。

使用边界
========

当前内核通过块设备读取镜像。loop 块设备、NFS 导出和 POSIX ACL 仍需要相应的
内核支持。``security.*`` 在这里是可读取的镜像元数据。读取标签不会启用 SELinux
策略或基于文件属性的权限授予。ReliefNT 私有目录接口保留原有的固定结构大小。
