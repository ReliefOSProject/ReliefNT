/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <linux/errno.h>
#ifdef CONFIG_MODULES
#include "../module/internal.h"
#endif

/** @brief Borrow parameters only while retaining their owning LIVE image.
 * @param name Terminated module name.
 * @param owner Required pin output, NULL for permanent builtin data.
 * @return Borrowed table or NULL. Execution ownership required; no registry
 * lock is retained after module_get_by_name and no allocation occurs.
 */
static struct parameter_values *parameters_get(const char *name,struct module **owner)
{
    *owner=NULL;
    if(module_parameter_builtin_exists(name))return parameter_builtin_values();
#ifdef CONFIG_MODULES
    *owner=module_get_by_name(name);
    if(*owner)return &((struct module_runtime *)(*owner)->runtime)->parameters;
#endif
    return NULL;
}
/** @brief Select the local name from a descriptor owned by this module.
 * @param record Retained descriptor.
 * @param module Borrowed queried module name.
 * @param builtin Whether descriptors use fully qualified names.
 * @return Borrowed local name or NULL; no allocation or locks.
 */
static const char *parameter_local(const struct module_parameter *record,const char *module,bool builtin)
{
    const char *name=record->name;
    if(!builtin)return name;
    while(*name && *name!='.' && *module) {
        char a=*name=='-' ? '_' : *name,b=*module=='-' ? '_' : *module;
        if(a!=b)return NULL;
        ++name;++module;
    }
    return *name=='.' && !*module ? name+1 : NULL;
}
/** @brief Find one visible descriptor in a caller-pinned table.
 * @param values Stable retained table.
 * @param module Borrowed module name.
 * @param name Borrowed local parameter name.
 * @param builtin Whether table holds fully qualified builtin records.
 * @return Borrowed matching descriptor or NULL. Execution ownership required.
 */
static const struct module_parameter *parameter_find(struct parameter_values *values,const char *module,const char *name,bool builtin)
{
    for(size_t i=0;i<values->count;++i) {
        const struct module_parameter *record=&values->records[i];
        const char *local=parameter_local(record,module,builtin);
        if(record->permissions && local && parameter_name_equal(local,name))return record;
    }
    return NULL;
}
/** @brief Copy one visible parameter descriptor with balanced image pinning.
 * @param module Borrowed terminated module name.
 * @param index Zero-based visible attribute index.
 * @param out Required caller-owned snapshot.
 * @return Zero or negative errno. Execution-owned task context; no borrowed
 * runtime data escapes and no allocation/callback spans the registry lock.
 */
int module_parameter_at(const char *module,size_t index,struct module_parameter_info *out)
{
    if(!module || !out)return -EINVAL;
    struct module *owner;struct parameter_values *values=parameters_get(module,&owner);
    if(!values)return -ENOENT;
    int rc=-ENOENT;
    for(size_t i=0;i<values->count;++i) {
        const struct module_parameter *record=&values->records[i];
        const char *name=parameter_local(record,module,!owner);
        if(!record->permissions || !name || index--)continue;
        size_t length=0;while(name[length])++length;
        if(length>=sizeof(out->name)){rc=-ENAMETOOLONG;break;}
        __builtin_memcpy(out->name,name,length+1);out->permissions=record->permissions;rc=0;break;
    }
    module_put(owner);return rc;
}
/** @brief Format a visible parameter with its exact LIVE image pinned.
 * @param module Borrowed module name.
 * @param name Borrowed local parameter name.
 * @param offset Byte offset.
 * @param buffer Output bytes, NULL only for zero capacity.
 * @param capacity Output bound.
 * @param written Required byte count, cleared on error.
 * @return Zero or negative errno. Execution ownership required; formatting
 * runs after the registry lock has been released and the pin is balanced.
 */
int module_parameter_read(const char *module,const char *name,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *written)
{
    if(!written)return -EINVAL;*written=0;
    if(!module || !name)return -EINVAL;
    struct module *owner;struct parameter_values *values=parameters_get(module,&owner);
    if(!values)return -ENOENT;
    const struct module_parameter *record=parameter_find(values,module,name,!owner);
    int rc=!record ? -ENOENT : !(record->permissions&0444u) ? -EIO :
        parameter_value_read(record,offset,buffer,capacity,written);
    module_put(owner);return rc;
}
/** @brief Copy and apply one complete sysfs write without losing old strings.
 * @param module Borrowed module name.
 * @param name Borrowed local parameter name.
 * @param buffer Readable payload, NULL only for zero length.
 * @param length Byte count.
 * @param written Required output, full length on success.
 * @return Zero or negative errno. Execution ownership required; image stays
 * pinned through allocation/store/free, with no module registry lock held.
 */
int module_parameter_write(const char *module,const char *name,const void *buffer,uint32_t length,uint32_t *written)
{
    if(!written)return -EINVAL;*written=0;
    if(!module || !name || (!buffer && length))return -EINVAL;
    /* sysfs has a PAGE_SIZE store buffer; charp applies its own tighter bound. */
    if(length>=4096)return -E2BIG;
    struct module *owner;struct parameter_values *values=parameters_get(module,&owner);
    if(!values)return -ENOENT;
    const struct module_parameter *record=parameter_find(values,module,name,!owner);
    int rc=record ? 0 : -ENOENT;
    if(!rc && !(record->permissions&0222u))rc=-EIO;
    char *text=NULL;
    if(!rc && !(text=kernel_malloc((size_t)length+1)))rc=-ENOMEM;
    if(!rc) {
        if(length)__builtin_memcpy(text,buffer,length);text[length]=0;
        rc=parameter_value_set(values,record,text);
        if(!rc)*written=length;
    }
    kernel_free(text);module_put(owner);return rc;
}
