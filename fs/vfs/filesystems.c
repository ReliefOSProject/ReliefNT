/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/fs_type.h>
#include <reliefnt/module.h>
#include <reliefnt/mm.h>
#include <reliefnt/lock.h>
#include <reliefnt/export.h>
#include <linux/errno.h>
#include "mount_internal.h"

struct fs_registration {
    struct fs_registration *next;
    const struct file_system_type *type;
    uint64_t references;
    bool linked,active;
};
static struct fs_registration *filesystems;
static struct kernel_spinlock filesystem_lock=KERNEL_SPINLOCK_INIT;

/** @brief Drop one core entry reference without running any provider callback.
 * @param entry Owned core allocation reference, required and stable until put.
 * @return None. IRQ-safe; frees only after dropping the short registry lock.
 */
static void registration_put(struct fs_registration *entry)
{
    uint64_t irq;kernel_spin_lock_irqsave(&filesystem_lock,&irq);
    bool last=!--entry->references;
    kernel_spin_unlock_irqrestore(&filesystem_lock,irq);
    if(last)kernel_free(entry);
}

/** @brief Close this exact entry even if its descriptor was later re-registered.
 * @param context Resource-owned core registration, retained through final release.
 * @return Zero. Task context, no I/O/provider calls; idempotent for unload retry.
 */
static int registration_close(void *context)
{
    struct fs_registration *entry=context;
    uint64_t irq;kernel_spin_lock_irqsave(&filesystem_lock,&irq);
    bool linked=entry->linked;
    if(linked) {
        struct fs_registration **at=&filesystems;
        while(*at && *at!=entry)at=&(*at)->next;
        if(*at)*at=entry->next;
        entry->linked=false;entry->active=false;entry->next=NULL;
    }
    kernel_spin_unlock_irqrestore(&filesystem_lock,irq);
    if(linked)registration_put(entry);
    return 0;
}

/** @brief Release the module ledger's core entry after complete callback drain.
 * @param context Owned registry resource reference, already closed by quiesce.
 * @return None. Task context without module-table lock; never enters module code.
 */
static void registration_release(void *context)
{ registration_put(context); }

/** @brief Check the same simple filesystem-name alphabet used by mount metadata.
 * @param name Borrowed terminated descriptor name, nullable for rejection.
 * @return True for 1..VFS_NAME_MAX ASCII letters/digits/dot/dash/underscore.
 * No allocation or locks; descriptors are trusted persistent kernel objects.
 */
static bool filesystem_name_valid(const char *name)
{
    if(!name || !name[0])return false;
    for(size_t i=0;name[i];++i) {
        unsigned char c=name[i];
        if(i==VFS_NAME_MAX || !((c>='a' && c<='z') || (c>='A' && c<='Z') ||
            (c>='0' && c<='9') || c=='.' || c=='-' || c=='_'))return false;
    }
    return true;
}

/** @brief Reserve then publish a type only after its cleanup obligation exists.
 * @param type Persistent immutable kernel/module descriptor.
 * @return Zero or negative validation/duplicate/allocation/admission errno.
 * Init or pinned task context; reservation excludes duplicate names during
 * allocation, and its constructor reference survives concurrent cancellation.
 * No registry/module spinlock spans allocation or provider callbacks.
 */
int register_filesystem(const struct file_system_type *type)
{
    if(!type || !filesystem_name_valid(type->name) || !type->get_tree)return -EINVAL;
    struct fs_registration *entry=kernel_malloc(sizeof(*entry));if(!entry)return -ENOMEM;
    *entry=(struct fs_registration){.type=type,.references=1};
    uint64_t irq;kernel_spin_lock_irqsave(&filesystem_lock,&irq);
    for(struct fs_registration *at=filesystems;at;at=at->next) {
        if(!__builtin_strcmp(type->name,at->type->name)) {
            kernel_spin_unlock_irqrestore(&filesystem_lock,irq);kernel_free(entry);return -EBUSY;
        }
    }
    entry->linked=true;entry->next=filesystems;filesystems=entry;++entry->references;
    if(type->owner)++entry->references;
    kernel_spin_unlock_irqrestore(&filesystem_lock,irq);
    int rc=type->owner ? module_add_resource(type->owner,registration_close,registration_release,entry) : 0;
    if(rc) {
        registration_put(entry);registration_close(entry);
    } else {
        kernel_spin_lock_irqsave(&filesystem_lock,&irq);
        if(entry->linked)entry->active=true;else rc=-EBUSY;
        kernel_spin_unlock_irqrestore(&filesystem_lock,irq);
    }
    registration_put(entry);return rc;
}

/** @brief Unpublish the exact descriptor without invalidating admitted readers.
 * @param type Stable descriptor, nullable for EINVAL.
 * @return Zero or EINVAL when no matching registration is linked. No callback
 * or I/O; task context, with entry lifetime retained across unlocking.
 */
int unregister_filesystem(const struct file_system_type *type)
{
    uint64_t irq;kernel_spin_lock_irqsave(&filesystem_lock,&irq);
    struct fs_registration **at=&filesystems;
    while(*at && (*at)->type!=type)at=&(*at)->next;
    struct fs_registration *entry=*at;
    if(entry) {
        *at=entry->next;entry->next=NULL;entry->linked=false;entry->active=false;
    }
    kernel_spin_unlock_irqrestore(&filesystem_lock,irq);
    if(!entry)return -EINVAL;
    registration_put(entry);return 0;
}

/** @brief Admit a name lookup and pin both core entry and LIVE provider code.
 * @param name Borrowed case-sensitive name. @param out Required empty output.
 * @return Zero or negative input/absent/overflow errno. IRQ-safe; no provider
 * callback, allocation or I/O, registry-to-module lock order only.
 */
int fs_type_get(const char *name,struct fs_type_ref *out)
{
    if(!out)return -EINVAL;*out=(struct fs_type_ref){0};if(!name)return -EINVAL;
    int rc=-ENODEV;uint64_t irq;kernel_spin_lock_irqsave(&filesystem_lock,&irq);
    for(struct fs_registration *entry=filesystems;entry;entry=entry->next) {
        if(!entry->active || __builtin_strcmp(entry->type->name,name))continue;
        if(entry->references==UINT64_MAX){rc=-EOVERFLOW;break;}
        if(!try_module_get(entry->type->owner))break;
        ++entry->references;*out=(struct fs_type_ref){entry->type,entry};rc=0;break;
    }
    kernel_spin_unlock_irqrestore(&filesystem_lock,irq);return rc;
}

/** @brief Consume an admitted descriptor without entering potentially retired code.
 * @param reference Owned reference cleared on entry, nullable/empty accepted.
 * @return None. IRQ-safe; entry freed outside the registry lock before owner put.
 */
void fs_type_put(struct fs_type_ref *reference)
{
    if(!reference || !reference->type)return;
    struct fs_type_ref old=*reference;*reference=(struct fs_type_ref){0};
    struct module *owner=old.type->owner;
    registration_put(old.registration);module_put(owner);
}

/** @brief Enter a retained filesystem factory without holding registry locks.
 * @param reference Borrowed admitted entry and provider pin.
 * @param request Borrowed validated kernel mount operands.
 * @param out Required owned detached mount output, cleared on failure.
 * @return Zero or negative factory/input/inconsistent-owner errno. Task context
 * under VFS execution ownership; validates independent mount provider lifetime.
 */
int fs_type_get_tree(const struct fs_type_ref *reference,const struct fs_mount_request *request,
                     struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!reference || !reference->type || !reference->registration || !request)return -EINVAL;
    struct vfs_mount *mount=NULL;int rc=reference->type->get_tree(request,&mount);
    if(!rc && (!mount || mount->owner!=reference->type->owner))rc=-EIO;
    if(rc)vfs_mount_put(mount);else *out=mount;
    return rc;
}

EXPORT_SYMBOL(register_filesystem);
EXPORT_SYMBOL(unregister_filesystem);
EXPORT_SYMBOL(fs_type_get);
EXPORT_SYMBOL(fs_type_put);
EXPORT_SYMBOL(fs_type_get_tree);

/** @brief Enter a retained native formatter without releasing its admission pin.
 * @param reference Borrowed admitted type/code reference, required.
 * @param disk Borrowed authorized unmounted managed target disk.
 * @param first First validated sector. @param sectors Validated extent length.
 * @return Zero or input/unsupported/native errno. Task context under storage
 * execution ownership; no registry/module lock spans I/O and no pointer escapes.
 */
int fs_type_format(const struct fs_type_ref *reference,struct install_disk_state *disk,
                     uint64_t first,uint64_t sectors)
{
    if(!reference || !reference->type || !reference->registration || !disk || !sectors)return -EINVAL;
    return reference->type->format ? reference->type->format(disk,first,sectors) : -EOPNOTSUPP;
}
EXPORT_SYMBOL(fs_type_format);
