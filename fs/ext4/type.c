/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../ext_common/internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Construct one registered ext4 instance with its frontend/code pin.
 * @param request Borrowed validated kernel operands with a resolved block source.
 * @param out Required owned mount output, cleared on failure.
 * @return Zero or native constructor errno. Execution ownership required; the
 * frontend's ELF dependency retains the actual shared ext implementation.
 */
static int ext4_get_tree(const struct fs_mount_request *request,struct vfs_mount **out)
{ return storage_block_get_tree("ext4",request,out); }

static const struct file_system_type ext4_filesystem={
    .name="ext4",.owner=THIS_MODULE,.flags=FS_REQUIRES_DEV,
    .get_tree=ext4_get_tree,.format=install_format_ext4,.storage=&storage_ext_ops};

/** @brief Register the ext4 name without creating a permanent module self-pin.
 * @return Zero or registration errno. Early builtin phase or module init; COMING
 * admission stays closed, and the module ledger owns failed-init/final rollback.
 */
static int ext4_register_filesystem(void)
{ return register_filesystem(&ext4_filesystem); }
fs_initcall(ext4_register_filesystem);

/** @brief Close ext4 name admission after all mounted/external users drain.
 * Automatic registration cleanup remains idempotent, no hardware or I/O callback.
 */
static void ext4_unregister_filesystem(void)
{ (void)unregister_filesystem(&ext4_filesystem); }
module_exit(ext4_unregister_filesystem);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT ext4 filesystem frontend");
MODULE_ALIAS("fs-ext4");
