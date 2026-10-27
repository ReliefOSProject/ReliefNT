/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Construct the registered iso9660 backend from a resolved block node.
 * @param request Borrowed validated kernel operands. @param out Owned mount output.
 * @return Zero or negative provider errno. Execution ownership and factory pin
 * required; common block construction retains its independent instance owner.
 */
static int iso9660_get_tree(const struct fs_mount_request *request,struct vfs_mount **out)
{ return storage_block_get_tree("iso9660",request,out); }
static const struct file_system_type iso9660_filesystem={.name="iso9660",.owner=THIS_MODULE,
    .flags=FS_REQUIRES_DEV,.get_tree=iso9660_get_tree,.storage=&storage_iso_ops};
/** @brief Register this native family in the early filesystem phase or module init.
 * @return Zero or registration errno. Init context; failed registration rolls
 * back earlier names, while the module ledger owns final automatic removal.
 */
static int iso_register_filesystem(void)
{
    return register_filesystem(&iso9660_filesystem);
}
fs_initcall(iso_register_filesystem);
/** @brief Close family name admission after all instance users have retired.
 * @return None. Unload task context; automatic resources are idempotent and
 * reclaim core registrations after callback drain without an external self-pin.
 */
static void iso_unregister_filesystem(void)
{
    (void)unregister_filesystem(&iso9660_filesystem);
}
module_exit(iso_unregister_filesystem);
MODULE_LICENSE("Apache-2.0");
