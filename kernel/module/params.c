/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Validate every relocated parameter before publishing its storage.
 * @param view Checked ELF matching the private runtime layout.
 * @param runtime Unpublished runtime receiving a borrowed descriptor table.
 * @param arguments Borrowed terminated module arguments, nullable.
 * @return Zero or negative errno. All names, initial strings and value widths
 * are checked even when no arguments were supplied. Caller owns rollback of
 * replacement strings; no registry lock or module callback spans parsing.
 */
int module_parameters_prepare(const struct module_elf *view,struct module_runtime *runtime,const char *arguments)
{
    size_t bytes=0;
    const struct module_parameter *parameters=module_layout_named(view,&runtime->layout,"__param",&bytes);
    if(bytes%sizeof(*parameters) || (bytes && (uintptr_t)parameters%_Alignof(struct module_parameter)))return -ENOEXEC;
    size_t count=bytes/sizeof(*parameters);
    for(size_t i=0;i<count;++i) {
        const struct module_parameter *parameter=&parameters[i];
        if(!module_layout_string(&runtime->layout,parameter->name) || !*parameter->name ||
           parameter->permissions&~0666u || parameter->permissions&0002u)return -ENOEXEC;
        size_t length=0;
        while(parameter->name[length]) {
            char c=parameter->name[length++];
            if(c=='/' || c=='.' || length>=MODULE_PARAMETER_NAME_LEN)return -ENOEXEC;
        }
        size_t width=parameter_value_width(parameter);
        if(!width || !module_layout_contains(&runtime->layout,(uintptr_t)parameter->value,width,MODULE_ELF_WRITE))return -ENOEXEC;
        if(parameter->type==MODULE_PARAMETER_charp) {
            const char *text;module_copy(&text,parameter->value,sizeof(text));
            if(text && !module_layout_string(&runtime->layout,text))return -ENOEXEC;
        }
        for(size_t j=0;j<i;++j)
            if(parameter_name_equal(parameter->name,parameters[j].name))return -ENOEXEC;
    }
    runtime->parameters.records=parameters;runtime->parameters.count=count;
    return parameter_arguments(&runtime->parameters,arguments,false);
}
