"""Separate VFS units linked against the storage image fixture's inode/I/O boundary.

This is a host-fixture composition, not a production source inventory. The
kernel's Kbuild directories remain the owners of production objects.
"""
STORAGE_GRAPH_SOURCES = [
    'kernel/dma/buffer.c',
    'drivers/storage/ahci/dma.c',
    'fs/vfs/mount_tree.c',
    'fs/vfs/filesystems.c',
    'fs/vfs/filesystem_instance.c',
    'fs/vfs/mount_table.c',
    'fs/vfs/path.c',
    'fs/vfs/dentry.c',
    'fs/vfs/file.c',
    'fs/vfs/open.c',
    'fs/vfs/kernel_file.c',
    'fs/vfs/kernel_path.c',
    'fs/vfs/attributes.c',
    'fs/vfs/storage_attributes.c',
    'fs/vfs/storage_access.c',
    'fs/vfs/storage_backend.c',
    'fs/vfs/mutation.c',
    'fs/vfs/storage_mutation.c',
    'fs/vfs/storage_create.c',
    'fs/vfs/storage_security.c',
    'fs/vfs/write_access.c',
    'fs/vfs/storage_mount.c',
    'fs/security/access.c',
    'fs/devfs/mount.c',
    'fs/devfs/identity.c',
    'fs/devfs/device.c',
    'fs/vfs/pseudo.c',
]
