/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/fs_type.h>
#include <reliefnt/module.h>

static int squashfs_get_tree(const struct fs_mount_request *request, struct vfs_mount **out)
{
    return storage_block_get_tree("squashfs", request, out);
}

static const struct file_system_type squashfs_type = {
    .name = "squashfs",
    .flags = FS_REQUIRES_DEV,
    .get_tree = squashfs_get_tree,
    .storage = &storage_squashfs_ops,
};

static int squashfs_init(void)
{
    return register_filesystem(&squashfs_type);
}
fs_initcall(squashfs_init);
