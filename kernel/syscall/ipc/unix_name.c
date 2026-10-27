/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include "unix_path.h"
#include <linux/socket.h>

/** @brief Snapshot counted UNIX address bytes before entering filesystem I/O.
 * @param ns Borrowed shared namespace; unused for unnamed/abstract addresses.
 * @param task Borrowed caller with optional active syscall operand scope.
 * @param address User sockaddr_un input checked for the entire counted range.
 * @param length Byte count including family, bounded by sockaddr_un.
 * @param create True for pathname bind, false for a pathname peer lookup.
 * @param out Empty output owning a path for filesystem addresses only.
 * @return Zero or negative errno with empty output on error. Embedded NUL
 * bytes in abstract names survive unchanged; user bytes belong to the scope
 * or are freed before provider I/O. Caller puts out->object. No locks span I/O.
 */
int syscall_unix_address(struct vfs_namespace *ns,struct task *task,uint64_t address,
                             uint32_t length,bool create,struct unix_address *out)
{
    if(!out)return -RELIEFOS_EINVAL;*out=(struct unix_address){0};
    if(length<sizeof(sa_family_t) || length>sizeof(struct sockaddr_un))return -RELIEFOS_EINVAL;
    struct task_user_text text={0};
    int rc=task_lookup_user_bytes(task,address,length,&text);if(rc)return rc;
    struct sockaddr_un input={0};__builtin_memcpy(&input,text.text,length);kernel_free(text.allocation);
    if(input.sun_family!=AF_UNIX)return -RELIEFOS_EINVAL;
    struct unix_address result={.length=length-sizeof(sa_family_t)};
    __builtin_memcpy(result.path,input.sun_path,result.length);
    if(result.length && result.path[0]) {
        uint32_t used=0;while(used<result.length && result.path[used])++used;
        result.path[used]=0;result.length=used+1;
        rc=syscall_unix_path(ns,task,result.path,create,&result.object);
    }
    if(!rc)*out=result;
    return rc;
}
