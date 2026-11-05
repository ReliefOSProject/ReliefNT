/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/module.h>
#include <reliefnt/moduleparam.h>
#include <reliefnt/time.h>
#include <reliefnt/usb.h>
#include <reliefnt/workqueue.h>
#include <linux/errno.h>
static struct kernel_work *job;
static struct usb_host *source;
static bool active,started,going_seen;
static unsigned calls,errors;
module_param(active,bool,0444);
module_param(calls,uint,0444);
module_param(errors,uint,0444);
module_param(going_seen,bool,0444);

/** @brief Exercise a cancellable execution pin while another CPU unloads us.
 * @param work Owned registered work. @param context Unused NULL.
 * The intentional 50 ms QA phase holds no execution/queue/driver lock; the
 * core must retain our image until resume and callback completion.
 */
static void probe_work(struct kernel_work *work,void *context)
{
    (void)context;
    if(kernel_work_cancel_sync(work)!=-EDEADLK || kernel_work_flush(work)!=-EDEADLK)++errors;
    struct kernel_execution_suspend_token token={0};
    if(kernel_execution_suspend_irqrestore(&token)) {++errors;return;}
    __atomic_store_n(&active,true,__ATOMIC_RELEASE);
    uint64_t start=time_ticks(),observed=UINT64_MAX;
    while(time_ticks()-start<5) {
        if(time_ticks()!=observed) {
            observed=time_ticks();
            if(try_module_get(THIS_MODULE))module_put(THIS_MODULE);
            else going_seen=true;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    if(try_module_get(THIS_MODULE))module_put(THIS_MODULE);else going_seen=true;
    if(kernel_execution_resume_irqsave(&token)) {++errors;return;}
    __atomic_store_n(&active,false,__ATOMIC_RELEASE);++calls;
    if(calls==1)console_info("[workqueue-probe] callback completed self-wait checks errors=%u\n",errors);
    (void)kernel_queue_delayed_work(work,1);
}
/** @brief Kick work from an IRQ registration only after the module is LIVE.
 * @param context Unused NULL; core USB polling owns the module code pin.
 */
static void probe_poll(void *context)
{
    (void)context;
    if(!started && kernel_queue_work(job))started=true;
}
/** @brief Register ledger-owned work and a timer-driven callback source.
 * @return Zero or registration error; failed init is cleaned by the ledger.
 */
static int probe_init(void)
{
    int rc=kernel_work_create(probe_work,NULL,THIS_MODULE,&job);
    if(!rc)rc=usb_host_register(probe_poll,NULL,THIS_MODULE,&source);
    return rc;
}
/** @brief Remove the callback source after the module ledger has cancelled work. */
static void probe_exit(void)
{
    if(usb_host_unregister(source))++errors;
    console_info("[workqueue-probe] EXIT calls=%u active=%u errors=%u going_seen=%u\n",
        calls,active ? 1u : 0u,errors,going_seen ? 1u : 0u);
}
module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("Deferred work execution, cancellation and module-unload probe");
