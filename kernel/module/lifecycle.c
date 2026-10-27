/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/export.h>
#include "internal.h"
#include <reliefnt/heap.h>
#include <linux/fcntl.h>

struct kernel_spinlock module_table_lock=KERNEL_SPINLOCK_INIT;
struct module_runtime *module_list;

/** @brief Retain the exact published instance's non-code identity token.
 * @param name Terminated normalized registry name, nullable.
 * @return Owned token or NULL when absent/reference count exhausted. No
 * allocation or callback under the short registry lock; does not alter the
 * module's external-user or executing counts and does not inhibit unload.
 */
struct module_identity *module_identity_get(const char *name)
{
    if(!name)return NULL;
    struct module_identity *identity=NULL;
    uint64_t flags;kernel_spin_lock_irqsave(&module_table_lock,&flags);
    for(struct module_runtime *r=module_list;r;r=r->next) {
        if(!module_string_equal(r->name,name))continue;
        if(r->identity && r->identity->alive && r->identity->references<UINT64_MAX) {
            identity=r->identity;++identity->references;
        }
        break;
    }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);return identity;
}

/** @brief Test a retained token without borrowing potentially freed module data.
 * @param identity Borrowed owned token, or NULL.
 * @return Whether the original instance is still published. A reload never
 * changes an old token back to alive. Short registry lock, no ownership
 * transfer, allocation, callbacks or I/O; callers retain the token throughout.
 */
bool module_identity_alive(const struct module_identity *identity)
{
    if(!identity)return false;
    uint64_t flags;kernel_spin_lock_irqsave(&module_table_lock,&flags);
    bool alive=identity->alive;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);return alive;
}

/** @brief Drop one runtime/generated-inode identity reference.
 * @param identity Owned token or NULL; consumed once by this call.
 * Task context. Refcount and retirement serialize under the registry lock;
 * final free occurs after unlocking, with no code/data callbacks or I/O.
 */
void module_identity_put(struct module_identity *identity)
{
    if(!identity)return;
    uint64_t flags;kernel_spin_lock_irqsave(&module_table_lock,&flags);
    bool final=!--identity->references;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    if(final)kernel_free(identity);
}

/** @brief Compare normalized module names without borrowing transient buffers.
 * @param left Terminated stored name.
 * @param right Terminated lookup name, hyphens equivalent to underscores.
 * @return True for equality; caller provides stable strings.
 */
static bool module_name_equal(const char *left,const char *right)
{
    while(*left && *right) {
        char a=*left=='-' ? '_' : *left,b=*right=='-' ? '_' : *right;
        if(a!=b) return false;
        ++left; ++right;
    }
    return *left==*right;
}
/** @brief Find an existing runtime under module_table_lock.
 * @param name Terminated lookup name.
 * @return Borrowed runtime or NULL; lock must remain held until pin acquired.
 */
static struct module_runtime *runtime_find(const char *name)
{
    if(!name) return NULL;
    for(struct module_runtime *r=module_list;r;r=r->next)
        if(module_name_equal(r->name,name)) return r;
    return NULL;
}
/** @brief Acquire an external LIVE reference while the registration owner is stable.
 * @param owner Stable borrowed owner; NULL denotes permanent built-in code.
 * @return True if admitted; IRQ-safe, no allocation or callbacks.
 */
bool try_module_get(struct module *owner)
{
    if(!owner) return true;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *r=owner->runtime;
    bool allowed=r && r->state==MODULE_LIVE && r->users!=UINT64_MAX;
    if(allowed) ++r->users;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags); return allowed;
}
/** @brief Release a caller-owned external module reference without entering module code.
 * @param owner Owner of one reference; NULL is a no-op. IRQ-safe.
 */
void module_put(struct module *owner)
{
    if(!owner) return;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    if(owner->runtime && owner->runtime->users) --owner->runtime->users;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
}
/** @brief Resolve a LIVE module name and atomically pin its image.
 * @param name Terminated lookup name; NULL yields NULL.
 * @return Owned external reference or NULL; release with module_put, IRQ-safe.
 */
struct module *module_get_by_name(const char *name)
{
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *r=runtime_find(name);
    struct module *owner=NULL;
    if(r && r->state==MODULE_LIVE && r->users!=UINT64_MAX) { ++r->users; owner=r->owner; }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags); return owner;
}
/** @brief Pin admitted module code before a callback is entered or queued.
 * @param owner Borrowed stable registration owner; NULL denotes permanent code.
 * @return True if execution was pinned; false while initializing/unloading, IRQ-safe.
 */
bool module_execution_get(struct module *owner)
{
    if(!owner) return true;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *r=owner->runtime;
    bool allowed=r && r->state==MODULE_LIVE && r->executing!=UINT64_MAX;
    if(allowed) ++r->executing;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags); return allowed;
}
/** @brief Release one completed or cancelled execution pin; never calls module code.
 * @param owner Owner of an acquired pin; NULL is a no-op. IRQ-safe.
 */
void module_execution_put(struct module *owner)
{
    if(!owner) return;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    if(owner->runtime && owner->runtime->executing) --owner->runtime->executing;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
}
/** @brief Add a self-owned cleanup ledger entry without an external self-reference.
 * @param owner Stable COMING/LIVE owner; NULL denotes a permanent builtin resource.
 * @param quiesce Optional synchronous STOP/cancel callback; nonzero prevents destruction.
 * @param release Required final resource release callback, called after complete drain.
 * @param context Borrowed resource state retained until release callback.
 * @return Zero or negative errno; allocation/callbacks never run under table lock.
 */
int module_add_resource(struct module *owner,int (*quiesce)(void *),
                         void (*release)(void *),void *context)
{
    if(!release) return -EINVAL;
    if(!owner) return 0;
    struct module_resource *resource=kernel_malloc(sizeof(*resource));
    if(!resource) return -ENOMEM;
    *resource=(struct module_resource){.quiesce=quiesce,.release=release,.context=context};
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *r=owner->runtime;
    bool allowed=r && r->state!=MODULE_GOING;
    if(allowed) { resource->next=r->resources; r->resources=resource; }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    if(!allowed) { kernel_free(resource); return -EBUSY; }
    return 0;
}
/** @brief Remove a fully quiesced runtime from lookup before freeing code/data/providers.
 * @param runtime Exclusively cleanup-owned runtime with no users/executors/resources.
 * Task context; no global lock across unmapping/free/provider reference drops.
 */
static void runtime_destroy(struct module_runtime *runtime)
{
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime **at=&module_list;
    while(*at && *at!=runtime) at=&(*at)->next;
    if(*at) *at=runtime->next;
    if(runtime->identity)runtime->identity->alive=false;
    if(runtime->owner) runtime->owner->runtime=NULL;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    module_identity_put(runtime->identity);
    module_dependencies_release(runtime);
    parameter_values_release(&runtime->parameters);kernel_free(runtime->modinfo);
    module_layout_destroy(&runtime->layout); kernel_free(runtime);
}
/** @brief STOP every owned source, drain pinned callbacks, then release code and resources.
 * @param runtime Exclusively cleanup-owned GOING runtime, cleanup_running already set.
 * @return Zero when destroyed; -EBUSY retains the complete image and pending resources.
 * No registry lock spans driver callbacks, waits, exit code or physical-memory release.
 */
static int runtime_cleanup(struct module_runtime *runtime)
{
    bool stopped=true;
    for(struct module_resource *r=runtime->resources;r;r=r->next) {
        if(!r->stopped && (!r->quiesce || !r->quiesce(r->context))) r->stopped=true;
        if(!r->stopped) stopped=false;
    }
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    bool drained=runtime->executing==0;
    if(!stopped || !drained) runtime->cleanup_running=false;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    if(!stopped || !drained) return -EBUSY;
    if(runtime->init_succeeded && runtime->owner->exit && !runtime->exit_called) {
        runtime->exit_called=true; runtime->owner->exit();
    }
    while(runtime->resources) {
        struct module_resource *resource=runtime->resources;
        runtime->resources=resource->next;
        resource->release(resource->context); kernel_free(resource);
    }
    runtime_destroy(runtime); return 0;
}
/** @brief Claim unload admission only after external references reach zero.
 * @param name Terminated module name.
 * @param flags Linux delete_module flags, ignored as on Linux 6.12 without
 * CONFIG_MODULE_FORCE_UNLOAD; even O_TRUNC cannot bypass references or exit.
 * @return Zero, -ENOENT, -EWOULDBLOCK, -EBUSY, -ENOSYS when unloading is
 * disabled, or native cleanup errno. Task context.
 * STOP/callback failure retains GOING code for a later safe cleanup retry.
 */
int module_unload(const char *name,unsigned int flags)
{
#ifndef CONFIG_MODULE_UNLOAD
    (void)name; (void)flags; return -ENOSYS;
#else
    /* Linux 6.12 delete_module does not reject extra flag bits. In particular
     * BusyBox rmmod sets O_EXCL. With forced unloading disabled all requests
     * still take exactly the ordinary reference/exit/drain checks below. */
    (void)flags;
    uint64_t irq_flags; kernel_spin_lock_irqsave(&module_table_lock,&irq_flags);
    struct module_runtime *runtime=runtime_find(name);
    int rc=0;
    if(!runtime) rc=-ENOENT;
    else if(runtime->users) rc=-EWOULDBLOCK;
    else if(runtime->state==MODULE_COMING || runtime->cleanup_running ||
            (runtime->init_succeeded && runtime->owner->init && !runtime->owner->exit)) rc=-EBUSY;
    else { runtime->state=MODULE_GOING; runtime->cleanup_running=true; }
    kernel_spin_unlock_irqrestore(&module_table_lock,irq_flags);
    return rc ? rc : runtime_cleanup(runtime);
#endif
}
/** @brief Validate modpost's relocated instance and its init/exit code ownership.
 * @param view Checked ELF view matching runtime layout.
 * @param runtime Private COMING runtime receiving the module instance pointer.
 * @return Zero or -ENOEXEC; layout is still RW/NX and no module code is entered.
 */
static int runtime_instance(const struct module_elf *view,struct module_runtime *runtime)
{
    uint64_t address=module_layout_find_symbol(view,&runtime->layout,"__this_module");
    if(!address || address%_Alignof(struct module) ||
       !module_layout_contains(&runtime->layout,address,sizeof(struct module),MODULE_ELF_WRITE)) return -ENOEXEC;
    struct module *owner=(void *)(uintptr_t)address;
    size_t length=0; while(length<MODULE_NAME_LEN && owner->name[length]) ++length;
    if(length==MODULE_NAME_LEN || !module_name_equal(runtime->name,owner->name) || owner->runtime || owner->arch.reserved)
        return -ENOEXEC;
    uint64_t init=module_layout_find_symbol(view,&runtime->layout,"init_module");
    uint64_t exit=module_layout_find_symbol(view,&runtime->layout,"cleanup_module");
    if((uintptr_t)owner->init!=init || (uintptr_t)owner->exit!=exit ||
       (init && !module_layout_contains(&runtime->layout,init,1,MODULE_ELF_EXEC)) ||
       (exit && !module_layout_contains(&runtime->layout,exit,1,MODULE_ELF_EXEC))) return -ENOEXEC;
    runtime->owner=owner; owner->runtime=runtime; return 0;
}
/** @brief Load one native ET_REL transaction without holding the registry lock over init.
 * @param image Stable immutable kernel-owned bytes, not a user pointer.
 * @param size Readable image byte count.
 * @param params Stable terminated kernel parameter string, NULL means empty.
 * @param flags Must be zero; forced ABI/version bypass is unsupported.
 * @return Zero or negative errno. Failed init closes admission and safely unwinds resources.
 */
int module_load_image(const void *image,size_t size,const char *params,unsigned int flags)
{
    if(flags) return -EINVAL;
    struct module_elf view;
    int rc=module_elf_validate(image,size,&view);
    if(rc) return rc;
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));
    if(!runtime) return -ENOMEM;
    module_zero(runtime,sizeof(*runtime));
    runtime->identity=kernel_malloc(sizeof(*runtime->identity));
    if(!runtime->identity){runtime_destroy(runtime);return -ENOMEM;}
    *runtime->identity=(struct module_identity){.references=1};
    rc=module_metadata_prepare(&view,runtime);
    if(rc) { runtime_destroy(runtime); return rc; }
    uint64_t irq_flags; kernel_spin_lock_irqsave(&module_table_lock,&irq_flags);
    if(runtime_find(runtime->name) || module_parameter_builtin_exists(runtime->name)) rc=-EEXIST;
    else { runtime->state=MODULE_COMING;runtime->identity->alive=true;runtime->next=module_list;module_list=runtime; }
    kernel_spin_unlock_irqrestore(&module_table_lock,irq_flags);
    if(rc) { runtime_destroy(runtime); return rc; }
    rc=module_layout_create(&view,&runtime->layout);
    if(!rc) rc=module_versions_prepare(&view,runtime);
    if(!rc) rc=module_layout_relocate(&view,&runtime->layout,module_resolve_symbol,runtime);
    if(!rc) rc=runtime_instance(&view,runtime);
    if(!rc) rc=module_parameters_prepare(&view,runtime,params);
    if(!rc) rc=module_symbols_prepare(&view,runtime);
    if(!rc) rc=module_layout_seal(&runtime->layout);
    if(!rc && runtime->owner->init) rc=runtime->owner->init();
    kernel_spin_lock_irqsave(&module_table_lock,&irq_flags);
    if(rc>=0) {
        runtime->init_succeeded=true; runtime->state=MODULE_LIVE;
        for(size_t i=1;i<runtime->layout.section_count;++i)
            if(runtime->layout.sections[i].memory) runtime->memory_size+=runtime->layout.sections[i].memory->size;
    } else { runtime->state=MODULE_GOING; runtime->cleanup_running=true; }
    kernel_spin_unlock_irqrestore(&module_table_lock,irq_flags);
    if(rc<0) { (void)runtime_cleanup(runtime); return rc; }
    return 0;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(module_add_resource);

EXPORT_SYMBOL(module_put);
EXPORT_SYMBOL(try_module_get);
