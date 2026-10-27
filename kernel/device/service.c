/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/service.h>
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <linux/errno.h>

struct kernel_service_binding {
    struct kernel_service_slot *slot;
    const struct kernel_service_descriptor *descriptor;
    uint64_t references,users;
    bool linked,active;
};
static struct kernel_spinlock service_binding_lock=KERNEL_SPINLOCK_INIT;
/** @brief Consume an internal binding reference after dropping registry locks.
 * @param binding Owned construction/list/ledger/call reference.
 * IRQ/task safe; final core heap free occurs outside registry/module locks.
 */
static void service_binding_put(struct kernel_service_binding *binding)
{
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);
    bool final=!--binding->references;
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    if(final)kernel_free(binding);
}
/** @brief Stop exact provider admission once all admitted callbacks return.
 * @param value Stable construction/ledger-owned core binding.
 * @return Zero or EBUSY. Task context; idempotent after prior detach. No
 * provider callback, hardware operation or allocation spans the service lock.
 */
static int service_binding_stop(void *value)
{
    struct kernel_service_binding *binding=value;bool detached=false;
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);
    int rc=binding->users ? -EBUSY : 0;
    if(!rc && binding->linked) {
        if(binding->slot->binding==binding)binding->slot->binding=NULL;
        binding->linked=false;binding->active=false;detached=true;
    }
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    if(detached)service_binding_put(binding);return rc;
}
/** @brief Consume a fully stopped module cleanup-ledger binding reference.
 * @param value Owned ledger reference, consumed once.
 * Task context outside registry locks; never enters optional provider code.
 */
static void service_binding_release(void *value)
{service_binding_put(value);}
/** @brief Reserve a core slot and attach its original owner cleanup obligation.
 * @param slot Permanent core slot. @param descriptor Persistent immutable provider.
 * @return Zero or negative errno with complete rollback of unpublished state.
 * Init/task context; construction keeps the binding alive across ledger setup,
 * and no service/module lock spans allocation or resource-ledger insertion.
 */
int kernel_service_register(struct kernel_service_slot *slot,const struct kernel_service_descriptor *descriptor)
{
    if(!slot || !descriptor || !descriptor->operations)return -EINVAL;
    struct kernel_service_binding *binding=kernel_malloc(sizeof(*binding));if(!binding)return -ENOMEM;
    *binding=(struct kernel_service_binding){.slot=slot,.descriptor=descriptor,.references=1};
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);
    if(slot->binding){kernel_spin_unlock_irqrestore(&service_binding_lock,flags);kernel_free(binding);return -EEXIST;}
    slot->binding=binding;binding->linked=true;++binding->references;
    if(descriptor->owner)++binding->references;
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    int rc=descriptor->owner ? module_add_resource(descriptor->owner,service_binding_stop,service_binding_release,binding) : 0;
    if(rc){service_binding_put(binding);(void)service_binding_stop(binding);}
    else {
        kernel_spin_lock_irqsave(&service_binding_lock,&flags);
        if(binding->linked)binding->active=true;else rc=-EBUSY;
        kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    }
    service_binding_put(binding);return rc;
}
/** @brief Detach an idle exact table without rebinding a former provider's token.
 * @param slot Permanent core slot. @param descriptor Exact immutable identity.
 * @return Zero or EINVAL/ENOENT/EBUSY. Task context; temporary binding reference
 * protects concurrent detach, and no provider callback or allocation occurs.
 */
int kernel_service_unregister(struct kernel_service_slot *slot,const struct kernel_service_descriptor *descriptor)
{
    if(!slot || !descriptor)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);
    struct kernel_service_binding *binding=slot->binding;
    if(binding && binding->descriptor==descriptor && binding->references<UINT64_MAX)++binding->references;
    else binding=NULL;
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    if(!binding)return -ENOENT;
    int rc=service_binding_stop(binding);service_binding_put(binding);return rc;
}
/** @brief Admit a callback only after pinning the original provider's code.
 * @param slot Permanent core slot, nullable. @param call Empty owned output.
 * @return True on admission, false with no new ownership. IRQ/task safe; no
 * heap allocation or callback. Nested service->module lock order only protects
 * immutable pointer/counter snapshots; neither lock spans provider execution.
 * A user pin precedes the execution pin so a callback makes delete_module
 * return EWOULDBLOCK without stranding an otherwise healthy provider in GOING.
 */
bool kernel_service_acquire(struct kernel_service_slot *slot,struct kernel_service_call *call)
{
    if(!call || call->binding)return false;call->operations=NULL;if(!slot)return false;
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);
    struct kernel_service_binding *binding=slot->binding;
    struct module *owner=binding ? binding->descriptor->owner : NULL;
    bool available=binding && binding->active && binding->users<UINT64_MAX && binding->references<UINT64_MAX &&
        try_module_get(owner);
    if(available && !module_execution_get(owner)){module_put(owner);available=false;}
    if(available) {
        ++binding->users;++binding->references;
        call->binding=binding;call->operations=binding->descriptor->operations;
    }
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);return available;
}
/** @brief Consume one returned callback's binding/user/execution reference set.
 * @param call Owned admitted record, nullable; cleared before any final put.
 * IRQ/task safe without callbacks or waits. Owner is captured before releasing
 * the core binding. The execution pin drops before the final user pin, and no
 * descriptor/owner is read after the final put can permit code reclamation.
 */
void kernel_service_release(struct kernel_service_call *call)
{
    if(!call || !call->binding)return;
    struct kernel_service_binding *binding=call->binding;*call=(struct kernel_service_call){0};
    struct module *owner=binding->descriptor->owner;
    uint64_t flags;kernel_spin_lock_irqsave(&service_binding_lock,&flags);--binding->users;
    kernel_spin_unlock_irqrestore(&service_binding_lock,flags);
    service_binding_put(binding);module_execution_put(owner);module_put(owner);
}
EXPORT_SYMBOL(kernel_service_register);
EXPORT_SYMBOL(kernel_service_unregister);
EXPORT_SYMBOL(kernel_service_acquire);
EXPORT_SYMBOL(kernel_service_release);
