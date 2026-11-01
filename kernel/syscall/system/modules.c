/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/module.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/heap.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/paging.h>
#include <reliefnt/swap.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/errno.h>

#ifdef CONFIG_MODULES
#ifdef CONFIG_SWAP
/* Each scoped operand is one allocation, so common lookup cancellation owns
 * the complete copied prefix without retaining any user alias across waits. */
struct module_user_operand {
    uint64_t address, extent;
    size_t used, capacity;
    bool complete;
    unsigned char bytes[];
};

/** @brief Acquire one immutable module-operand retry slot.
 * @param task Current execution-owned task. @param kind Private discriminator.
 * @param address Original numerical user address. @param extent Count/limit.
 * @param capacity Initial bytes to allocate. @param slot Receives owned scope
 * slot or NULL. @param out Receives its private allocated operand.
 * @return Zero or errno; an armed scope owns the allocation until cancellation.
 */
static int module_operand_acquire(struct task *task,uint32_t kind,uint64_t address,
                                  uint64_t extent,size_t capacity,
                                  struct task_lookup_path **slot,
                                  struct module_user_operand **out)
{
    *slot=NULL;*out=NULL;
    int rc=task_lookup_scope_next(task,slot);if(rc)return rc;
    if(*slot && (*slot)->name) {
        struct module_user_operand *old=(void *)(*slot)->name;
        if((*slot)->flags!=kind || old->address!=address || old->extent!=extent ||
           old->used>old->capacity)return -EINVAL;
        *out=old;return 0;
    }
    if(capacity>SIZE_MAX-sizeof(struct module_user_operand))return -ENOMEM;
    struct module_user_operand *value=kernel_malloc(sizeof(*value)+capacity);
    if(!value)return -ENOMEM;
    *value=(struct module_user_operand){.address=address,.extent=extent,.capacity=capacity};
    if(*slot){(*slot)->flags=kind;(*slot)->name=(char *)value;}
    *out=value;return 0;
}

/** @brief Discard one failed operand without leaving a scope-owned dangling pointer. */
static void module_operand_discard(struct task_lookup_path *slot,struct module_user_operand *value)
{
    if(slot)slot->name=NULL;
    kernel_free(value);
}

/** @brief Import a module image in bounded slices, retaining accepted bytes.
 * @param task Current task. @param address Original user image address.
 * @param length Caller-validated ELF byte count. @param image Borrowed output.
 * @param allocation Unscoped allocation to free, otherwise NULL.
 * @return Zero, ordinary errno, or prerequisite-contention marker. No ELF
 * parser or initializer runs until the complete immutable snapshot exists.
 */
static int module_copy_image(struct task *task,uint64_t address,size_t length,
                             const void **image,void **allocation)
{
    *image=NULL;*allocation=NULL;
    struct task_lookup_path *slot;struct module_user_operand *value;
    int rc=module_operand_acquire(task,TASK_LOOKUP_MODULE_IMAGE_KIND,address,length,length,&slot,&value);
    if(rc)return rc;
    if(address<RELIEFNT_USER_BASE || address>=RELIEFNT_USER_TOP ||
       length>RELIEFNT_USER_TOP-address){module_operand_discard(slot,value);return -EFAULT;}
    while(value->used<length) {
        size_t part=length-value->used;if(part>32768u)part=32768u;
        uint64_t at=address+value->used;
        rc=swap_prepare_user_range(task,at,part,false);
        if(!rc && user_copy_from_task(task,value->bytes+value->used,at,part))rc=-EFAULT;
        if(rc) {
            if(rc!=SWAP_STORAGE_CONTENDED || !slot)module_operand_discard(slot,value);
            return rc==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -EFAULT;
        }
        value->used+=part;
    }
    value->complete=true;*image=value->bytes;if(!slot)*allocation=value;
    return 0;
}

/** @brief Snapshot an argument string or the existing truncated module name.
 * @param task Current task. @param address User string address. @param name
 * True for the ABI's at-most-55-byte name; false for growable parameters.
 * @param out Borrowed terminated bytes. @param allocation Unscoped owner.
 * @return Zero, errno, or prerequisite contention. A scoped prefix is kept
 * through waits and is never recaptured from mutable userspace on retry.
 */
static int module_copy_string(struct task *task,uint64_t address,bool name,
                              char **out,void **allocation)
{
    *out=NULL;*allocation=NULL;if(!address)return -EFAULT;
    struct task_lookup_path *slot;struct module_user_operand *value;
    size_t initial=name ? MODULE_NAME_LEN : 256u;
    int rc=module_operand_acquire(task,name ? TASK_LOOKUP_MODULE_NAME_KIND : TASK_LOOKUP_MODULE_PARAMETERS_KIND,
                                  address,name ? MODULE_NAME_LEN-1u : 0,initial,&slot,&value);
    if(rc)return rc;
    while(!value->complete) {
        if(name && value->used==MODULE_NAME_LEN-1u) {
            value->bytes[value->used]=0;value->complete=true;break;
        }
        if(value->used>UINT64_MAX-address){module_operand_discard(slot,value);return -EFAULT;}
        uint64_t at=address+value->used;
        size_t count=name ? 1u : 256u,remaining=4096u-(size_t)(at&4095u);
        if(count>remaining)count=remaining;
        unsigned char part[256];
        rc=swap_prepare_user_range(task,at,count,false);
        if(!rc && user_copy_from_task(task,part,at,count))rc=-EFAULT;
        if(rc) {
            if(rc!=SWAP_STORAGE_CONTENDED || !slot)module_operand_discard(slot,value);
            return rc==SWAP_STORAGE_CONTENDED ? KERNEL_SYSCALL_SWAP_CONTENDED : -EFAULT;
        }
        size_t length=0;while(length<count && part[length])++length;
        bool terminated=length<count;size_t copy=length+(terminated ? 1u : 0u);
        if(copy>SIZE_MAX-value->used){module_operand_discard(slot,value);return -E2BIG;}
        if(value->used+copy>value->capacity) {
            size_t grown=value->capacity;
            while(grown<value->used+copy) {
                if(grown>SIZE_MAX/2u){module_operand_discard(slot,value);return -E2BIG;}
                grown*=2u;
            }
            if(grown>SIZE_MAX-sizeof(*value)){module_operand_discard(slot,value);return -E2BIG;}
            struct module_user_operand *next=kernel_malloc(sizeof(*next)+grown);
            if(!next){module_operand_discard(slot,value);return -ENOMEM;}
            __builtin_memcpy(next,value,sizeof(*next)+value->used);next->capacity=grown;
            if(slot)slot->name=(char *)next;kernel_free(value);value=next;
        }
        __builtin_memcpy(value->bytes+value->used,part,copy);value->used+=copy;
        if(terminated)value->complete=true;
    }
    *out=(char *)value->bytes;if(!slot)*allocation=value;
    return 0;
}
#else
/** @brief Copy an arbitrarily sized user argument string into growable private storage.
 * @param task Pinned calling task under the execution transaction.
 * @param address User pointer; NULL/unreadable pages return -EFAULT.
 * @param out Required owned string result; remains NULL on error.
 * @return Zero or negative errno; reads never cross an unchecked user page boundary.
 */
static int module_copy_arguments(struct task *task,uint64_t address,char **out,void **allocation)
{
    *out=NULL;*allocation=NULL;
    if(!address) return -EFAULT;
    size_t used=0,capacity=256;
    char *buffer=kernel_malloc(capacity);
    if(!buffer) return -ENOMEM;
    for(;;) {
        if(used>UINT64_MAX-address) { kernel_free(buffer); return -EFAULT; }
        uint64_t at=address+used;
        size_t count=256,page_remaining=4096-(size_t)(at&4095);
        if(count>page_remaining) count=page_remaining;
        char part[256];
        int rc=user_copy_from_task(task,part,at,count);
        if(rc) { kernel_free(buffer); return -EFAULT; }
        size_t length=0; while(length<count && part[length]) ++length;
        bool terminated=length<count;
        size_t copy=length+(terminated ? 1u : 0u);
        if(copy>SIZE_MAX-used) { kernel_free(buffer); return -E2BIG; }
        if(used+copy>capacity) {
            size_t grown=capacity;
            while(grown<used+copy) {
                if(grown>SIZE_MAX/2) { kernel_free(buffer); return -E2BIG; }
                grown*=2;
            }
            char *replacement=kernel_malloc(grown);
            if(!replacement) { kernel_free(buffer); return -ENOMEM; }
            __builtin_memcpy(replacement,buffer,used); kernel_free(buffer);
            buffer=replacement; capacity=grown;
        }
        __builtin_memcpy(buffer+used,part,copy); used+=copy;
        if(terminated) { *out=buffer; *allocation=buffer; return 0; }
    }
}
#endif
#endif

/** @brief Authorize and validate a module-file request before any pin allocation.
 * @param fd Linux file descriptor, consumed as signed 32 bits.
 * @param flags Linux finit_module flags, consumed as unsigned 32 bits.
 * @return Zero or Linux errno. Task/execution context, no usercopy, I/O or
 * reference transfer. The caller retains the transaction through OFD pinning.
 */
int syscall_module_file_prepare(uint64_t fd,uint64_t flags)
{
#ifndef CONFIG_MODULES
    (void)fd;(void)flags;return -ENOSYS;
#else
    struct task *task=sched_current_task();
    if(!task || !(task->cap_effective&(1ULL<<CAP_SYS_MODULE))) return -EPERM;
    return module_file_validate(task_file_for_io(task,(int32_t)fd),(unsigned int)flags);
#endif
}

/** @brief Dispatch Linux module calls after CAP_SYS_MODULE authorization and safe copies.
 * @param number Linux x86-64 syscall number (175, 176 or 313).
 * @param a0 Image pointer, module-name pointer or descriptor number.
 * @param a1 Image length, delete flags or user parameter pointer.
 * @param a2 User parameter pointer or finit_module flags, according to number.
 * @return Linux result/errno or private MODULE_FILE_IO_RETRY consumed by frame
 * entry before return. Never exposes user storage to the ELF parser.
 * Task context; OFDs and image snapshots remain owned through every failure path.
 */
int64_t syscall_modules(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2)
{
#ifndef CONFIG_MODULES
    (void)number; (void)a0; (void)a1; (void)a2; return -ENOSYS;
#else
#ifndef CONFIG_MODULE_UNLOAD
    if(number==LINUX_SYS_DELETE_MODULE) return -ENOSYS;
#endif
    struct task *task=sched_current_task();
    if(!task || !(task->cap_effective&(1ULL<<CAP_SYS_MODULE))) return -EPERM;
    if(number==LINUX_SYS_DELETE_MODULE) {
#ifdef CONFIG_SWAP
        char *name=NULL;void *allocation=NULL;
        int rc=module_copy_string(task,a0,true,&name,&allocation);
        if(!rc)rc=module_unload(name,(unsigned int)a1);
        kernel_free(allocation);return rc;
#else
        char name[MODULE_NAME_LEN];
        size_t n=0;
        for(;n<MODULE_NAME_LEN-1;++n) {
            if(a0>UINT64_MAX-n || user_copy_from_task(task,&name[n],a0+n,1)) return -EFAULT;
            if(!name[n]) break;
        }
        name[n]=0;
        return module_unload(name,(unsigned int)a1);
#endif
    }
    if(number==LINUX_SYS_INIT_MODULE) {
        if(a1<64) return -ENOEXEC;
        if(a1>SIZE_MAX) return -ENOMEM;
        const void *image=NULL;void *image_allocation=NULL;
        char *arguments=NULL;void *arguments_allocation=NULL;
#ifdef CONFIG_SWAP
        int rc=module_copy_image(task,a0,(size_t)a1,&image,&image_allocation);
        if(!rc) rc=module_copy_string(task,a2,false,&arguments,&arguments_allocation);
#else
        image_allocation=kernel_malloc((size_t)a1);image=image_allocation;
        if(!image)return -ENOMEM;
        int rc=user_copy_from_task(task,image_allocation,a0,a1) ? -EFAULT : 0;
        if(!rc)rc=module_copy_arguments(task,a2,&arguments,&arguments_allocation);
#endif
        if(!rc) rc=module_load_image(image,(size_t)a1,arguments,0);
        kernel_free(arguments_allocation);kernel_free(image_allocation);return rc;
    }
    if(number==LINUX_SYS_FINIT_MODULE) {
        unsigned int flags=(unsigned int)a2;
        int preflight=syscall_module_file_prepare(a0,a2);
        if(preflight) return preflight;
        struct task_file *file=task_file_for_io(task,(int32_t)a0);
        file=task_file_get(file);
        if(!file) return -ENOMEM;
        char *arguments=NULL;void *arguments_allocation=NULL;
#ifdef CONFIG_SWAP
        int rc=module_copy_string(task,a1,false,&arguments,&arguments_allocation);
#else
        int rc=module_copy_arguments(task,a1,&arguments,&arguments_allocation);
#endif
        if(!rc) rc=module_load_file(file,arguments,flags);
        kernel_free(arguments_allocation);task_file_put(file);return rc;
    }
    return -ENOSYS;
#endif
}
