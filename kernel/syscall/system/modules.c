/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/module.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/heap.h>
#include <reliefnt/usercopy.h>
#include <linux/capability.h>
#include <linux/module.h>
#include <linux/errno.h>

#ifdef CONFIG_MODULES
/** @brief Copy an arbitrarily sized user argument string into growable private storage.
 * @param task Pinned calling task under the execution transaction.
 * @param address User pointer; NULL/unreadable pages return -EFAULT.
 * @param out Required owned string result; remains NULL on error.
 * @return Zero or negative errno; reads never cross an unchecked user page boundary.
 */
static int module_copy_arguments(struct task *task,uint64_t address,char **out)
{
    *out=NULL;
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
        if(terminated) { *out=buffer; return 0; }
    }
}
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
        char name[MODULE_NAME_LEN];
        size_t n=0;
        for(;n<MODULE_NAME_LEN-1;++n) {
            if(a0>UINT64_MAX-n || user_copy_from_task(task,&name[n],a0+n,1)) return -EFAULT;
            if(!name[n]) break;
        }
        name[n]=0;
        return module_unload(name,(unsigned int)a1);
    }
    if(number==LINUX_SYS_INIT_MODULE) {
        if(a1<64) return -ENOEXEC;
        if(a1>SIZE_MAX) return -ENOMEM;
        void *image=kernel_malloc((size_t)a1);
        if(!image) return -ENOMEM;
        int rc=user_copy_from_task(task,image,a0,a1) ? -EFAULT : 0;
        char *arguments=NULL;
        if(!rc) rc=module_copy_arguments(task,a2,&arguments);
        if(!rc) rc=module_load_image(image,(size_t)a1,arguments,0);
        kernel_free(arguments); kernel_free(image); return rc;
    }
    if(number==LINUX_SYS_FINIT_MODULE) {
        unsigned int flags=(unsigned int)a2;
        int preflight=syscall_module_file_prepare(a0,a2);
        if(preflight) return preflight;
        struct task_file *file=task_file_for_io(task,(int32_t)a0);
        file=task_file_get(file);
        if(!file) return -ENOMEM;
        char *arguments=NULL;
        int rc=module_copy_arguments(task,a1,&arguments);
        if(!rc) rc=module_load_file(file,arguments,flags);
        kernel_free(arguments); task_file_put(file); return rc;
    }
    return -ENOSYS;
#endif
}
