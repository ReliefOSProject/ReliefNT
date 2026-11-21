========
SquashFS
========

ReliefNT 默认内建 ``CONFIG_SQUASHFS=y``。它提供 SquashFS 4.0 的只读挂载，
支持 gzip 压缩块和未压缩块、普通文件、目录、稀疏文件及软硬链接，
并保留文件权限、数字属主和属组。无需载入整个解压后的镜像。

制作和挂载镜像
==============

在宿主机生成兼容镜像（``staging`` 为要打包的目录）::

    mksquashfs staging payload.squashfs -noappend -comp gzip \
        -no-fragments -no-xattrs

镜像必须位于支持的块设备上。引导器以 ``reliefos-installer-payload`` 为名称提供
Multiboot2 模块时，内核将其注册为只读 ``/dev/ram1``。从来宾挂载：

.. code-block:: sh

    mkdir -p /mnt/payload
    mount -t squashfs -o ro,nosuid,nodev /dev/ram1 /mnt/payload

限制
====

不支持写入、fragment 尾块、扩展属性或 gzip 以外的压缩算法。
目录项名称仍受 ReliefNT 现有目录接口限制，最多 127 字节。
当前尚未接入 GPT 分区上的 SquashFS 挂载；普通镜像文件也不能直接传给
``mount(2)``，应通过已支持的块设备提供。
