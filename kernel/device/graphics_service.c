/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/graphics_service.h>
#include <reliefnt/export.h>
#include <reliefnt/deferred.h>
#include <reliefnt/framebuffer.h>
#include <linux/errno.h>
static struct kernel_service_slot graphics_service_slot;
/** @brief Keep maintenance queued while a provider initializes or owns the slot.
 * @return Core binding presence without entering optional code.
 */
bool graphics_service_present(void) {return kernel_service_present(&graphics_service_slot);}
/** @brief Publish one graphics table without taking a permanent self-reference.
 * @param provider Persistent immutable table and owner descriptor.
 * @return Zero or generic registration errno. Init/task context; the core ledger
 * detaches/drains before code release. No lock spans allocation or callbacks.
 */
int graphics_service_register(const struct kernel_service_descriptor *provider)
{
    int rc=kernel_service_register(&graphics_service_slot,provider);
    if(!rc)kernel_deferred_schedule(DEFERRED_GRAPHICS);
    return rc;
}
/** @brief Detach one idle exact graphics table without entering its code.
 * @param provider Exact persistent registration identity.
 * @return Zero or ENOENT/EBUSY/EINVAL. Task context; active calls preserve binding.
 */
int graphics_service_unregister(const struct kernel_service_descriptor *provider)
{return kernel_service_unregister(&graphics_service_slot,provider);}
/** @brief Report whether a graphics provider owns a discovered primary node.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_is_registered(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return false;
    const struct graphics_service_operations *provider_ops=call.operations;
    bool result=provider_ops->is_registered ? provider_ops->is_registered() : false;
    kernel_service_release(&call);return result;
}
/** @brief Report whether the retained provider can expose its render node.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_render_node_ready(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return false;
    const struct graphics_service_operations *provider_ops=call.operations;
    bool result=provider_ops->render_node_ready ? provider_ops->render_node_ready() : false;
    kernel_service_release(&call);return result;
}
/** @brief Copy the admitted primary identity without retaining provider data.
 * @param out Required writable identity; untouched when no provider exists.
 * @return Backend result or -ENODEV when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
int graphics_service_pci_identity(struct pci_device *out)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return -ENODEV;
    const struct graphics_service_operations *provider_ops=call.operations;
    int result=provider_ops->pci_identity ? provider_ops->pci_identity(out) : -ENODEV;
    kernel_service_release(&call);return result;
}
/** @brief Snapshot whether the provider currently owns the KMS scanout.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_kms_scanout_active(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return false;
    const struct graphics_service_operations *provider_ops=call.operations;
    bool result=provider_ops->kms_scanout_active ? provider_ops->kms_scanout_active() : false;
    kernel_service_release(&call);return result;
}
/** @brief Release KMS ownership through the admitted provider during VT handoff.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
void graphics_service_kms_vt_release(void)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return;
    const struct graphics_service_operations *provider_ops=call.operations;
    if(provider_ops->kms_vt_release)provider_ops->kms_vt_release();
    kernel_service_release(&call);
}
/** @brief Run bounded graphics retirement work under the exact provider pin.
 * @param budget Maximum work requested; provider applies its existing bounds.
 * @return Backend result or 0 when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
uint32_t graphics_service_process_retirements(uint32_t budget)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return 0;
    const struct graphics_service_operations *provider_ops=call.operations;
    uint32_t result=provider_ops->process_retirements ? provider_ops->process_retirements(budget) : 0;
    kernel_service_release(&call);return result;
}
/** @brief Cancel optional graphics syscall continuation ownership for one task.
 * @param task Execution-owned task, nullable; core never dereferences optional state.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
void graphics_service_cancel_task_continuation(struct task *task)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return;
    const struct graphics_service_operations *provider_ops=call.operations;
    if(provider_ops->cancel_task_continuation)provider_ops->cancel_task_continuation(task);
    kernel_service_release(&call);
}
/** @brief Query optional graphics continuation signal-deferral policy.
 * @param task Execution-owned task, nullable.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_task_uninterruptible(const struct task *task)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return false;
    const struct graphics_service_operations *provider_ops=call.operations;
    bool result=provider_ops->task_uninterruptible ? provider_ops->task_uninterruptible(task) : false;
    kernel_service_release(&call);return result;
}
/** @brief Query optional graphics CPU synchronization signal policy.
 * @param task Execution-owned task, nullable.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_task_signal_busy(const struct task *task)
{
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return false;
    const struct graphics_service_operations *provider_ops=call.operations;
    bool result=provider_ops->task_signal_busy ? provider_ops->task_signal_busy(task) : false;
    kernel_service_release(&call);return result;
}
EXPORT_SYMBOL(graphics_service_register);
EXPORT_SYMBOL(graphics_service_unregister);

/** @brief Copy current connector metadata while the exact graphics owner is pinned.
 * @param index Zero-based output. @param out Required permanent caller storage.
 * @return Zero or errno. Task/execution context; generic single-output providers
 * retain their existing firmware mode without exposing optional pointers.
 */
int graphics_service_connector_info(uint32_t index,struct graphics_connector_info *out)
{
    if(!out)return -EINVAL;
    struct kernel_service_call call={0};
    if(!kernel_service_acquire(&graphics_service_slot,&call))return -ENODEV;
    const struct graphics_service_operations *ops=call.operations;
    *out=(struct graphics_connector_info){0};
    int rc;
    if(ops->connector_info)rc=ops->connector_info(index,out);
    else if(index)rc=-ENOENT;
    else {
        const struct framebuffer *fb=framebuffer_get();
        out->connected=fb->available;
        out->enabled=ops->kms_scanout_active && ops->kms_scanout_active();
        if(fb->available){out->mode_count=1;out->widths[0]=fb->width;out->heights[0]=fb->height;}
        rc=0;
    }
    kernel_service_release(&call);return rc;
}

/** @brief Select a retained panic target after all ordinary CPUs have parked.
 * Fatal context only; never acquires a service/module lock or returns to tasks.
 */
void graphics_service_panic_prepare(void)
{
    const struct graphics_service_operations *ops=kernel_service_panic_operations(&graphics_service_slot);
    if(ops && ops->panic_prepare)ops->panic_prepare();
}
/** @brief Copy only already-rendered fatal text cells into retained scanout backing.
 * @param x Left. @param y Top. @param width Painted pixels. @param height Rows.
 * Fatal owner only after other CPUs stop; no normal registry admission or locks.
 */
void graphics_service_panic_damage(uint32_t x,uint32_t y,uint32_t width,uint32_t height)
{
    const struct graphics_service_operations *ops=kernel_service_panic_operations(&graphics_service_slot);
    if(ops && ops->panic_damage)ops->panic_damage(x,y,width,height);
}
/** @brief Submit preallocated fatal damage without entering normal display locks.
 * Fatal owner only after other CPUs stop; no references escape or tasks resume.
 */
void graphics_service_panic_present(void)
{
    const struct graphics_service_operations *ops=kernel_service_panic_operations(&graphics_service_slot);
    if(ops && ops->panic_present)ops->panic_present();
}
