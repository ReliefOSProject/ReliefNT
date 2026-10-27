/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/text_stream.h>
#include <generated/module_abi.h>

/** @brief Match one modinfo key without scanning beyond a validated record.
 * @param record Terminated validated record.
 * @param key Terminated key, excluding equals.
 * @return Borrowed value or NULL; no ownership/locking effects.
 */
static const char *record_value(const char *record,const char *key)
{
    while(*key && *record==*key) { ++record; ++key; }
    return !*key && *record=='=' ? record+1 : NULL;
}
/** @brief Return a metadata value from the runtime-owned validated string table.
 * @param runtime Stable runtime retaining modinfo.
 * @param key Terminated metadata key.
 * @return Borrowed first matching value or NULL; no locks.
 */
const char *module_metadata(const struct module_runtime *runtime,const char *key)
{
    for(size_t at=0;at<runtime->modinfo_size;) {
        const char *record=runtime->modinfo+at;
        const char *value=record_value(record,key);
        if(value) return value;
        do { ++at; } while(at<runtime->modinfo_size && runtime->modinfo[at-1]);
    }
    return NULL;
}
/** @brief Check namespace-import metadata while all string bytes remain kernel-owned.
 * @param runtime Stable importing runtime.
 * @param namespace Terminated namespace; empty needs no explicit import.
 * @return True when allowed; no reference acquisition or locks.
 */
bool module_imports_namespace(const struct module_runtime *runtime,const char *namespace)
{
    if(!namespace || !*namespace) return true;
    for(size_t at=0;at<runtime->modinfo_size;) {
        const char *record=runtime->modinfo+at;
        const char *value=record_value(record,"import_ns");
        if(value && module_string_equal(value,namespace)) return true;
        do { ++at; } while(at<runtime->modinfo_size && runtime->modinfo[at-1]);
    }
    return false;
}
/** @brief Copy modinfo and require exactly one valid name and exact native build identity.
 * @param view Validated ELF image retained by caller for this call.
 * @param runtime Private zeroed runtime receiving owned metadata and normalized name.
 * @return Zero or negative errno; allocated metadata remains runtime-owned on error.
 */
int module_metadata_prepare(const struct module_elf *view,struct module_runtime *runtime)
{
    struct module_elf_section info={0}; bool found=false;
    for(unsigned i=1;i<view->header.shnum;++i) {
        const char *name=module_elf_section_name(view,i);
        if(name && module_string_equal(name,".modinfo")) {
            if(found || module_elf_get_section(view,i,&info) || info.type!=1 || !info.size)
                return -ENOEXEC;
            found=true;
        }
    }
    if(!found || info.size>SIZE_MAX) return -ENOEXEC;
    runtime->modinfo=kernel_malloc((size_t)info.size);
    if(!runtime->modinfo) return -ENOMEM;
    runtime->modinfo_size=(size_t)info.size;
    module_copy(runtime->modinfo,view->image+info.offset,(size_t)info.size);
    unsigned names=0,identities=0;
    for(size_t at=0;at<runtime->modinfo_size;) {
        size_t start=at;
        while(at<runtime->modinfo_size && runtime->modinfo[at]) ++at;
        if(at==runtime->modinfo_size) return -ENOEXEC;
        const char *record=runtime->modinfo+start;
        if(record_value(record,"name")) ++names;
        if(record_value(record,"vermagic")) ++identities;
        const char *license=record_value(record,"license");
        if(license) {
            if(!*license || (runtime->license && !module_string_equal(runtime->license,license)))
                return -ENOEXEC;
            runtime->license=license;
        }
        ++at;
    }
    const char *name=module_metadata(runtime,"name");
    const char *magic=module_metadata(runtime,"vermagic");
    if(names!=1 || identities!=1 || !runtime->license ||
       !magic || !module_string_equal(magic,RELIEFNT_MODULE_VERMAGIC)) return -ENOEXEC;
    size_t length=0;
    while(name[length]) {
        unsigned char c=(unsigned char)name[length];
        if(length>=MODULE_NAME_LEN-1 || !((c>='a' && c<='z') || (c>='A' && c<='Z') ||
           (c>='0' && c<='9') || c=='_' || c=='-' || c=='.')) return -ENOEXEC;
        runtime->name[length]=c=='-' ? '_' : (char)c; ++length;
    }
    if(!length) return -ENOEXEC;
    runtime->name[length]=0;
    return 0;
}
/** @brief Stream a Linux /proc/modules snapshot while registry and dependency pins are stable.
 * @param offset First requested byte.
 * @param buffer Writable kernel buffer, NULL only for zero capacity.
 * @param capacity Byte capacity.
 * @param written Required output byte count.
 * @return Zero or -EINVAL; no I/O, callbacks or allocation under the short table lock.
 */
int module_proc_read(uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written)
{
    if(!written || (!buffer && capacity)) return -EINVAL;
    *written=0;
    struct text_stream text={.buffer=buffer,.capacity=capacity,.offset=offset};
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    for(struct module_runtime *r=module_list;r;r=r->next) {
        text_string(&text,r->name); text_string(&text," "); text_unsigned(&text,r->memory_size);
        text_string(&text," "); text_unsigned(&text,r->users); text_string(&text," ");
        bool any=false;
        for(struct module_runtime *other=module_list;other;other=other->next)
            for(struct module_dependency *d=other->dependencies;d;d=d->next) if(d->owner==r->owner) {
                text_string(&text,other->name); text_string(&text,","); any=true; break;
            }
        if(!any) text_string(&text,"-");
        text_string(&text,r->state==MODULE_LIVE ? " Live " : r->state==MODULE_COMING ? " Loading " : " Unloading ");
        text_string(&text,"0x0000000000000000\n"); /* No unprivileged kernel-address disclosure. */
    }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    *written=text.written; return 0;
}
/** @brief Copy one current module's status while its runtime is stable.
 * @param index Zero-based current list index.
 * @param out Writable caller-owned snapshot.
 * @return Zero or -EINVAL/-ENOENT; no references/pointers escape the table lock.
 */
int module_status_at(size_t index,struct module_status *out)
{
    if(!out) return -EINVAL;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *r=module_list;
    while(r && index) { r=r->next; --index; }
    if(r) {
        module_zero(out,sizeof(*out));
        module_copy(out->name,r->name,MODULE_NAME_LEN);
        out->state=r->state; out->size=r->memory_size; out->references=r->users;
    }
    bool found=r!=NULL;
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    return found ? 0 : -ENOENT;
}
/** @brief Copy one dependent name for a provider without retaining temporary runtime pointers.
 * @param name Terminated provider name from module_status_at.
 * @param index Zero-based holder index.
 * @param out Writable MODULE_NAME_LEN-byte output.
 * @return Zero or -EINVAL/-ENOENT; short registry lock and no callbacks.
 */
int module_holder_at(const char *name,size_t index,char out[MODULE_NAME_LEN])
{
    if(!name || !out) return -EINVAL;
    uint64_t flags; kernel_spin_lock_irqsave(&module_table_lock,&flags);
    struct module_runtime *provider=module_list;
    while(provider && !module_string_equal(provider->name,name)) provider=provider->next;
    bool found=false;
    for(struct module_runtime *r=module_list;provider && r && !found;r=r->next)
        for(struct module_dependency *d=r->dependencies;d;d=d->next) if(d->owner==provider->owner) {
            if(!index) { module_copy(out,r->name,MODULE_NAME_LEN); found=true; }
            else --index;
            break;
        }
    kernel_spin_unlock_irqrestore(&module_table_lock,flags);
    return found ? 0 : -ENOENT;
}
