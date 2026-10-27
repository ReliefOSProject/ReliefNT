/* SPDX-License-Identifier: Apache-2.0 */
#include "file_ops.h"
#include <reliefnt/device_mapping.h>
#include <reliefnt/drm.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <linux/errno.h>
#include <linux/mman.h>

struct drm_mapping_context {
    struct drm_bo *bo;
    struct task_file *file;
    uint64_t offset;
};
/** @brief Release the sharing family's BO and original canonical OFD lease.
 * @param value Owned context with one BO and OFD reference.
 * Execution-owned task context without registry locks. BO pages retire before
 * the OFD's final backend close; the generic object still pins provider code.
 */
static void drm_mapping_release(void *value)
{
    struct drm_mapping_context *context=value;
    drm_bo_put(context->bo);task_file_put(context->file);
}
/** @brief Free context after the final BO/OFD release has completed.
 * @param value Owned drained context, consumed once.
 * Task/execution ownership required; no dereference of the already retired BO.
 */
static void drm_mapping_destroy(void *value)
{kernel_free(value);}
/** @brief Resolve the actual BO page instead of assuming contiguous allocation.
 * @param value Retained BO context. @param offset In-bounds generic page offset.
 * @return Actual physical address or zero on overflow/invalid BO offset.
 * Execution-owned, no allocation, registry lock or reference transfer.
 */
static uint64_t drm_mapping_physical(void *value,uint64_t offset)
{
    const struct drm_mapping_context *context=value;
    return offset>UINT64_MAX-context->offset ? 0 : drm_bo_page_physical(context->bo,context->offset+offset);
}
/** @brief Validate DRM mmap and transactionally adopt its BO/OFD lease.
 * @param task Current caller. @param file Admitted canonical card/PRIME OFD.
 * @param offset DRM BO selector. @param length Rounded nonzero page extent.
 * @param prot Requested protection. @param flags Native mmap flags.
 * @param out Required empty output, one owned generic reference on success.
 * @return Zero or backend/ENOMEM/EACCES/EINVAL errno. Execution ownership and
 * original OFD/provider pin required. All failures balance acquired resources;
 * no graph/module/character registry lock spans allocation or final put.
 */
int drm_character_mmap(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                       uint32_t prot,uint64_t flags,struct device_mapping **out)
{
    (void)task;
    if(!out)return -EINVAL;*out=NULL;
    if(file->drm_prime_file)return drm_prime_file_mmap(file,flags,prot);
    if(!file->drm_file)return -EBADF;
    if((flags&(LINUX_MAP_SHARED|LINUX_MAP_PRIVATE))!=LINUX_MAP_SHARED || (prot&LINUX_PROT_EXEC))
        return prot&LINUX_PROT_EXEC ? -EACCES : -EINVAL;
    if(!file_can_read(file))return -EACCES;
    uint32_t max_prot=LINUX_PROT_READ|(file_can_write(file) ? LINUX_PROT_WRITE : 0);
    if(prot&~max_prot)return -EACCES;
    struct drm_bo *bo=NULL;uint64_t bo_offset=0;
    int rc=drm_file_mmap_bo(file,offset,length,&bo,&bo_offset);if(rc)return rc;
    struct task_file *pin=task_file_get(file);
    if(!pin){drm_bo_put(bo);return -ENOMEM;}
    struct drm_mapping_context *context=kernel_malloc(sizeof(*context));
    if(!context){drm_bo_put(bo);task_file_put(pin);return -ENOMEM;}
    *context=(struct drm_mapping_context){.bo=bo,.file=pin,.offset=bo_offset};
    static const struct device_mapping_operations operations={
        .release=drm_mapping_release,.destroy=drm_mapping_destroy,.physical=drm_mapping_physical};
    struct device_mapping_info info={.length=length,.max_prot=max_prot,.can_split=true,.can_move=true};
    rc=device_mapping_create(THIS_MODULE,&operations,context,&info,out);
    if(rc){drm_mapping_release(context);kernel_free(context);}return rc;
}
