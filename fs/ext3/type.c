/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "../ext_common/internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Construct an ext3 mount with retained frontend and shared engine ownership.
 * @param request Borrowed validated mount operands and resolved block source.
 * @param out Required mount result, cleared on failure.
 * @return Zero or negative constructor errno. Storage execution ownership is required.
 */
static int ext3_get_tree(const struct fs_mount_request *request, struct vfs_mount **out)
{
    if (request && request->options && !__builtin_strcmp(request->options, "data=ordered")) {
        struct fs_mount_request ordered = *request;
        ordered.options = NULL;
        return storage_block_get_tree("ext3", &ordered, out);
    }
    return storage_block_get_tree("ext3", request, out);
}

static const struct file_system_type ext3_filesystem = {
    .name = "ext3", .owner = THIS_MODULE, .flags = FS_REQUIRES_DEV,
    .get_tree = ext3_get_tree, .format = install_format_ext3, .storage = &storage_ext_ops,
};

/** @brief Register ext3 while allowing the module ledger to unwind failed admission.
 * @return Zero or a negative filesystem registry errno.
 */
static int ext3_register_filesystem(void)
{
    return register_filesystem(&ext3_filesystem);
}
fs_initcall(ext3_register_filesystem);

/** @brief Withdraw ext3 after all mounts, callbacks and external references drain. */
static void ext3_unregister_filesystem(void)
{
    (void)unregister_filesystem(&ext3_filesystem);
}
module_exit(ext3_unregister_filesystem);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT ext3 journaled filesystem frontend");
MODULE_ALIAS("fs-ext3");
