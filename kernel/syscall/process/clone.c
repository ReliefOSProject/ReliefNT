#include <reliefnt/syscall.h>
#include <reliefnt/sched.h>
#include <reliefnt/arch.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/paging.h>
#include <reliefnt/lock.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/heap.h>
#include <linux/sched.h>
#include <linux/errno.h>

/** @brief Prepare a single clone operand only at an eligible outer boundary.
 * @param task Live calling task. @param address User address.
 * @param size Byte count. @param writing True for a TID output.
 * @return Zero, EFAULT or a private prerequisite-contention marker. Nested
 * callers do no slot I/O and retain the original checked-usercopy behavior.
 */
static int clone_prepare_operand(struct task *task,uint64_t address,uint64_t size,bool writing)
{
#ifdef CONFIG_SWAP
    if(task && kernel_execution_is_outermost()) {
        int rc=swap_prepare_user_range(task,address,size,writing);
        if(rc==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(rc<0)return -LINUX_EFAULT;
    }
#else
    (void)task;(void)address;(void)size;(void)writing;
#endif
    return 0;
}

/** @brief Prepare and temporarily lease clone's actual TID output pages.
 * @param frame Saved parent registers. @param flags Linux clone flags.
 * @param stack Optional child stack. @param parent_tid Parent TID output.
 * @param child_tid Child TID output. @param tls Requested child TLS.
 * @return Child PID, errno, or the private prerequisite-contention marker.
 * The execution writer serializes the temporary parent-page leases with fork;
 * preparation happens before scheduler_lock and every lease ends on return.
 */
int64_t syscall_clone_prepared(const struct trap_frame *frame,uint64_t flags,
                               uint64_t stack,uint64_t parent_tid,
                               uint64_t child_tid,uint64_t tls)
{
    int rc=sched_clone_validate_flags(flags,tls);if(rc)return rc;
    struct task *task=sched_current_task();
    if(flags&CLONE_PARENT_SETTID) {
        rc=clone_prepare_operand(task,parent_tid,4,true);if(rc)return rc;
        if(!user_range_writable(parent_tid,4))return -LINUX_EFAULT;
    }
    if(flags&CLONE_CHILD_SETTID) {
        rc=clone_prepare_operand(task,child_tid,4,true);if(rc)return rc;
        if(!user_range_writable(child_tid,4))return -LINUX_EFAULT;
    }
    if(stack && (stack<RELIEFNT_USER_BASE || stack>=RELIEFNT_USER_TOP))return -LINUX_EFAULT;

    /* The scheduler writes parent_tid after creating a private COW child.
     * Register the already-admitted parent pages for this synchronous call so
     * clone copies the child instead of write-protecting the parent's store.
     * No pointer or lease survives return, including allocation failures. */
    struct paging_writable_alias leases[2]={{0}};
    unsigned count=0;
    if(task && frame && (flags&CLONE_PARENT_SETTID)) {
        uint64_t endpoints[]={parent_tid,parent_tid+3};
        for(unsigned i=0;i<2;++i) {
            uint64_t physical=address_space_user_page_phys(sched_task_as(task),endpoints[i]);
            if(physical && (!count || leases[0].physical!=physical)) {
                paging_writable_alias_add(&leases[count],physical);++count;
            }
        }
    }
    int64_t result=sched_clone_current(frame,flags,stack,parent_tid,child_tid,tls);
    while(count)paging_writable_alias_remove(&leases[--count]);
    return result;
}

/**
 * @brief Decode Linux 6.12 clone_args and delegate validated process creation.
 * @param frame Parent syscall frame; the scheduler supplies the child's zero return.
 * @param arguments User pointer to versioned clone_args with zero-only extensions.
 * @param size Bytes in the supplied structure, bounded by one page.
 * @return Child TID, Linux validation errno, or ENOSYS for missing backend features.
 */
int64_t syscall_clone3(const struct trap_frame *frame, uint64_t arguments, uint64_t size)
{
    if (size > 4096) return -LINUX_E2BIG;
    if (size < CLONE_ARGS_SIZE_VER0) return -LINUX_EINVAL;
    struct task *task=sched_current_task();
    const unsigned char *bytes=(const unsigned char *)(uintptr_t)arguments;
#ifdef CONFIG_SWAP
    struct task_user_text snapshot={0};
    if(task && task->lookup_scope.armed && kernel_execution_is_outermost()) {
        int rc=task_lookup_user_bytes(task,arguments,(uint32_t)size,&snapshot);
        if(rc)return rc;
        bytes=(const unsigned char *)snapshot.text;
    } else
#endif
    {
        int rc=clone_prepare_operand(task,arguments,size,false);if(rc)return rc;
        if(!user_range_ok(arguments,size))return -LINUX_EFAULT;
    }
    struct clone_args args = {0};
    uint64_t known_size = size < sizeof(args) ? size : sizeof(args);
    __builtin_memcpy(&args, bytes, known_size);
    for (uint64_t i = sizeof(args); i < size; ++i)
        if (bytes[i]) return -LINUX_E2BIG;

    if (args.set_tid_size > 32 || (!args.set_tid && args.set_tid_size) ||
        (args.set_tid && !args.set_tid_size)) return -LINUX_EINVAL;
    if (args.exit_signal > 64) return -LINUX_EINVAL;
    if ((args.flags & CLONE_INTO_CGROUP) &&
        (args.cgroup > INT32_MAX || size < CLONE_ARGS_SIZE_VER2)) return -LINUX_EINVAL;
    if (args.set_tid) {
        int rc=clone_prepare_operand(task,args.set_tid,args.set_tid_size*sizeof(uint32_t),false);
        if(rc)return rc;
        if(!user_range_ok(args.set_tid,args.set_tid_size*sizeof(uint32_t)))return -LINUX_EFAULT;
    }
    if (args.flags & ~(0xffffffffULL | CLONE_CLEAR_SIGHAND | CLONE_INTO_CGROUP))
        return -LINUX_EINVAL;
    if (args.flags & (CLONE_DETACHED | (CSIGNAL & ~CLONE_NEWTIME))) return -LINUX_EINVAL;
    if ((args.flags & CLONE_SIGHAND) && (args.flags & CLONE_CLEAR_SIGHAND)) return -LINUX_EINVAL;
    if ((args.flags & (CLONE_THREAD | CLONE_PARENT)) && args.exit_signal) return -LINUX_EINVAL;
    if (!args.stack) {
        if (args.stack_size) return -LINUX_EINVAL;
    } else if (!args.stack_size || args.stack >= RELIEFNT_USER_TLS_LIMIT ||
               args.stack_size > RELIEFNT_USER_TLS_LIMIT - args.stack) return -LINUX_EINVAL;

    /* Do not fold CLONE_NEWTIME into clone's low-byte exit signal. */
    if (args.set_tid_size || (args.flags & (CLONE_NEWTIME | CLONE_INTO_CGROUP)))
        return -LINUX_ENOSYS;
    return syscall_clone_prepared(frame, args.flags | args.exit_signal,
                               args.stack ? args.stack + args.stack_size : 0,
                               args.parent_tid, args.child_tid, args.tls);
}
