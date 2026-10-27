/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/chrdev.h>
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <linux/errno.h>
#include <linux/poll.h>

struct character_registration {
    struct character_registration *next;
    const struct character_device_registration *descriptor;
    uint64_t references,users;
    bool linked,active;
};
struct character_file {struct character_registration *registration;};
static struct kernel_spinlock character_lock=KERNEL_SPINLOCK_INIT;
static struct character_registration *character_devices;

/** @brief Release one internal registration reference after dropping its lock.
 * @param registration Owned reference, consumed once.
 * Task context; final free never runs under character/module registry locks.
 */
static void character_registration_put(struct character_registration *registration)
{
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);
    bool final=!--registration->references;
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    if(final)kernel_free(registration);
}
/** @brief Close admission and unlink an idle exact registration.
 * @param value Stable ledger/construction-owned registration.
 * @return Zero or EBUSY. Task context; idempotent for already detached records.
 * No provider callback, allocation or hardware access under the registry lock.
 */
static int character_registration_stop(void *value)
{
    struct character_registration *registration=value;bool detached=false;
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);
    int rc=registration->users ? -EBUSY : 0;
    if(!rc && registration->linked) {
        struct character_registration **at=&character_devices;
        while(*at && *at!=registration)at=&(*at)->next;
        if(*at)*at=registration->next;
        registration->linked=false;registration->active=false;detached=true;
    }
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    if(detached)character_registration_put(registration);
    return rc;
}
/** @brief Consume the module cleanup ledger's registration reference.
 * @param value Owned stopped registration; no OFDs remain.
 * Task context outside registry locks; never invokes provider code.
 */
static void character_registration_release(void *value)
{character_registration_put(value);}
/** @brief Reserve a nonoverlapping major/minor interval and cleanup obligation.
 * @param descriptor Persistent immutable registration and callback table.
 * @return Zero or negative errno; all unpublished failures roll back completely.
 * Task/init context. Construction pin survives concurrent unregistration;
 * no registry lock spans heap allocation or module resource-ledger creation.
 */
int character_device_register(const struct character_device_registration *descriptor)
{
    if(!descriptor || !descriptor->operations || descriptor->major>4095u ||
       !descriptor->count || descriptor->first_minor>=1048576u ||
       descriptor->count>1048576u-descriptor->first_minor ||
       (descriptor->operations->open && !descriptor->operations->close))return -EINVAL;
    struct character_registration *registration=kernel_malloc(sizeof(*registration));
    if(!registration)return -ENOMEM;
    *registration=(struct character_registration){.descriptor=descriptor,.references=1};
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);
    for(struct character_registration *at=character_devices;at;at=at->next) {
        const struct character_device_registration *old=at->descriptor;
        if(old->major==descriptor->major && old->first_minor<descriptor->first_minor+descriptor->count &&
           descriptor->first_minor<old->first_minor+old->count) {
            kernel_spin_unlock_irqrestore(&character_lock,flags);kernel_free(registration);return -EEXIST;
        }
    }
    registration->next=character_devices;character_devices=registration;
    registration->linked=true;++registration->references;
    if(descriptor->owner)++registration->references;
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    int rc=descriptor->owner ? module_add_resource(descriptor->owner,character_registration_stop,
                                                    character_registration_release,registration) : 0;
    if(rc){character_registration_put(registration);(void)character_registration_stop(registration);}
    else {
        kernel_spin_lock_irqsave(&character_lock,&flags);
        if(registration->linked)registration->active=true;else rc=-EBUSY;
        kernel_spin_unlock_irqrestore(&character_lock,flags);
    }
    character_registration_put(registration);return rc;
}
/** @brief Unpublish one exact idle range while retaining ledger identity.
 * @param descriptor Stable exact descriptor, required.
 * @return Zero or EINVAL/ENOENT/EBUSY; no provider callback or hardware access.
 * Execution-owned task context, balanced temporary registration reference.
 */
int character_device_unregister(const struct character_device_registration *descriptor)
{
    if(!descriptor)return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);
    struct character_registration *registration=character_devices;
    while(registration && registration->descriptor!=descriptor)registration=registration->next;
    if(registration && registration->references<UINT64_MAX)++registration->references;
    else registration=NULL;
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    if(!registration)return -ENOENT;
    int rc=character_registration_stop(registration);character_registration_put(registration);return rc;
}
/** @brief Drop one admitted OFD's registration and provider references.
 * @param registration Owned admitted user/reference pair.
 * Task context after all callbacks complete. Provider put is outside the
 * character lock, and no descriptor is read after its final provider put.
 */
static void character_open_put(struct character_registration *registration)
{
    struct module *owner=registration->descriptor->owner;
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);--registration->users;
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    character_registration_put(registration);module_put(owner);
}
/** @brief Admit an exact dev_t and enter its original callback with code pinned.
 * @param device Linux encoded dev_t, bounded to kernel major/minor widths.
 * @param task Borrowed current task.
 * @param file Borrowed OFD passed through without retaining its slot address.
 * @param out Empty handle output, kept empty on failure.
 * @return Zero or negative errno. Execution-owned task context; partial open
 * closes before dropping code, no character/module lock spans any callback.
 */
int character_file_open(uint64_t device,struct task *task,struct task_file *file,struct character_file **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(device>UINT32_MAX)return -ENODEV;
    uint32_t major=(uint32_t)(device>>8)&4095u,minor=(uint32_t)(device&255u)|((uint32_t)(device>>12)&~255u);
    uint64_t flags;kernel_spin_lock_irqsave(&character_lock,&flags);
    struct character_registration *registration=character_devices;
    for(;registration;registration=registration->next) {
        const struct character_device_registration *descriptor=registration->descriptor;
        if(descriptor->major!=major || minor<descriptor->first_minor || minor-descriptor->first_minor>=descriptor->count)continue;
        if(!registration->active || registration->users==UINT64_MAX || registration->references==UINT64_MAX ||
           !try_module_get(descriptor->owner))registration=NULL;
        else{++registration->users;++registration->references;}
        break;
    }
    kernel_spin_unlock_irqrestore(&character_lock,flags);
    if(!registration)return -ENODEV;
    struct character_file *handle=kernel_malloc(sizeof(*handle));
    if(!handle){character_open_put(registration);return -ENOMEM;}
    handle->registration=registration;
    const struct character_file_operations *ops=registration->descriptor->operations;
    int rc=ops->open ? ops->open(task,file) : 0;
    if(rc){if(ops->close)ops->close(file);character_open_put(registration);kernel_free(handle);return rc;}
    *out=handle;return 0;
}
/** @brief Finish provider close before releasing its admitted code reference.
 * @param handle Owned final OFD handle, nullable; consumed.
 * @param file Borrowed current canonical OFD.
 * Execution-owned task context; no registry lock spans callbacks or freeing.
 */
void character_file_close(struct character_file *handle,struct task_file *file)
{
    if(!handle)return;
    struct character_registration *registration=handle->registration;
    const struct character_file_operations *ops=registration->descriptor->operations;
    if(ops->close)ops->close(file);
    character_open_put(registration);kernel_free(handle);
}
/** @brief Dispatch a read through an already retained character provider.
 * @param handle Borrowed admitted handle. @param task Current task.
 * @param file Borrowed OFD. @param buffer Validated destination.
 * @param length Requested byte count.
 * @return Backend result or EINVAL; execution-owned, no locks/reference changes.
 */
int64_t character_file_read(struct character_file *handle,struct task *task,struct task_file *file,void *buffer,uint64_t length)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->read ? ops->read(task,file,buffer,length) : -EINVAL;
}
/** @brief Dispatch a write through its exact OFD-pinned provider.
 * @param handle Borrowed handle. @param task Current task. @param file OFD.
 * @param buffer Validated input. @param length Requested byte count.
 * @return Backend result or EINVAL; execution-owned, no locks/reference changes.
 */
int64_t character_file_write(struct character_file *handle,struct task *task,struct task_file *file,const void *buffer,uint64_t length)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->write ? ops->write(task,file,buffer,length) : -EINVAL;
}
/** @brief Dispatch ioctl with original provider code retained by the caller.
 * @param handle Borrowed handle. @param task Current task. @param file OFD.
 * @param request Linux selector. @param argument Backend-validated operand.
 * @return Backend result or ENOTTY; execution-owned, no registry locks.
 */
int64_t character_file_ioctl(struct character_file *handle,struct task *task,struct task_file *file,uint64_t request,uint64_t argument)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->ioctl ? ops->ioctl(task,file,request,argument) : -ENOTTY;
}
/** @brief Query readiness through one retained OFD, never a new dev_t lookup.
 * @param handle Borrowed handle. @param file OFD. @param events Requested bits.
 * @return Backend readiness or Linux default data readiness filtered by events.
 * Execution-owned task context; no registry lock or ownership transfer.
 */
short character_file_poll(struct character_file *handle,struct task_file *file,short events)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->poll ? ops->poll(file,events) : events&(POLLIN|POLLOUT|POLLRDNORM|POLLWRNORM);
}
/** @brief Seek through an exact retained provider.
 * @param handle Borrowed handle. @param file OFD. @param offset Signed offset.
 * @param whence Linux selector.
 * @return Backend offset/errno or ESPIPE; execution-owned, no registry locks.
 */
int64_t character_file_seek(struct character_file *handle,struct task_file *file,int64_t offset,unsigned whence)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->seek ? ops->seek(file,offset,whence) : -ESPIPE;
}
/** @brief Delegate mapping construction while the original OFD pins provider code.
 * @param handle Borrowed admitted handle. @param task Current task.
 * @param file Borrowed OFD. @param offset Backend selector.
 * @param length Rounded extent. @param prot Requested protection.
 * @param flags Linux mmap flags. @param out Required empty owned output.
 * @return Zero or negative errno; task/execution ownership required, no locks
 * span callbacks and absent mmap returns ENODEV without creating any backing.
 */
int character_file_mmap(struct character_file *handle,struct task *task,struct task_file *file,
                         uint64_t offset,uint64_t length,uint32_t prot,uint64_t flags,struct device_mapping **out)
{
    if(!out)return -EINVAL;*out=NULL;
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->mmap ? ops->mmap(task,file,offset,length,prot,flags,out) : -ENODEV;
}
/** @brief Inspect the immutable operation table retained by an admitted OFD.
 * @param handle Borrowed admitted handle.
 * @return Whether readv exists. Execution-owned, no locks or reference changes.
 */
bool character_file_has_readv(const struct character_file *handle)
{return handle->registration->descriptor->operations->readv!=NULL;}
/** @brief Enter the original provider's vector reader with its code pinned.
 * @param handle Borrowed admitted handle. @param task Current caller.
 * @param file Canonical OFD. @param vectors Imported, validated kernel vectors.
 * @param count Vector count. @param positional Select explicit offset semantics.
 * @param position Explicit offset. @param flags Backend-validated RWF flags.
 * @return Backend bytes/errno or EOPNOTSUPP; execution-owned, no registry locks
 * or reference transfer. This adapter neither copies nor retains vector memory.
 */
int64_t character_file_readv(struct character_file *handle,struct task *task,struct task_file *file,
                             const struct iovec *vectors,uint64_t count,bool positional,
                             uint64_t position,uint64_t flags)
{
    const struct character_file_operations *ops=handle->registration->descriptor->operations;
    return ops->readv ? ops->readv(task,file,vectors,count,positional,position,flags) : -EOPNOTSUPP;
}

EXPORT_SYMBOL(character_device_register);
EXPORT_SYMBOL(character_device_unregister);

EXPORT_SYMBOL(character_file_open);
