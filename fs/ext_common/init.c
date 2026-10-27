/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/module.h>

/** @brief Check shared cache obligations before allowing native-code retirement.
 * @param context Unused self-resource context, NULL.
 * @return Zero or -EBUSY for dirty/pinned cache state. Task context; briefly owns
 * storage execution, no provider/module lock or hardware I/O. Frontend dependency
 * references must already be zero, so no new native cache user can enter.
 */
static int ext_common_quiesce(void *context)
{
    (void)context;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    int rc=storage_ext4_cache_quiesce();
    kernel_execution_unlock_irqrestore(flags);return rc;
}

/** @brief Release shared native cache pages after frontend/execution drain.
 * Task context after successful quiesce; execution exclusion protects globals.
 * Final release is idempotent for module_exit and automatic resource rollback.
 */
static void ext_common_destroy(void)
{
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    (void)storage_ext4_cache_release();
    (void)ext2_cache_release();
    kernel_execution_unlock_irqrestore(flags);
}

/** @brief Consume the module's actual physical-cache cleanup obligation.
 * @param context Unused self-resource context, NULL.
 * Called only after quiesce and callback drain; no importer/frontends remain.
 */
static void ext_common_release(void *context)
{ (void)context;ext_common_destroy(); }

/** @brief Register cache teardown without pinning the shared module to itself.
 * @return Zero or ledger allocation errno. Builtin early phase or module init;
 * no parser/hardware work is performed, and no private caches exist on failure.
 */
static int ext_common_initialize(void)
{ return module_add_resource(THIS_MODULE,ext_common_quiesce,ext_common_release,NULL); }
fs_initcall(ext_common_initialize);
module_exit(ext_common_destroy);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT shared native ext filesystem implementation");
