/* SPDX-License-Identifier: Apache-2.0 */
#include "boot.h"
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>
#include <linux/errno.h>

/** @brief Map only an explicit unique initramfs module with checked physical bounds.
 * @param boot Borrowed decoded handoff, nullable.
 * @param image Required borrowed output, cleared before validation.
 * @param length Required source byte output, cleared before validation.
 * @return One found, zero absent or EINVAL/EEXIST/ENOEXEC. BSP after mm reserves
 * every module and direct-map setup; no source copy/allocation/locks/disk I/O.
 */
int initramfs_boot_source(const struct boot_info *boot,const void **image,size_t *length)
{
    if(!image || !length)return -EINVAL;
    *image=NULL;*length=0;if(!boot)return 0;
    if(boot->module_count>sizeof(boot->modules)/sizeof(boot->modules[0]))return -EINVAL;
    const struct boot_module *selected=NULL;
    for(uint32_t i=0;i<boot->module_count;++i) {
        const struct boot_module *module=&boot->modules[i];
        if(!module->name || __builtin_strcmp(module->name,"initramfs"))continue;
        if(selected)return -EEXIST;
        selected=module;
    }
    if(!selected)return 0;
    if(!selected->start || selected->end<=selected->start)return -ENOEXEC;
    uint64_t bytes=selected->end-selected->start;
    if(bytes>SIZE_MAX || !paging_kernel_direct_map_range(selected->start,bytes))return -EINVAL;
    const void *mapping=paging_kernel_direct_map(selected->start);if(!mapping)return -EINVAL;
    *image=mapping;*length=(size_t)bytes;return 1;
}
