/* SPDX-License-Identifier: Apache-2.0 */
#include "runtime.h"
#include <reliefnt/permissions.h>
#include <linux/errno.h>
#include <linux/stat.h>

/** @brief Find or create one boot directory with a retained actual path.
 * @param ns Borrowed boot namespace under exclusive execution.
 * @param root Borrowed visible root constraining lookup.
 * @param name Constant kernel pathname, required; final links are rejected.
 * @param mode Required root-owned directory permission bits.
 * @param out Required empty owned path output, empty on failure.
 * @return Zero or negative lookup/type/create/metadata errno. New and existing
 * directories receive the same metadata. No graph/module lock spans I/O;
 * every temporary parent/inode reference retires on all outcomes.
 */
static int runtime_directory(struct vfs_namespace *ns,const struct vfs_path *root,
                                const char *name,uint32_t mode,struct vfs_path *out)
{
    struct vfs_lookup_result found={0};
    const struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_MISSING};
    struct reliefos_permissions permissions={mode,0,0};
    int rc=vfs_path_resolve(ns,root,root,name,&policy,&found);
    if(!rc) {
        if(found.missing)rc=vfs_path_create_node(&found.path,found.last,LINUX_S_IFDIR,&permissions,NULL,out);
        else if(vfs_path_node(&found.path)->type!=RELIEFOS_FS_TYPE_DIR)rc=-ENOTDIR;
        else vfs_path_get(&found.path,out);
    }
    vfs_path_put(&found.path);
    if(!rc)rc=vfs_path_permissions(out,&permissions,true);
    if(rc)vfs_path_put(out);return rc;
}

/** @brief Clear an actual boot tree without following links or other mounts.
 * @param ns Borrowed initialized boot namespace.
 * @param base Borrowed retained directory, kept as the traversal boundary.
 * @return Zero or negative enumeration/lookup/mutation/type/I/O/OOM errno.
 * Holds only current/base paths; cached dentry ancestry replaces a recursive
 * stack or concatenated pathname. Each deletion restarts native enumeration,
 * allowing coalescing directory records. An attachment boundary returns EXDEV,
 * even when both mounts share the same backend. Exclusive boot execution is
 * required; no graph/module lock spans callbacks, all owned paths retire.
 */
static int runtime_clear_tree(struct vfs_namespace *ns,const struct vfs_path *base)
{
    struct vfs_path current={0};vfs_path_get(base,&current);int rc=0;
    for(;;) {
        struct reliefos_dir_entry entry;uint64_t cookie=0;
        do {
            rc=vfs_path_readdir(&current,&cookie,&entry);
        } while(rc>0 && entry.name[0]=='.' &&
            (!entry.name[1] || (entry.name[1]=='.' && !entry.name[2])));
        if(rc<0)break;
        if(!rc) {
            if(vfs_path_equal(&current,base))break;
            struct vfs_path parent={0};rc=vfs_path_lookup(ns,base,&current,"..",0,&parent);
            if(!rc)rc=vfs_path_remove(ns,&current,true);
            if(rc){vfs_path_put(&parent);break;}
            vfs_path_put(&current);current=parent;continue;
        }
        size_t length=0;
        while(length<sizeof(entry.name) && entry.name[length] && entry.name[length]!='/')++length;
        if(!length || length>VFS_NAME_MAX || length==sizeof(entry.name) || entry.name[length]){rc=-EIO;break;}
        struct vfs_path child={0};rc=vfs_path_lookup(ns,base,&current,entry.name,0,&child);
        if(!rc && child.mount!=base->mount)rc=-EXDEV;
        if(!rc && vfs_path_node(&child)->type==RELIEFOS_FS_TYPE_DIR) {
            vfs_path_put(&current);current=child;continue;
        }
        if(!rc)rc=vfs_path_remove(ns,&child,false);
        vfs_path_put(&child);if(rc)break;
    }
    vfs_path_put(&current);return rc;
}

/** @brief Initialize volatile directories using shared namespace objects.
 * @param ns Borrowed fully assembled namespace before the first user process.
 * @return Zero or negative lookup/type/readonly/I/O/allocation errno. Clears
 * the actual /run and /dev/shm trees and creates /run/lock with root ownership.
 * No filesystem type/volume label/text router is consulted. Exclusive boot
 * execution is required; storage is synchronous, no graph/module lock spans
 * callbacks. Every path reference is released before returning.
 */
int userland_prepare_runtime(struct vfs_namespace *ns)
{
    if(!ns || !ns->root)return -EINVAL;
    storage_set_io_async_context(false);
    struct vfs_path root={0},run={0},shm={0},lock={0};
    int rc=vfs_namespace_root(ns,&root);
    if(!rc)rc=runtime_directory(ns,&root,"/run",0755,&run);
    if(!rc)rc=runtime_clear_tree(ns,&run);
    if(!rc)rc=runtime_directory(ns,&root,"/dev/shm",01777,&shm);
    if(!rc)rc=runtime_clear_tree(ns,&shm);
    if(!rc)rc=runtime_directory(ns,&root,"/run/lock",0755,&lock);
    vfs_path_put(&lock);vfs_path_put(&shm);vfs_path_put(&run);vfs_path_put(&root);return rc;
}
