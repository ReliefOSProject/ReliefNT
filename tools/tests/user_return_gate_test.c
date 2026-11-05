#define _POSIX_C_SOURCE 200809L
#include <reliefnt/graphics_service.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <reliefnt/arch.h>
#include <reliefnt/console.h>
#include <reliefnt/input.h>
#include <reliefnt/sched.h>
#include <reliefnt/proc_stats.h>
#include <reliefnt/smp.h>
#include <reliefnt/net.h>
#include <reliefnt/usb.h>
#include <reliefnt/blkdev.h>
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/signal.h>
#include <reliefnt/userland.h>
#include <reliefnt/syscall.h>
#include <reliefnt/module.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/storage.h>
#include <reliefnt/swap.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/memory_pressure.h>
#include <reliefnt/time.h>
#include <reliefnt/framebuffer.h>
#include <reliefos/fb.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/fb.h>
#include <reliefnt/futex.h>
#include <reliefnt/fsnotify_vfs.h>
#include <linux/inotify.h>
#define RELIEFOS_FLOCK_NB 4u

/* Real execution admission, including queued-writer exclusion. Privileged
 * IRQ operations and loader/scheduler boundaries are the fixture's scope. */
#include "../../kernel/sync/lock.c"
static _Thread_local uint32_t cpu;
static _Thread_local uint64_t irq_state;
static bool network_due;
static bool network_stack_active;
static unsigned network_maintenance_calls;
static unsigned network_stack_maintenance_calls;
static unsigned network_stack_calls;
static unsigned pressure_calls;
#define SCHED_IDLE_SERVICE_STACK_BYTES (16u * 1024u)
static uint8_t sched_idle_service_stacks[SMP_MAX_CPUS]
    [SCHED_IDLE_SERVICE_STACK_BYTES] __attribute__((aligned(16)));
struct sched_idle_service_state { uint64_t execution_flags; };
static void test_idle_wait(void);
static bool exclusive(void) { return execution_owner==cpu && execution_depth[cpu]==1; }
static struct task current, replacement;
static struct task *selected;
static atomic_bool key_pending;
static unsigned drained, delivered, released, fpu_saved, fpu_restored;
static uint64_t installed_fs;
#ifdef CONFIG_SWAP
static struct task *staged_exec;
static unsigned exec_continuations, exec_cancellations, exec_exits, exec_backend_calls;
void task_socket_stage_enter(struct task *task,const struct trap_frame *frame)
{ (void)task;(void)frame; }
struct task *sched_exec_staged(const struct task *task) { return task==staged_exec ? staged_exec : NULL; }
int userland_exec_continue(struct task *task)
{ assert(exclusive() && task==staged_exec);++exec_continuations;task->frame.rip=0x900000;staged_exec=NULL;return 0; }
void userland_exec_cancel(struct task *task) { assert(task==staged_exec);staged_exec=NULL;++exec_cancellations; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{ (void)task;(void)address;(void)length;(void)writing;return 0; }
/** @brief Reject process_vm ownership, outside this scheduler-gate fixture.
 * @param state Unexpected private process_vm scope.
 */
void task_process_vm_scope_release(void *state) { (void)state;assert(false); }
#endif
static bool accounting_kernel;
void sched_account_mode(bool kernel)
{
    accounting_kernel = kernel;
}

uint32_t smp_current_cpu(void) { return cpu; }
bool net_tcp_maintenance_due(void)
{ assert(exclusive()); return network_due; }
/** @brief Model a completed pressure batch on the scheduler service stack.
 * @param budget Requested page budget from the real timer admission.
 */
void memory_pressure_poll(uint32_t budget)
{ assert(exclusive() && network_stack_active && budget == 16); ++pressure_calls; }
#ifdef CONFIG_SWAP
/** @brief Model an available swap transport for this return-gate fixture.
 * @return False; no storage transaction is active in the fixture.
 */
bool swap_storage_busy(void) { assert(exclusive()); return false; }
#endif
void net_tcp_maintenance(void)
{
    assert(exclusive());
    ++network_maintenance_calls;
    if (network_stack_active) ++network_stack_maintenance_calls;
    network_due = false;
}
/* Model the executor boundary here; the actual queue/pin state machine has
 * separate workqueue coverage. Actual USB registry and wait scopes are linked. */
bool kthread_runnable(void) {assert(exclusive());return network_due || usb_service_pending();}
void kernel_deferred_run(void) {assert(exclusive() && network_stack_active);net_tcp_maintenance();usb_process_work();}
void arch_call_on_stack(void *stack_top, void (*callback)(void *), void *argument)
{
    assert(exclusive());
    assert(cpu < SMP_MAX_CPUS &&
           stack_top == &sched_idle_service_stacks[cpu][SCHED_IDLE_SERVICE_STACK_BYTES]);
    ++network_stack_calls;
    network_stack_active = true;
    callback(argument);
    network_stack_active = false;
}
void smp_membarrier_poll(void) { }
uint64_t kernel_irq_save(void) { uint64_t saved=irq_state; irq_state=0; return saved; }
void kernel_irq_restore(uint64_t saved) { irq_state=saved; }
void input_process_pending(void)
{ assert(exclusive()); if (atomic_exchange(&key_pending, false)) ++drained; }
struct task *sched_current_task(void) { return &current; }
bool sched_try_resume_current_user_frame(const struct trap_frame *frame)
{
    assert(exclusive());
    if (!frame || (frame->cs&3)!=3 || current.state!=TASK_RUNNING ||
        current.flags&TASK_FLAG_PENDING_LOAD ||
        sched_task_pending(&current)&~current.blocked_signals) return false;
    current.frame=*frame;
    sched_account_mode(false);
    return true;
}
bool sched_capture_current_user_frame(const struct trap_frame *frame)
{ assert(exclusive()); current.frame=*frame; return true; }
struct task *sched_select_next_user(void) { assert(exclusive());
#ifdef CONFIG_SWAP
    if (staged_exec==&current && current.state==TASK_BLOCKED && selected==&current) return &replacement;
#endif
    return selected; }
struct task *sched_reclaim_current_user(void) { assert(exclusive()); return selected; }
void sched_quiesce_exited_current(void) { assert(exclusive()); }
void sched_release_task_resources(struct task *task)
{ assert(exclusive() && task==&current); ++released; }
void sched_cleanup_poll(uint32_t budget) { assert(exclusive() && budget==8); }
void sched_exit(uint32_t pid, uint64_t code) {
#ifdef CONFIG_SWAP
    if(pid==current.pid && code==127){++exec_exits;current.state=TASK_EXITED;return;}
#endif
    (void)pid;(void)code; assert(false);
}
int kernel_signal_deliver_pending_outer(struct task *task, struct trap_frame *frame)
{
    assert(exclusive());
    if (sched_task_pending(task)&~task->blocked_signals) {
        ++delivered; task->pending_signals=task->process_pending_signals=0;
        frame->rax=0x51a7;
    }
    return 0;
}
void arch_set_user_fs(uint64_t base)
{
    assert(!irq_state && exclusive());
    installed_fs=base;
}
void arch_fpu_save(void *state) { (void)state; assert(exclusive()); ++fpu_saved; }
void arch_fpu_restore(const void *state) { (void)state; assert(exclusive()); ++fpu_restored; }
uint64_t paging_kernel_cr3(void) { return 0x1000; }
void paging_load_cr3(uint64_t cr3) { assert(exclusive() && cr3==0x1000); }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level;(void)format; assert(exclusive()); }
static bool userland_load_task_image(struct task *task)
{ (void)task; assert(exclusive()); return true; }
static struct task *wait_for_runnable_task(uint64_t *flags)
{ (void)flags; assert(false); return NULL; }

/* The runner inserts the complete, unmodified production dispatch function.
 * The real scheduler's eligibility rules have separate sched_resume coverage. */
#include "network_maintenance_body.inc"
#include "network_maintenance_runner_body.inc"
#include "pressure_maintenance_body.inc"
#include "pressure_maintenance_runner_body.inc"
#include "idle_service_body.inc"
#include "user_return_entry_body.inc"
#include "user_return_body.inc"
#include "user_timer_return_body.inc"

static void reset(void)
{
    cpu=0; irq_state=0;
    assert(!execution_readers && execution_next_ticket==execution_serving_ticket);
    memset(&current,0,sizeof(current)); memset(&replacement,0,sizeof(replacement));
    current.pid=41;current.kind=TASK_KIND_USER;current.state=TASK_RUNNING;
    current.fs_base=0x1234000;
    current.frame=(struct trap_frame){.rip=0x4001000,.rsp=0x7fff000,.cs=0x23,.ss=0x1b};
    replacement.pid=42;replacement.kind=TASK_KIND_USER;replacement.state=TASK_RUNNING;
    replacement.fs_base=0x5678000;replacement.frame=current.frame;
    selected=&current;drained=delivered=released=fpu_saved=fpu_restored=0;
    network_due=false;network_stack_active=false;
    network_maintenance_calls=network_stack_maintenance_calls=network_stack_calls=0;
    pressure_calls=0;
    installed_fs=0;
#ifdef CONFIG_SWAP
    staged_exec=NULL;exec_continuations=exec_cancellations=exec_exits=exec_backend_calls=0;
#endif
    accounting_kernel = false;
    atomic_store(&key_pending,false);
}
static bool async_io, inject_input;
static int64_t backend_result;
static unsigned slept, io_releases;
static bool syscall_trace_task(const struct task *task) { (void)task; return false; }
static int task_begin_syscall_io(struct task *task, uint64_t nr, int32_t fd, uint64_t arg2)
{ (void)task; (void)nr; (void)fd; (void)arg2; assert(exclusive()); return 0; }
void *kernel_malloc(size_t size){return malloc(size);}
void kernel_free(void *pointer){free(pointer);}
void task_release_syscall_file(struct task *task) { assert(exclusive()); task_lookup_scope_cancel(task); }
static int syscall_eagain_is_nonblocking_device(struct task *task, uint64_t nr, uint64_t fd)
{ (void)task; (void)nr; (void)fd; return 0; }
uint32_t graphics_service_process_retirements(uint32_t budget)
{ (void)budget; assert(exclusive()); return 0; }
void driver_manager_process_audio_faults(void) { assert(exclusive()); }
void task_sysv_sem_cancel(struct task *task) { (void)task; assert(false); }
void task_sysv_msg_cancel(struct task *task) { (void)task; assert(false); }
void task_fifo_cancel(struct task *task) { (void)task; assert(false); }
uint32_t sched_current_pid(void) { return current.pid; }
void storage_release_task_io(uint32_t pid) { assert(exclusive() && pid==current.pid); ++io_releases; }
void storage_drain_task_io(uint32_t pid){assert(exclusive() && pid==current.pid);}
void kernel_wait_queue_remove(struct kernel_wait_queue *queue,struct task *task)
{(void)queue;(void)task;abort();} /* No device wait is published in this fixture. */
void storage_set_io_async_context(bool enabled) { assert(exclusive()); async_io=enabled; }
uint64_t time_ticks(void) { return 100; }
uint64_t time_uptime_ms(void)
{
    return 1000;
}
void sched_sleep_current_until(uint64_t tick)
{ assert(exclusive() && tick==101); current.state=TASK_BLOCKED; ++slept; }
int64_t kernel_signal_rt_sigreturn(struct task *task, struct trap_frame *frame)
{ assert(exclusive() && task==&current); frame->rax=0x5151; return 0; }
int64_t sched_clone_current(const struct trap_frame *frame, uint64_t flags,
    uint64_t stack, uint64_t parent, uint64_t child, uint64_t tls)
{ (void)frame;(void)flags;(void)stack;(void)parent;(void)child;(void)tls;assert(false);return 0; }
int64_t syscall_clone_prepared(const struct trap_frame *frame, uint64_t flags,
    uint64_t stack, uint64_t parent, uint64_t child, uint64_t tls)
{ (void)frame;(void)flags;(void)stack;(void)parent;(void)child;(void)tls;assert(false);return 0; }
int64_t syscall_clone3(const struct trap_frame *frame, uint64_t args, uint64_t size)
{ (void)frame;(void)args;(void)size;assert(false);return 0; }
int64_t sched_fork_current(const struct trap_frame *frame)
{ (void)frame;assert(false);return 0; }
int64_t sched_vfork_current(const struct trap_frame *frame)
{ (void)frame;assert(false);return 0; }
static int64_t backend(struct task *task, uint64_t nr, uint64_t a0,
    uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)nr;(void)a0;(void)a1;(void)a2;(void)a3;(void)a4;(void)a5;
    assert(exclusive() && async_io && task==&current);
#ifdef CONFIG_SWAP
    ++exec_backend_calls;
    if (backend_result==KERNEL_SYSCALL_EXEC_COMMIT_WAIT) staged_exec=task;
#endif
    assert(exclusive() && async_io && task == &current && accounting_kernel);
    if (inject_input) atomic_store(&key_pending,true);
    return backend_result;
}
/* Pipe reader admission is tested against real concurrent rings separately. */
static bool syscall_try_pipe_io(struct trap_frame *frame,struct task **resume)
{(void)frame;(void)resume;return false;}
#define RELIEFOS_SYSCALL_FRAME_DISPATCH backend
#include "syscall_return_body.inc"
#include "int80_return_body.inc"

static struct trap_frame prepare(uint64_t number)
{
    reset();async_io=inject_input=false;backend_result=123;slept=io_releases=0;
    struct trap_frame frame=current.frame;frame.rax=number;return frame;
}
static void test_one_transaction_for_completed_syscall(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    uint32_t before=execution_next_ticket;
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rax==123 && current.frame.rax==123);
    assert(!accounting_kernel);
    assert(installed_fs==current.fs_base && io_releases==1 && !async_io);
    assert(network_maintenance_calls==0 && network_stack_maintenance_calls==0 &&
           network_stack_calls==0);
    assert(!network_stack_active);
    assert(!fpu_saved && !fpu_restored && !irq_state);
    if (execution_next_ticket-before!=1)
        fputs("FAIL: completed syscall reacquired the execution transaction on user return\n",stderr);
    assert(execution_next_ticket-before==1);
}
#ifdef CONFIG_SWAP
/** @brief A committed exec continues at the selected-task gate, never old RIP. */
static void test_exec_continues_without_old_user_instruction(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_EXECVE);
    uint64_t old_rip=frame.rip;
    backend_result=KERNEL_SYSCALL_EXEC_COMMIT_WAIT;
    assert(int80_dispatch(&frame)==&replacement);
    assert(frame.rip==old_rip && current.frame.rip==old_rip && staged_exec==&current);
    assert(current.state==TASK_BLOCKED && slept==1 && !exec_continuations);
    current.state=TASK_READY;selected=&current;
    assert(userland_schedule_from_frame(NULL)==&current);
    assert(!staged_exec && exec_continuations==1 && current.frame.rip==0x900000);
}
/** @brief No syscall frame may resume a committed kernel-owned exec. */
static void test_committed_exec_rejects_unexpected_syscall(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_EXECVE);
    staged_exec=&current;
    assert(!syscall_dispatch_frame(&frame));
    assert(current.state==TASK_EXITED && !staged_exec);
    assert(exec_cancellations==1 && exec_exits==1 && !exec_continuations && !exec_backend_calls);
    assert(!current.lookup_scope.armed && !async_io);
}
#endif
static void test_timer_user_return_network_maintenance_stack(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    network_due=true;
    assert(userland_schedule_from_frame(&frame)==&current);
    assert(network_maintenance_calls==1 && network_stack_maintenance_calls==1);
    assert(network_stack_calls==1 && !network_stack_active);
    assert(!irq_state);
}
/** @brief Check that only the timer wrapper admits bounded pressure work. */
static void test_timer_pressure_entry(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    assert(userland_schedule_from_frame(&frame)==&current);
    assert(!pressure_calls && !network_stack_calls);
    assert(userland_schedule_from_timer(&frame)==&current);
    assert(pressure_calls==1 && network_stack_calls==1 && !network_stack_active);
    frame.cs=8;
    assert(!userland_schedule_from_timer(&frame));
    assert(pressure_calls==1 && network_stack_calls==1);
}
static struct usb_host *hotplug_host;
static struct block_device_registration hotplug_disk;
static unsigned hotplug_visible,hotplug_polls;
static bool polling_irq,idle_poll,hotplug_gate_busy;
static int fake_usb_read(void *context,uint64_t first,uint32_t count,void *buffer)
{(void)context;(void)first;(void)count;(void)buffer;return 0;}
static const struct block_device_operations hotplug_ops={.read=fake_usb_read};
static void hotplug_poll(void *context)
{
    assert(context==&hotplug_host && !hotplug_visible);
    polling_irq=true;++hotplug_polls;usb_host_request_service(hotplug_host);
    assert(!hotplug_visible);polling_irq=false;
}
static void hotplug_service(void *context)
{
    assert(context==&hotplug_host && !polling_irq && network_stack_active && !exclusive());
    assert(irq_state&(1u<<9));
    assert(driver_manager_phase_try_enter()==-EBUSY);
    if(hotplug_gate_busy) {usb_host_request_service(hotplug_host);return;}
    assert(!register_block_device(&hotplug_disk));++hotplug_visible;
}
static void test_idle_wait(void)
{
    assert(!exclusive() && idle_poll);
    irq_state|=1u<<9;usb_poll();irq_state=0;
    assert(!hotplug_visible && usb_service_pending());
}
static void make_hotplug(void)
{
    hotplug_visible=hotplug_polls=0;hotplug_host=NULL;hotplug_gate_busy=false;
    hotplug_disk=(struct block_device_registration){.info={.device=block_device_allocate_number(false),
        .blocks=100,.block_size=512,.max_blocks=1,.flags=BLOCK_DEVICE_READ_ONLY},.operations=&hotplug_ops};
    assert(!usb_host_register_service(hotplug_poll,hotplug_service,&hotplug_host,NULL,&hotplug_host));
}
static void test_usb_work_without_syscall(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);make_hotplug();
    usb_poll();assert(hotplug_polls==1 && !hotplug_visible && usb_service_pending());
    assert(!driver_manager_phase_try_enter());
    assert(userland_schedule_from_frame(&frame)==&current);
    assert(!hotplug_visible && usb_service_pending());driver_manager_phase_leave();
    hotplug_gate_busy=true;assert(userland_schedule_from_frame(&frame)==&current);
    assert(!hotplug_visible && usb_service_pending());hotplug_gate_busy=false;
    assert(userland_schedule_from_frame(&frame)==&current);
    struct block_device_ref ref={0};
    assert(hotplug_visible==1 && !usb_service_pending() && !block_device_get(hotplug_disk.info.device,&ref));
    block_device_put(&ref);usb_host_request_service(hotplug_host);
    assert(!usb_host_unregister(hotplug_host));hotplug_host=NULL;
    assert(userland_schedule_from_frame(&frame)==&current);
    assert(hotplug_visible==1 && !usb_service_pending());
    assert(!unregister_block_device(&hotplug_disk));
}
static void test_idle_usb_work_without_syscall(void)
{
    reset();make_hotplug();idle_poll=true;uint64_t flags;
    kernel_execution_lock_irqsave(&flags);
    struct sched_idle_service_state state={.execution_flags=flags};
    arch_call_on_stack(&sched_idle_service_stacks[cpu][SCHED_IDLE_SERVICE_STACK_BYTES],
                       sched_idle_service_wait,&state);
    assert(exclusive());kernel_execution_unlock_irqrestore(state.execution_flags);idle_poll=false;
    struct block_device_ref ref={0};
    assert(hotplug_visible==1 && !usb_service_pending() && !block_device_get(hotplug_disk.info.device,&ref));
    block_device_put(&ref);assert(!usb_host_unregister(hotplug_host));hotplug_host=NULL;
    assert(!unregister_block_device(&hotplug_disk));
}
static void test_signal_input_and_retry(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_GETPID);
    current.pending_signals=1ULL<<5;
    assert(int80_dispatch(&frame)==&current);
    assert(delivered==1 && fpu_saved==1 && current.frame.rax==0x51a7);
    frame=prepare(LINUX_SYS_GETPID);inject_input=true;
    assert(int80_dispatch(&frame)==&current);
    assert(drained==1 && !atomic_load(&key_pending));
    frame=prepare(LINUX_SYS_GETPID);backend_result=-RELIEFOS_EAGAIN;
    selected=&replacement;uint64_t rip=frame.rip;
    assert(int80_dispatch(&frame)==&replacement);
    assert(frame.rax==LINUX_SYS_GETPID && frame.rip==rip-2 && slept==1);
    assert(current.restart_syscall==LINUX_SYS_GETPID+1 && !io_releases && !async_io);
    for(unsigned i=0;i<2;++i) {
        uint64_t number=i ? LINUX_SYS_SWAPOFF : LINUX_SYS_SWAPON;
        frame=prepare(number);backend_result=-RELIEFOS_EAGAIN;selected=&replacement;rip=frame.rip;
        assert(int80_dispatch(&frame)==&current);
        assert((int64_t)frame.rax==-RELIEFOS_EAGAIN && frame.rip==rip && !slept);
        assert(!current.restart_syscall && io_releases==1 && !async_io);
    }
}
static void test_restored_context_and_fallback(void)
{
    struct trap_frame frame=prepare(LINUX_SYS_RT_SIGRETURN);
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rax==0x5151 && current.frame.rax==0x5151);
    frame=prepare(LINUX_SYS_EXECVE);backend_result=0;
    current.frame.rip+=0x100;current.frame.error=1;current.frame.rax=0x6161;
    assert(int80_dispatch(&frame)==&current);
    assert(frame.rip==current.frame.rip && !frame.error && frame.rax==0x6161);
    frame=prepare(LINUX_SYS_GETPID);current.state=TASK_EXITED;selected=&replacement;
    assert(int80_dispatch(&frame)==&replacement);
    assert(released==1 && fpu_restored==1);
    frame=prepare(LINUX_SYS_GETPID);frame.cs=8;
    assert(!int80_dispatch(&frame));
}
int main(void)
{
#ifdef CONFIG_SWAP
    test_exec_continues_without_old_user_instruction();
    test_committed_exec_rejects_unexpected_syscall();
#endif
    test_one_transaction_for_completed_syscall();
    test_timer_user_return_network_maintenance_stack();
    test_timer_pressure_entry();
    test_idle_usb_work_without_syscall();
    test_usb_work_without_syscall();
    test_signal_input_and_retry();
    test_restored_context_and_fallback();
    puts("PASS syscall user return: one transaction, result/TLS, signal, deferred input, retry, exec/sigreturn and retirement");
}

/* These socket/return fixtures never create an executable guard. Scope
 * cancellation must not cross this absent inode backend with a live guard. */
void storage_inode_allow_write(struct storage_inode_ref *inode){assert(!inode);}
int storage_inode_put(struct storage_inode_ref *inode){assert(!inode);return 0;}

/* No copy transaction is created in the return-gate-only fixture. */
void task_transfer_enter(struct task *task,const struct trap_frame *frame)
{(void)frame;assert(!task || !task->transfer_io);}
