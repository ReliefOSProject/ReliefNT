/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/kthread.h>
#include <reliefnt/console.h>
#include <reliefnt/module.h>
#include <reliefnt/moduleparam.h>
#include <reliefnt/time.h>
#include <reliefnt/usb.h>
#include <linux/errno.h>
static struct kthread *spinner,*sleeper,*cold;
static struct usb_host *source;
static bool active,sleeping,going_seen,kicked;
static unsigned errors,wakes;
module_param(active,bool,0444);
module_param(sleeping,bool,0444);
module_param(going_seen,bool,0444);
module_param(errors,uint,0444);
module_param(wakes,uint,0444);
/** @brief Burn CPU without yielding so same-CPU user progress proves preemption.
 * @param context Unused NULL. @return 23 after cooperative stop.
 * The finite timeout records failure if a user cannot request stop within 5 s.
 */
static int spin_entry(void *context)
{
    (void)context;volatile uint64_t retained=0x7acde123456789abULL;
    __atomic_store_n(&active,true,__ATOMIC_RELEASE);
    uint64_t start=time_ticks(),observed=UINT64_MAX;
    while(!kthread_should_stop() && time_ticks()-start<500) {
        if(time_ticks()!=observed) {
            observed=time_ticks();
            if(try_module_get(THIS_MODULE))module_put(THIS_MODULE);else going_seen=true;
        }
        __asm__ volatile("pause" ::: "memory");
    }
    if(!kthread_should_stop() || retained!=0x7acde123456789abULL)++errors;
    if(try_module_get(THIS_MODULE))module_put(THIS_MODULE);else going_seen=true;
    __atomic_store_n(&active,false,__ATOMIC_RELEASE);
    return 23;
}
/** @brief Observe an actual timed sleep before the unload wakes an event sleeper.
 * @param context Unused NULL. @return 29 after cooperative stop.
 */
static int sleep_entry(void *context)
{
    (void)context;uint64_t start=time_ticks();sleeping=true;
    if(kthread_sleep_until(start+3))++errors;
    if(time_ticks()-start<3 && !kthread_should_stop())++errors;
    ++wakes;
    while(!kthread_should_stop())if(kthread_sleep_until(UINT64_MAX))++errors;
    sleeping=false;return 29;
}
/** @brief A cold stopped thread must never enter its module text.
 * @param context Unused NULL. @return Failure marker if incorrectly called.
 */
static int cold_entry(void *context){(void)context;++errors;return 1;}
/** @brief Admit both module functions from a pinned IRQ only after LIVE.
 * @param context Unused NULL; the USB registry protects this poll callback.
 */
static void kick(void *context)
{
    (void)context;if(kicked)return;
    bool a=kthread_wake(spinner),b=kthread_wake(sleeper);if(a && b)kicked=true;
}
/** @brief Create CPU-0 threads and an IRQ source, with ledger-owned rollback.
 * @return Zero or a native registration/allocation error.
 */
static int probe_init(void)
{
    int rc=kthread_create(spin_entry,NULL,"ktprobe-spin",THIS_MODULE,0,&spinner);
    if(!rc)rc=kthread_create(sleep_entry,NULL,"ktprobe-sleep",THIS_MODULE,0,&sleeper);
    if(!rc)rc=kthread_create(cold_entry,NULL,"ktprobe-cold",THIS_MODULE,0,&cold);
    if(!rc)rc=usb_host_register(kick,NULL,THIS_MODULE,&source);
    return rc;
}
/** @brief Verify all module entries and their stacks have retired before release. */
static void probe_exit(void)
{
    int a=0,b=0,c=0;
    if(usb_host_unregister(source))++errors;
    if(kthread_join(spinner,&a) || a!=23)++errors;
    if(kthread_join(sleeper,&b) || b!=29)++errors;
    if(kthread_join(cold,&c) || c!=-EINTR)++errors;
    console_info("[kthread-probe] EXIT active=%u sleeping=%u wakes=%u errors=%u going_seen=%u result=%d,%d,%d\n",
        active ? 1u : 0u,sleeping ? 1u : 0u,wakes,errors,going_seen ? 1u : 0u,a,b,c);
}
module_init(probe_init);
module_exit(probe_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("Kernel-thread timer preemption, sleep, stop and module drain probe");
