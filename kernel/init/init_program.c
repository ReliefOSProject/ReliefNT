/* SPDX-License-Identifier: Apache-2.0 */
#include "init_program.h"
#include <linux/errno.h>

/** @brief Decode a bounded absolute PID1 path without confusing init and rdinit.
 * @param cmdline Borrowed boot text, nullable.
 * @param memory_root True for initial-memory-root boot, false for direct root.
 * @param out Required writable output. @param capacity Available output bytes.
 * @return Zero or EINVAL/ENAMETOOLONG. Last selected override wins; the other
 * boot stage's argument is ignored. Pure, no locks/allocation/I/O/retention.
 */
int init_program_path(const char *cmdline,bool memory_root,char *out,size_t capacity)
{
    if(!out || !capacity)return -EINVAL;
    const char *value=memory_root ? "/init" : "/sbin/init";
    size_t length=__builtin_strlen(value),key=memory_root ? 7 : 5;
    const char *prefix=memory_root ? "rdinit=" : "init=";
    for(const char *cursor=cmdline ? cmdline : "";*cursor;) {
        while(*cursor==' ' || *cursor=='\t')++cursor;
        const char *end=cursor;while(*end && *end!=' ' && *end!='\t')++end;
        if((size_t)(end-cursor)>=key && !__builtin_memcmp(cursor,prefix,key)) {
            value=cursor+key;length=(size_t)(end-value);
        }
        cursor=end;
    }
    if(!length || value[0]!='/')return -EINVAL;
    if(length>=capacity)return -ENAMETOOLONG;
    __builtin_memcpy(out,value,length);out[length]=0;return 0;
}
