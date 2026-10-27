/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <linux/errno.h>

/** @brief Recognize ASCII argument separators without locale state.
 * @param c Input byte.
 * @return Whether the byte separates unquoted tokens; no side effects.
 */
static bool parameter_space(char c)
{return c==' ' || c=='\t' || c=='\n' || c=='\r' || c=='\f' || c=='\v';}
/** @brief Apply Linux-style quoted tokens to a retained parameter table.
 * @param values Exclusively owned validated descriptor/string table.
 * @param arguments Borrowed terminated input, nullable.
 * @param ignore_unknown True for boot options belonging to other parsers.
 * @return Zero or negative errno. Task/execution ownership required; all
 * temporary storage is freed, with no registry lock across allocation/store.
 */
int parameter_arguments(struct parameter_values *values,const char *arguments,bool ignore_unknown)
{
    if(!arguments || !*arguments)return 0;
    size_t length=0;while(arguments[length]){if(length==SIZE_MAX-1)return -E2BIG;++length;}
    char *copy=kernel_malloc(length+1);if(!copy)return -ENOMEM;
    __builtin_memcpy(copy,arguments,length+1);char *cursor=copy;int rc=0;
    while(*cursor) {
        while(parameter_space(*cursor))++cursor;
        if(!*cursor)break;
        char *token=cursor,*write=cursor;bool quoted=false;
        while(*cursor && (quoted || !parameter_space(*cursor))) {
            char c=*cursor++;if(c=='"')quoted=!quoted;else *write++=c;
        }
        if(quoted){rc=-EINVAL;break;}
        char *next=cursor;if(*next)++next;*write=0;cursor=next;
        if(token[0]=='-' && token[1]=='-' && !token[2])break;
        char *value=token;while(*value && *value!='=')++value;
        if(*value)*value++=0;else value=NULL;
        size_t i=0;while(i<values->count && !parameter_name_equal(token,values->records[i].name))++i;
        if(i==values->count){if(ignore_unknown)continue;rc=-ENOENT;break;}
        rc=parameter_value_set(values,&values->records[i],value);if(rc)break;
    }
    kernel_free(copy);return rc;
}
