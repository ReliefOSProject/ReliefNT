/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/usb.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/export.h>
#include <linux/errno.h>

struct usb_host {
    struct usb_host *next;
    struct module *owner;
    void (*poll)(void *);
    void (*service)(void *);
    void *context;
    uint64_t id;
    bool closing;
    bool active;
    bool pending;
};
static struct kernel_spinlock usb_hosts_lock=KERNEL_SPINLOCK_INIT;
static struct usb_host *usb_hosts;
static uint64_t usb_host_sequence, usb_poll_cursor;
static uint64_t usb_service_cursor;
static bool usb_queued;

/** @brief Allocate a self-owned host registration before publishing its poll source.
 * @param poll IRQ-safe bounded callback, forbidden from unregistering itself.
 * @param context Borrowed controller context retained until unregister returns.
 * @param owner Borrowed module owner or NULL for permanent builtin code.
 * @param out Required exclusively owned registration handle result.
 * @return Zero or negative errno with out NULL; task context, allocation occurs
 * outside the registry lock and registration does not pin the owner forever.
 */
int usb_host_register(void (*poll)(void *),void *context,struct module *owner,
                      struct usb_host **out)
{
    return usb_host_register_service(poll,NULL,context,owner,out);
}

/** @brief Publish a host's non-overlapping IRQ and optional deferred callbacks.
 * @param poll Bounded IRQ callback. @param service Optional task callback.
 * @param context Retained controller. @param owner Borrowed native code owner.
 * @param out Required exclusive handle result. @return Zero or negative errno.
 * Initialization context; no callback/allocator runs under the registry lock.
 */
int usb_host_register_service(void (*poll)(void *),void (*service)(void *),
                               void *context,struct module *owner,struct usb_host **out)
{
    if(!out) return -EINVAL;
    *out=NULL;
    if(!poll) return -EINVAL;
    struct usb_host *host=kernel_malloc(sizeof(*host));
    if(!host) return -ENOMEM;
    *host=(struct usb_host){.poll=poll,.service=service,.context=context,.owner=owner};
    uint64_t flags;kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    int error=0;
    struct usb_host **tail=&usb_hosts;
    while(*tail) {
        if((*tail)->poll==poll && (*tail)->context==context) error=-EEXIST;
        tail=&(*tail)->next;
    }
    if(!error && usb_host_sequence==UINT64_MAX) error=-EOVERFLOW;
    if(!error) {host->id=++usb_host_sequence;*tail=host;*out=host;}
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    if(error) kernel_free(host);
    return error;
}
EXPORT_SYMBOL(usb_host_register);
EXPORT_SYMBOL(usb_host_register_service);

/** @brief Coalesce one host's maintenance request while its handle remains owned.
 * @param host Borrowed live host, nullable. IRQ/task safe; no callback or wait.
 */
void usb_host_request_service(struct usb_host *host)
{
    if(!host)return;
    uint64_t flags;kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    if(!host->closing && host->service) {
        host->pending=true;
        __atomic_store_n(&usb_queued,true,__ATOMIC_RELEASE);
    }
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
}
EXPORT_SYMBOL(usb_host_request_service);

/** @brief Query queued work without entering a callback or pinning an owner. */
bool usb_service_pending(void)
{ return __atomic_load_n(&usb_queued,__ATOMIC_ACQUIRE); }

/** @brief Fairly service up to four queued hosts with stable callback/code pins.
 * Waitable task context under manager admission; the registry lock never spans
 * hardware, allocation or I/O. Active pollers retain their pending request.
 */
void usb_service_run(void)
{
    uint64_t flags;kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    uint64_t start=usb_service_cursor,cursor=start,limit=usb_host_sequence;
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    bool wrapped=false;
    for(unsigned visited=0;visited<4;++visited) {
        kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
        struct usb_host *host=usb_hosts;
        while(host && (host->id<=cursor || host->id>(wrapped ? start : limit)))host=host->next;
        if(!host && !wrapped && start) {
            wrapped=true;cursor=0;host=usb_hosts;
            while(host && host->id>start)host=host->next;
        }
        if(!host){kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);break;}
        cursor=host->id;usb_service_cursor=cursor;
        bool admitted=host->pending && host->service && !host->closing &&
            !__atomic_load_n(&host->active,__ATOMIC_RELAXED) && module_execution_get(host->owner);
        if(admitted) { host->pending=false;__atomic_store_n(&host->active,true,__ATOMIC_RELEASE); }
        kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
        if(!admitted)continue;
        host->service(host->context);
        kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
        module_execution_put(host->owner);
        __atomic_store_n(&host->active,false,__ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    }
    kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    bool pending=false;
    for(struct usb_host *host=usb_hosts;host;host=host->next)
        if(!host->closing && host->pending)pending=true;
    __atomic_store_n(&usb_queued,pending,__ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
}

/** @brief Revoke a host's poll source, then drain code pins before releasing its handle.
 * @param host Exclusive live handle or NULL for no-op.
 * @return Zero, -ENOENT or -EBUSY; task context without callback-required locks.
 * DMA and device state remain caller-owned and must be stopped separately.
 */
int usb_host_unregister(struct usb_host *host)
{
    if(!host) return 0;
    uint64_t flags;kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    struct usb_host *found=usb_hosts;
    while(found && found!=host) found=found->next;
    int error=!found ? -ENOENT : (host->closing ? -EBUSY : 0);
    if(!error) host->closing=true;
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    if(error) return error;
    while(__atomic_load_n(&host->active,__ATOMIC_ACQUIRE))
        __asm__ volatile("pause" ::: "memory");
    kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    struct usb_host **at=&usb_hosts;
    while(*at!=host) at=&(*at)->next;
    *at=host->next;
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    kernel_free(host);
    return 0;
}
EXPORT_SYMBOL(usb_host_unregister);

/** @brief Fairly visit a bounded batch of host pollers, pinning each LIVE owner.
 * IRQ context, no allocation or waiting. Each visited host runs at most once
 * per invocation and never concurrently with itself. The cursor is numeric,
 * so concurrent unregister cannot leave a dangling iterator across callbacks.
 */
void usb_poll(void)
{
    uint64_t flags;kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
    uint64_t start=usb_poll_cursor,cursor=start,limit=usb_host_sequence;
    kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    bool wrapped=false;
    for(unsigned visited=0;visited<8;++visited) {
        kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
        struct usb_host *host=usb_hosts;
        while(host && (host->id<=cursor || host->id>(wrapped ? start : limit)))host=host->next;
        if(!host && !wrapped && start) {
            wrapped=true;cursor=0;host=usb_hosts;
            while(host && host->id>start)host=host->next;
        }
        if(!host) {kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);break;}
        cursor=host->id;usb_poll_cursor=cursor;
        bool admitted=!host->closing && !__atomic_load_n(&host->active,__ATOMIC_RELAXED) &&
                       module_execution_get(host->owner);
        if(admitted)__atomic_store_n(&host->active,true,__ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
        if(!admitted)continue;
        host->poll(host->context);
        kernel_spin_lock_irqsave(&usb_hosts_lock,&flags);
        module_execution_put(host->owner);
        __atomic_store_n(&host->active,false,__ATOMIC_RELEASE);
        kernel_spin_unlock_irqrestore(&usb_hosts_lock,flags);
    }
}
