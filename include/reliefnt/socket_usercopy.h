/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SOCKET_USERCOPY_H
#define RELIEFNT_SOCKET_USERCOPY_H
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <linux/errno.h>

/** @brief Prepare one actual socket operand at the outer execution boundary.
 * @param task Current user task. @param address First user byte.
 * @param length Actual extent, at most 32768. @param output True for writes.
 * @return Zero, EFAULT or private prerequisite wait. No user alias is held
 * after return; caller keeps the writer through its subsequent direct access.
 */
static inline int socket_user_prepare(struct task *task,uint64_t address,
                                      uint64_t length,bool output)
{
#ifdef CONFIG_SWAP
    if(task && task->kind==TASK_KIND_USER && length) {
        int ret=swap_prepare_user_range(task,address,length,output);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret<0)return -EFAULT;
    }
#else
    (void)task;(void)address;(void)length;(void)output;
#endif
    return 0;
}

/** @brief Import an actual fixed socket/ioctl input once across retries.
 * @param task Current task. @param address User source. @param destination
 * Kernel output buffer. @param length Nonzero bounded operand length.
 * @return Zero, EFAULT, allocation error or private prerequisite wait.
 * An armed syscall scope owns its snapshot until normal cancellation.
 */
static inline int socket_user_import(struct task *task,uint64_t address,
                                     void *destination,uint32_t length)
{
#ifdef CONFIG_SWAP
    if(task && task->kind==TASK_KIND_USER) {
        struct task_user_text snapshot={0};
        int rc=task_lookup_user_bytes(task,address,length,&snapshot);
        if(rc)return rc;
        __builtin_memcpy(destination,snapshot.text,length);
        kernel_free(snapshot.allocation);return 0;
    }
#else
    (void)task;
#endif
    if(!user_range_ok(address,length))return -EFAULT;
    __builtin_memcpy(destination,(const void *)(uintptr_t)address,length);return 0;
}

/** @brief Read and retain one in/out socklen value across prerequisite waits.
 * @param task Current task. @param address Writable socklen_t user pointer.
 * @param value Receives its original native 32-bit capacity.
 * @return Zero, EFAULT, ENOMEM or private wait. The syscall lookup scope owns
 * any copied input until common cancellation; no value is reread on retry.
 */
static inline int socket_user_length(struct task *task,uint64_t address,uint32_t *value)
{
    int rc=socket_user_prepare(task,address,sizeof(*value),true);if(rc)return rc;
    if(!user_range_writable(address,sizeof(*value)))return -EFAULT;
#ifdef CONFIG_SWAP
    if(task && task->kind==TASK_KIND_USER) {
        struct task_user_text snapshot={0};
        rc=task_lookup_user_bytes(task,address,sizeof(*value),&snapshot);
        if(rc)return rc;
        __builtin_memcpy(value,snapshot.text,sizeof(*value));kernel_free(snapshot.allocation);
        return 0;
    }
#endif
    __builtin_memcpy(value,(const void *)(uintptr_t)address,sizeof(*value));return 0;
}
#endif
