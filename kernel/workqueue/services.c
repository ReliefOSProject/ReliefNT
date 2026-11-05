/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/deferred.h>
#include <reliefnt/workqueue.h>
#include <reliefnt/usb.h>
#include <reliefnt/mount.h>
#include <reliefnt/graphics_service.h>
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/net.h>
#include <reliefnt/input.h>
#include <reliefnt/export.h>
#include <reliefnt/kthread.h>
#include <reliefnt/console.h>
#include <reliefnt/kernel.h>
#include <reliefnt/sched.h>

/** @brief Poll protocol timers/NIC work once per tick using existing budgets.
 * @param work Permanent protocol work. @param context Unused NULL.
 * Poll-only NICs and IPv6 discovery need this heartbeat without syscalls.
 */
static void network_work(struct kernel_work *work,void *context)
{ (void)context;net_tcp_maintenance();(void)kernel_queue_delayed_work(work,1); }
/** @brief Service coalesced USB hosts, retrying busy manager admission later.
 * @param work Permanent USB work. @param context Unused NULL.
 */
static void usb_work(struct kernel_work *work,void *context)
{
    (void)context;usb_process_work();
    if(usb_service_pending())(void)kernel_queue_delayed_work(work,1);
}
/** @brief Retry bounded detached-mount cleanup while preserving failed owners.
 * @param work Permanent VFS work. @param context Unused NULL.
 */
static void vfs_work(struct kernel_work *work,void *context)
{
    (void)context;(void)vfs_reclaim_mounts(2);
    if(vfs_reclaim_pending())(void)kernel_queue_delayed_work(work,1);
}
/** @brief Service graphics fences/retirement at the existing tick cadence.
 * @param work Permanent dispatch work. @param context Unused NULL.
 * Service acquisition retains actual optional code only across its callback;
 * a queued permanent dispatcher never keeps an absent provider alive.
 */
static void graphics_work(struct kernel_work *work,void *context)
{
    (void)context;(void)graphics_service_process_retirements(2);
    if(graphics_service_present())(void)kernel_queue_delayed_work(work,1);
}
/** @brief Drain audio work with the existing admission and task-work budget.
 * @param work Unused permanent work. @param context Unused NULL.
 */
static void audio_work(struct kernel_work *work,void *context)
{ (void)work;(void)context;driver_manager_process_audio_faults(); }
static struct kernel_work network=KERNEL_WORK_INIT(network_work,NULL);
static struct kernel_work services[]={
    KERNEL_WORK_INIT(usb_work,NULL),KERNEL_WORK_INIT(vfs_work,NULL),
    KERNEL_WORK_INIT(graphics_work,NULL),KERNEL_WORK_INIT(audio_work,NULL)};
/** @brief Coalesce a builtin dispatcher while preserving subsystem ownership.
 * @param service Valid permanent service index. IRQ/task safe, no callback.
 */
void kernel_deferred_request(enum kernel_deferred_service service)
{ if((unsigned)service<sizeof(services)/sizeof(services[0]))(void)kernel_queue_work(&services[service]); }
/** @brief Start the network heartbeat once; duplicate starts coalesce. */
void kernel_deferred_start(void)
{
    int rc=kernel_workqueue_start_workers();
    if(rc){console_fatal("[kthread] worker pool creation failed rc=%d\n",rc);kernel_idle_loop();}
    (void)kernel_queue_work(&network);
}
/** @brief Dispatch one schedulable kernel turn with short input checks. */
void kernel_deferred_run(void)
{
    input_process_pending();
    struct task *caller=sched_current_task();
    if(caller && caller->kind==TASK_KIND_USER)(void)kthread_dispatch_user(1);
    else (void)kthread_dispatch(1);
    input_process_pending();
}
EXPORT_SYMBOL(kernel_deferred_request);
