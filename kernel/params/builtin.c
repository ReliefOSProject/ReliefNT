/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <linux/errno.h>

extern const struct module_parameter __start___param[] __attribute__((weak));
extern const struct module_parameter __stop___param[] __attribute__((weak));
static struct parameter_values builtin_values;

/** @brief Borrow the permanent linker-selected parameter table.
 * @return Execution-owned singleton; no locks, allocation or pin transfer.
 */
struct parameter_values *parameter_builtin_values(void)
{
    builtin_values.records=__start___param;
    builtin_values.count=__start___param && __stop___param ? (size_t)(__stop___param-__start___param) : 0;
    return &builtin_values;
}
/** @brief Compare the module prefix of a fully qualified builtin name.
 * @param record Borrowed descriptor name containing its module prefix.
 * @param name Borrowed terminated module name.
 * @return Whether the complete prefix matches; no ownership or locks.
 */
static bool builtin_prefix(const char *record,const char *name)
{
    while(*record && *record!='.' && *name) {
        char a=*record=='-' ? '_' : *record,b=*name=='-' ? '_' : *name;
        ++record;++name;
        if(a!=b)return false;
    }
    return *record=='.' && !*name;
}
/** @brief Locate the first builtin descriptor belonging to a module.
 * @param name Borrowed terminated module name.
 * @return Permanent descriptor or NULL; no locks or allocation.
 */
const struct module_parameter *parameter_builtin_first(const char *name)
{
    if(!name)return NULL;
    struct parameter_values *values=parameter_builtin_values();
    for(size_t i=0;i<values->count;++i)
        if(builtin_prefix(values->records[i].name,name))return &values->records[i];
    return NULL;
}
/** @brief Test whether any permanent builtin descriptor uses this name.
 * @param name Borrowed terminated module name.
 * @return Presence, including invisible descriptors; no ownership effects.
 */
bool module_parameter_builtin_exists(const char *name)
{return parameter_builtin_first(name)!=NULL;}
/** @brief Enumerate distinct permanent modules with visible parameters.
 * @param index Zero-based visible module index.
 * @param out Writable MODULE_NAME_LEN-byte snapshot.
 * @return Zero or ENOENT/EINVAL; no allocation, locks or borrowed output.
 */
int module_parameter_builtin_at(size_t index,char out[MODULE_NAME_LEN])
{
    if(!out)return -EINVAL;
    struct parameter_values *values=parameter_builtin_values();
    for(size_t i=0;i<values->count;++i) {
        const struct module_parameter *record=&values->records[i];
        if(!record->permissions)continue;
        size_t length=0;while(record->name[length] && record->name[length]!='.')++length;
        if(!length || !record->name[length] || length>=MODULE_NAME_LEN)continue;
        __builtin_memcpy(out,record->name,length);out[length]=0;
        bool prior=false;
        for(size_t j=0;j<i;++j)
            if(values->records[j].permissions && builtin_prefix(values->records[j].name,out)){prior=true;break;}
        if(!prior && !index--)return 0;
    }
    return -ENOENT;
}
/** @brief Parse builtin module.name options before their initialization.
 * @param arguments Borrowed nullable boot command line.
 * @return Zero or negative parse/OOM errno. Boot task context after heap
 * initialization; strings remain owned for the kernel lifetime, no locks.
 */
int module_parameters_boot(const char *arguments)
{return parameter_arguments(parameter_builtin_values(),arguments,true);}
