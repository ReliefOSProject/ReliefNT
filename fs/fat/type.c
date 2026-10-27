/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Construct the registered vfat backend from a resolved block node.
 * @param request Borrowed validated kernel operands. @param out Owned mount output.
 * @return Zero or negative provider errno. Execution ownership and factory pin
 * required; common block construction retains its independent instance owner.
 */
static int vfat_get_tree(const struct fs_mount_request *request,struct vfs_mount **out)
{ return storage_block_get_tree("vfat",request,out); }
static const struct file_system_type vfat_filesystem={.name="vfat",.owner=THIS_MODULE,
    .flags=FS_REQUIRES_DEV,.get_tree=vfat_get_tree,.format=install_format_fat32,.storage=&storage_fat_ops};
/** @brief Register this native family in the early filesystem phase or module init.
 * @return Zero or registration errno. Init context; failed registration rolls
 * back earlier names, while the module ledger owns final automatic removal.
 */
static int fat_register_filesystem(void)
{
    return register_filesystem(&vfat_filesystem);
}
fs_initcall(fat_register_filesystem);
/** @brief Close family name admission after all instance users have retired.
 * @return None. Unload task context; automatic resources are idempotent and
 * reclaim core registrations after callback drain without an external self-pin.
 */
static void fat_unregister_filesystem(void)
{
    (void)unregister_filesystem(&vfat_filesystem);
}
module_exit(fat_unregister_filesystem);
MODULE_LICENSE("Apache-2.0");
