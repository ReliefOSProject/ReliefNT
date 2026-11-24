============
内核内部接口
============

按职责组织的内核部件可以内置，具有独立生命周期的部件也可加载为原生
``.ko``。内核私有接口随精确构建 ABI 绑定，不属于稳定 UAPI。

.. toctree::
   :maxdepth: 2

   modules
   execution
   workqueue
   kthread
   memory
   path-cache
   resource-limits
   drivers
   storage-completion
   virtio-blk
   network-devices
   e1000e
   i225
   realtek
   rtl8139
   vmxnet3
   ext3
   usb
   virtio-gpu
