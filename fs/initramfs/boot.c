/* SPDX-License-Identifier: Apache-2.0 */
#include "boot.h"
#include "unpack.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/mount.h>
#include <linux/errno.h>

static bool initial_memory_root;
/** @brief Construct a complete private newc root then bind its reserved volume role.
 * @param boot Borrowed handoff whose source ranges remain mm-reserved.
 * @return One constructed, zero absent, or negative busy/source/unpack errno.
 * BSP exclusive boot execution, before tasks/namespaces; no physical root
 * parsing or namespace publication. Failure frees all private filesystem state
 * and never exposes a partly decoded root. No registry lock spans allocation.
 */
int storage_initramfs_boot(const struct boot_info *boot)
{
    if(vfs_system_namespace() || initial_memory_root)return -EBUSY;
    const void *image=NULL;size_t length=0;
    int rc=initramfs_boot_source(boot,&image,&length);if(rc<=0)return rc;
    struct tmpfs_super *fs=NULL;rc=initramfs_unpack(image,length,STORAGE_VOLUME_ROOT,&fs);
    if(rc)return rc;
    storage_boot_reset();
    g_volumes[STORAGE_VOLUME_ROOT]=(struct storage_volume){.volume_id=STORAGE_VOLUME_ROOT,
        .kind=STORAGE_VOLUME_ANONYMOUS,.filesystem=STORAGE_FILESYSTEM_TMPFS,
        .fs_ops=&storage_tmpfs_ops,.tmpfs=fs,.ready=true};
    storage_copy_text(g_volumes[STORAGE_VOLUME_ROOT].tmpfs_source,
        sizeof(g_volumes[STORAGE_VOLUME_ROOT].tmpfs_source),"rootfs");
    initial_memory_root=true;return 1;
}
/** @brief Observe the selected initial memory-root PID1 contract.
 * @return True only after complete newc construction; boot-writer/execution
 * readers, no allocation, I/O, locks or ownership transfer.
 */
bool storage_initramfs_active(void)
{return initial_memory_root;}
