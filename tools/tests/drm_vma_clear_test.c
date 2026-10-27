/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <reliefnt/drm.h>
#include <reliefnt/sched.h>
#include <reliefnt/device_mapping.h>
#include <linux/errno.h>
#include <linux/mman.h>
#include "exec_source_fixture_stubs.h"
#include "../../drivers/video/drm/file_ops.h"

/* The BO allocator and canonical OFD are the hardware/file boundary. The
 * production DRM adapter, generic reference family and VM clear run unchanged. */
static struct drm_bo *const expected_bo=(struct drm_bo *)(uintptr_t)0x1000;
static struct task_file expected_file;
static unsigned bo_refs,file_refs,allocations;
static int fail_after=-1,fail_pin;
struct task *sched_current_task(void){return NULL;}
void *kernel_malloc(size_t bytes)
{
    if(!fail_after)return NULL;
    if(fail_after>0)--fail_after;
    void *memory=malloc(bytes);if(memory)++allocations;return memory;
}
void kernel_free(void *memory){if(memory){assert(allocations);--allocations;free(memory);}}
void drm_bo_put(struct drm_bo *bo){assert(bo==expected_bo && bo_refs);--bo_refs;}
int drm_file_mmap_bo(struct task_file *file,uint64_t offset,uint64_t length,
                     struct drm_bo **out,uint64_t *bo_offset)
{
    assert(file==&expected_file && offset==0x8000 && length==8192);
    ++bo_refs;*out=expected_bo;*bo_offset=4096;return 0;
}
uint64_t drm_bo_page_physical(const struct drm_bo *bo,uint64_t offset)
{assert(bo==expected_bo && (offset==4096 || offset==8192));return 0x200000+offset*2;}
struct task_file *task_file_get(struct task_file *file)
{assert(file==&expected_file);if(fail_pin)return NULL;++file_refs;return file;}
void task_file_put(struct task_file *file){assert(file==&expected_file && file_refs);--file_refs;}
int file_can_read(const struct task_file *file){return (file->flags&LINUX_O_ACCMODE)!=LINUX_O_WRONLY;}
int file_can_write(const struct task_file *file){return (file->flags&LINUX_O_ACCMODE)!=LINUX_O_RDONLY;}
int drm_prime_file_mmap(struct task_file *file,uint64_t flags,uint64_t prot)
{(void)file;(void)flags;(void)prot;assert(!"unexpected PRIME path");return -EINVAL;}
void sysv_shm_vma_release(struct task *task,struct task_vma *vma){(void)task;(void)vma;}
int storage_inode_put(struct storage_inode_ref *reference){assert(!reference);return 0;}
#ifndef DRM_MM_SOURCE
#define DRM_MM_SOURCE "modules/user_memory.h"
#endif
#include DRM_MM_SOURCE

int main(void)
{
    expected_file=(struct task_file){.used=1,.flags=LINUX_O_RDWR,.drm_file=(struct drm_file *)(uintptr_t)1};
    struct device_mapping *mapping=NULL;
    fail_pin=1;
    assert(drm_character_mmap(NULL,&expected_file,0x8000,8192,3,LINUX_MAP_SHARED,&mapping)==-ENOMEM);
    assert(!mapping && !bo_refs && !file_refs);fail_pin=0;
    for(int failure=0;failure<2;++failure) {
        fail_after=failure;
        assert(drm_character_mmap(NULL,&expected_file,0x8000,8192,3,LINUX_MAP_SHARED,&mapping)==-ENOMEM);
        assert(!mapping && !bo_refs && !file_refs && !allocations);
    }
    fail_after=-1;
    assert(drm_character_mmap(NULL,&expected_file,0x8000,8192,3,LINUX_MAP_PRIVATE,&mapping)==-EINVAL);
    assert(drm_character_mmap(NULL,&expected_file,0x8000,8192,LINUX_PROT_EXEC,LINUX_MAP_SHARED,&mapping)==-EACCES);
    assert(!mapping && !bo_refs && !file_refs && !allocations);
    expected_file.flags=LINUX_O_RDONLY;
    assert(drm_character_mmap(NULL,&expected_file,0x8000,8192,3,LINUX_MAP_SHARED,&mapping)==-EACCES);
    assert(!drm_character_mmap(NULL,&expected_file,0x8000,8192,1,LINUX_MAP_SHARED,&mapping));
    assert(bo_refs==1 && file_refs==1 && allocations==2);
    assert(device_mapping_info(mapping)->max_prot==LINUX_PROT_READ);
    assert(device_mapping_info(mapping)->can_split && device_mapping_info(mapping)->can_move);
    assert(device_mapping_physical(mapping,0)==0x202000);
    assert(device_mapping_physical(mapping,4096)==0x204000);
    assert(!device_mapping_physical(mapping,8192));
    struct task_vma first={.used=1,.device_mapping=mapping},second=first;
    assert(!device_mapping_get(mapping));
    task_vma_clear(&first);
    assert(!first.used && !first.device_mapping && bo_refs==1 && file_refs==1);
    task_vma_clear(&second);
    assert(!second.used && !second.device_mapping && !bo_refs && !file_refs && !allocations);
    puts("drm-vma-clear: PASS actual BO/OFD lease, noncontiguous PFNs, sharing/final clear, access policy and allocation rollback");
}
