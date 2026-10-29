/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/blkdev.h>
#include <reliefnt/module.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/export.h>
#include <linux/errno.h>

struct block_device {
    struct block_device *next;
    const struct block_device_registration *registration;
    uint64_t references,users,serial;
    bool linked,active,revoked;
};
static struct block_device *devices,*last_device;
static uint64_t last_serial;
static struct kernel_spinlock device_lock=KERNEL_SPINLOCK_INIT;

static uint32_t disk_numbers,optical_numbers;

/** @brief Reserve stable Linux identities independently of registry admission.
 * @param optical Select standard optical major 11 or dynamic block major 259.
 * @return Unique dev_t or zero on namespace exhaustion. IRQ-safe; failed probes
 * consume gaps, no callbacks/allocations and no registration/module self-pin.
 */
uint64_t block_device_allocate_number(bool optical)
{
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    uint32_t *next=optical ? &optical_numbers : &disk_numbers;
    uint32_t limit=optical ? 32768u : 4096u;
    uint64_t number=*next<limit ? block_device_make_number(optical ? 11u : 259u,(*next)++*(optical ? 1u : 256u)) : 0;
    kernel_spin_unlock_irqrestore(&device_lock,flags);return number;
}

/** @brief Drop one registry/resource identity without entering transport code.
 * @param device Owned core reference; must remain valid until decrement.
 * IRQ-safe; final heap free occurs after the registry lock is released.
 */
static void device_put(struct block_device *device)
{
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    bool last=!--device->references;
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    if(last)kernel_free(device);
}

/** @brief Unlink an exact core identity while the caller holds the registry lock.
 * @param device Stable linked core entry with no external users.
 * Caller later drops the linked reference outside the lock; no callback/I/O.
 */
static void device_unlink_locked(struct block_device *device)
{
    struct block_device **at=&devices,*previous=NULL;
    while(*at && *at!=device){previous=*at;at=&(*at)->next;}
    if(*at)*at=device->next;
    if(last_device==device)last_device=previous;
    device->next=NULL;device->linked=false;device->active=false;
}

/** @brief Close the exact registration after module admission/users drain.
 * @param context Ledger-owned core reference, stable through final release.
 * @return Zero or -EBUSY retaining entry/code if external users remain. No hardware
 * or callback; repeated quiesce is safe even after descriptor re-registration.
 */
static int device_close(void *context)
{
    struct block_device *device=context;uint64_t flags;
    kernel_spin_lock_irqsave(&device_lock,&flags);
    if(device->users){kernel_spin_unlock_irqrestore(&device_lock,flags);return -EBUSY;}
    bool linked=device->linked;if(linked)device_unlink_locked(device);
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    if(linked)device_put(device);
    return 0;
}

/** @brief Release the module's core identity after all quiesce/callback work.
 * @param context One ledger-owned already-closed device reference.
 * No transport callback, module lock or physical-memory ownership transfer.
 */
static void device_release(void *context) {device_put(context);}

/** @brief Validate actual advertised geometry and complete required operations.
 * @param registration Borrowed persistent descriptor, NULL rejected.
 * @return True only for nonzero identity/extent, supported flags and operations;
 * no arbitrary disk count/capacity limit, allocation, provider call or lock.
 */
static bool device_valid(const struct block_device_registration *registration)
{
    if(!registration || !registration->operations)return false;
    const struct block_device_info *info=&registration->info;
    const struct block_device_operations *ops=registration->operations;
    if(!info->device || !info->blocks || !info->max_blocks || info->block_size<512 ||
       (info->block_size&(info->block_size-1u)) ||
       (info->flags&~(BLOCK_DEVICE_READ_ONLY|BLOCK_DEVICE_REMOVABLE|BLOCK_DEVICE_OPTICAL|BLOCK_DEVICE_WRITE_THROUGH)))return false;
    return ops->read && ((info->flags&BLOCK_DEVICE_READ_ONLY) || ops->write) &&
        ((info->flags&(BLOCK_DEVICE_READ_ONLY|BLOCK_DEVICE_WRITE_THROUGH)) || ops->flush);
}

/** @brief Reserve a dev_t before publishing its actual owner/resource obligation.
 * @param registration Persistent immutable descriptor and driver-owned context.
 * @return Zero or -EINVAL/-ENOMEM/-EBUSY/-EOVERFLOW/owner errno. Task/init context;
 * construction reference survives concurrent cancellation, no callback lock spans
 * allocation or ledger creation, and registration never creates a self code pin.
 */
int register_block_device(const struct block_device_registration *registration)
{
    if(!device_valid(registration))return -EINVAL;
    struct block_device *device=kernel_malloc(sizeof(*device));if(!device)return -ENOMEM;
    *device=(struct block_device){.registration=registration,.references=1};
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    for(struct block_device *at=devices;at;at=at->next)
        if(at->registration->info.device==registration->info.device){
            kernel_spin_unlock_irqrestore(&device_lock,flags);kernel_free(device);return -EBUSY;
        }
    if(last_serial==UINT64_MAX){kernel_spin_unlock_irqrestore(&device_lock,flags);kernel_free(device);return -EOVERFLOW;}
    device->serial=++last_serial;device->linked=true;++device->references;
    if(last_device)last_device->next=device;else devices=device;last_device=device;
    if(registration->owner)++device->references;
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    int result=registration->owner ? module_add_resource(registration->owner,device_close,device_release,device) : 0;
    if(result){device_put(device);(void)device_close(device);}
    else {
        kernel_spin_lock_irqsave(&device_lock,&flags);
        if(device->linked && !device->revoked)device->active=true;
        else result=device->revoked ? -ENODEV : -EBUSY;
        kernel_spin_unlock_irqrestore(&device_lock,flags);
        if(result)(void)device_close(device);
    }
    device_put(device);return result;
}

/** @brief Unpublish only an idle descriptor, preserving active open/mount users.
 * @param registration Stable exact descriptor, NULL rejected.
 * @return Zero, -EINVAL/-ENOENT/-EBUSY. No hardware STOP or provider callback;
 * the caller retains responsibility for its context and controller backing.
 */
int unregister_block_device(const struct block_device_registration *registration)
{
    if(!registration)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    struct block_device *device=devices;
    while(device && device->registration!=registration)device=device->next;
    if(!device){kernel_spin_unlock_irqrestore(&device_lock,flags);return -ENOENT;}
    if(device->users){kernel_spin_unlock_irqrestore(&device_lock,flags);return -EBUSY;}
    device_unlink_locked(device);kernel_spin_unlock_irqrestore(&device_lock,flags);
    device_put(device);return 0;
}

/** @brief Close I/O admission for a physically removed exact block registration.
 * @param registration Stable provider descriptor retained until idle unregister.
 * @return Zero or EINVAL/ENOENT. Task context; existing references retain their
 * original metadata/code, but cannot invoke new transport I/O. No callback or
 * allocation occurs under the lock, and removal never reuses the device number.
 */
int disconnect_block_device(const struct block_device_registration *registration)
{
    if(!registration)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    struct block_device *device=devices;
    while(device && device->registration!=registration)device=device->next;
    if(device){device->active=false;device->revoked=true;}
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    return device ? 0 : -ENOENT;
}
EXPORT_SYMBOL(disconnect_block_device);

/** @brief Pin a published identity and its LIVE code under registry exclusion.
 * @param device Stable candidate under device_lock.
 * @param out Empty required external token output.
 * @return Zero, -ENODEV or -EOVERFLOW; registry-to-module lock order only.
 */
static int device_get_locked(struct block_device *device,struct block_device_ref *out)
{
    if(!device->active)return -ENODEV;
    if(device->references==UINT64_MAX || device->users==UINT64_MAX)return -EOVERFLOW;
    if(!try_module_get(device->registration->owner))return -ENODEV;
    ++device->references;++device->users;out->device=device;return 0;
}

/** @brief Resolve dev_t without callbacks and retain its complete code identity.
 * @param device Nonzero native device number.
 * @param out Required empty external token, unchanged on error.
 * @return Zero or -EINVAL/-EBUSY/-ENODEV/-EOVERFLOW. IRQ-safe, no allocation/I/O.
 */
int block_device_get(uint64_t device,struct block_device_ref *out)
{
    if(!out || !device)return -EINVAL;if(out->device)return -EBUSY;
    int result=-ENODEV;uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    for(struct block_device *at=devices;at;at=at->next)
        if(at->registration->info.device==device){result=device_get_locked(at,out);break;}
    kernel_spin_unlock_irqrestore(&device_lock,flags);return result;
}

/** @brief Enumerate live registrations using a monotonic nonreused serial cursor.
 * @param cursor Required in/out serial, zero starts; changes only on success.
 * @param out Empty required external token, caller later consumes with put.
 * @return Zero, -ENOENT or -EINVAL/-EBUSY/-EOVERFLOW; IRQ-safe, no callbacks/I/O.
 */
int block_device_next(uint64_t *cursor,struct block_device_ref *out)
{
    if(!cursor || !out)return -EINVAL;if(out->device)return -EBUSY;
    int result=-ENOENT;uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    for(struct block_device *at=devices;at;at=at->next) {
        if(at->serial<=*cursor)continue;
        int found=device_get_locked(at,out);
        if(found==-ENODEV)continue;
        result=found;if(!found)*cursor=at->serial;break;
    }
    kernel_spin_unlock_irqrestore(&device_lock,flags);return result;
}

/** @brief Consume an external block/code reference without entering driver code.
 * @param reference Owned noncopyable token, NULL/empty accepted and cleared first.
 * IRQ-safe; entry heap and module count release happen outside the registry lock.
 */
void block_device_put(struct block_device_ref *reference)
{
    if(!reference || !reference->device)return;
    struct block_device *device=reference->device;reference->device=NULL;
    struct module *owner=device->registration->owner;
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);--device->users;
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    device_put(device);module_put(owner);
}

/** @brief Copy immutable geometry while a caller-owned external token stays held.
 * @param reference Borrowed stable nonempty token.
 * @param out Required writable scalar snapshot, no reference transferred.
 * @return Zero or -EINVAL; no lock, allocation, I/O or provider callback.
 */
int block_device_info(const struct block_device_ref *reference,struct block_device_info *out)
{
    if(!reference || !reference->device || !out)return -EINVAL;
    *out=reference->device->registration->info;return 0;
}

/** @brief Dispatch a complete extent with external and executing code ownership.
 * @param reference Borrowed stable nonempty token, kept through the callback.
 * @param first First logical block. @param count Nonzero bounded block count.
 * @param buffer Stable kernel count*block_size bytes, readable/writable by direction.
 * @param write True for write, false for read.
 * @return Zero or argument/readonly/admission/native errno, -EAGAIN unchanged.
 * Task context under caller/driver exclusion, never a registry lock over I/O.
 */
static int device_transfer(const struct block_device_ref *reference,uint64_t first,
                            uint32_t count,void *buffer,bool write)
{
    if(!reference || !reference->device || !buffer)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    bool active=reference->device->active;
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    if(!active)return -ENODEV;
    const struct block_device_registration *registration=reference->device->registration;
    const struct block_device_info *info=&registration->info;
    if(!count || count>info->max_blocks || first>=info->blocks || count>info->blocks-first ||
       (uint64_t)count*info->block_size>SIZE_MAX)return -EINVAL;
    if(write && (info->flags&BLOCK_DEVICE_READ_ONLY))return -EROFS;
    if(!module_execution_get(registration->owner))return -ENODEV;
    int result=write ? registration->operations->write(registration->context,first,count,buffer) :
        registration->operations->read(registration->context,first,count,buffer);
    module_execution_put(registration->owner);return result;
}

/** @brief Read one complete bounded extent through the referenced transport.
 * @param reference Borrowed owned token. @param first First logical block.
 * @param count Nonzero bounded blocks. @param buffer Writable count*block_size bytes.
 * @return Zero or validation/admission/native errno. Task context, no registry I/O lock.
 */
int block_device_read(const struct block_device_ref *reference,uint64_t first,uint32_t count,void *buffer)
{return device_transfer(reference,first,count,buffer,false);}

/** @brief Write one complete bounded extent through the referenced transport.
 * @param reference Borrowed owned token. @param first First logical block.
 * @param count Nonzero bounded blocks. @param buffer Readable count*block_size bytes.
 * @return Zero or validation/readonly/admission/native errno. No registry I/O lock.
 */
int block_device_write(const struct block_device_ref *reference,uint64_t first,uint32_t count,const void *buffer)
{return device_transfer(reference,first,count,(void *)buffer,true);}

/** @brief Flush only a referenced actual provider, preserving its native error.
 * @param reference Borrowed owned token, stable until callback return.
 * @return Zero, -EINVAL/-ENODEV or native errno; no callback is needed only for
 * explicitly read-only/write-through devices. Task context, no registry I/O lock.
 */
int block_device_flush(const struct block_device_ref *reference)
{
    if(!reference || !reference->device)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&device_lock,&flags);
    bool active=reference->device->active;
    kernel_spin_unlock_irqrestore(&device_lock,flags);
    if(!active)return -ENODEV;
    const struct block_device_registration *registration=reference->device->registration;
    if(!registration->operations->flush)return 0; /* Validated read-only/write-through. */
    if(!module_execution_get(registration->owner))return -ENODEV;
    int result=registration->operations->flush(registration->context);
    module_execution_put(registration->owner);return result;
}

EXPORT_SYMBOL(register_block_device);
EXPORT_SYMBOL(unregister_block_device);
EXPORT_SYMBOL(block_device_get);
EXPORT_SYMBOL(block_device_next);
EXPORT_SYMBOL(block_device_put);
EXPORT_SYMBOL(block_device_info);
EXPORT_SYMBOL(block_device_read);
EXPORT_SYMBOL(block_device_write);
EXPORT_SYMBOL(block_device_flush);

/** @brief Visit admitted transports with exact identity/execution pins to cancel I/O.
 * @param pid Exiting task identity, zero ignored.
 * Caller supplies storage/execution exclusion; no registry lock spans a callback.
 * Enumeration uses monotonic serials and each token is consumed after execution.
 */
void block_device_cancel_all(uint32_t pid)
{
    if(!pid)return;
    uint64_t cursor=0;struct block_device_ref ref={0};
    while(!block_device_next(&cursor,&ref)) {
        const struct block_device_registration *r=ref.device->registration;
        if(r->operations->cancel && module_execution_get(r->owner)) {
            r->operations->cancel(r->context,pid);module_execution_put(r->owner);
        }
        block_device_put(&ref);
    }
}
EXPORT_SYMBOL(block_device_allocate_number);
EXPORT_SYMBOL(block_device_cancel_all);
