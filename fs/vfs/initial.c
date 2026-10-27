/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <reliefnt/permissions.h>
#include <linux/stat.h>

static struct vfs_namespace system_namespace;
static bool system_published;

/** @brief Borrow the published initial namespace without taking a reference.
 * @return Shared live namespace, or NULL before publication/after teardown.
 * Caller owns execution; no allocation, lock or callback occurs. Paths retain
 * their own mounts independently of this borrowed namespace pointer.
 */
struct vfs_namespace *vfs_system_namespace(void)
{return system_published && system_namespace.root ? &system_namespace : NULL;}

/** @brief Resolve an initial attachment and optionally create its final target.
 * @param root Retained shared namespace root, borrowed for synchronous lookup.
 * @param request Borrowed boot descriptor, valid for this call.
 * @param point Empty output receiving one owned directory path on success.
 * @return Zero or negative lookup/type/creation errno. Existing metadata is
 * preserved; a new target is root-owned 0755. Missing parents are errors. All
 * temporary paths retire on failure; a successfully created empty directory
 * may remain. Exclusive boot execution; no graph/module lock spans callbacks.
 */
static int initial_mountpoint(const struct vfs_path *root,
                              const struct vfs_boot_mount *request,struct vfs_path *point)
{
    struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_FOLLOW|
        (request->create_directory ? VFS_LOOKUP_MISSING : 0)};
    struct vfs_lookup_result found={0};
    int rc=vfs_path_resolve(&system_namespace,root,root,request->target,&policy,&found);
    if(!rc && found.missing) {
        const struct reliefos_permissions mode={0755,0,0};
        rc=vfs_path_create_node(&found.path,found.last,LINUX_S_IFDIR,&mode,NULL,point);
    } else if(!rc) {
        *point=found.path;found.path=(struct vfs_path){0};
    }
    vfs_path_put(&found.path);
    if(!rc && vfs_path_node(point)->type!=RELIEFOS_FS_TYPE_DIR)rc=-ENOTDIR;
    if(rc)vfs_path_put(point);
    return rc;
}

/** @brief Publish one initial namespace after all actual attachments succeed.
 * @param create_root Factory transferring a detached owned root on success.
 * @param root_context Borrowed factory context.
 * @param mounts Ordered borrowed descriptors; parents precede children.
 * @param count Descriptor count; zero permits NULL mounts.
 * @return Zero or negative errno. Failures retire all temporary mounts/paths;
 * newly created empty target directories can remain. Existing publication is
 * EBUSY. Exclusive early boot execution; no graph/module locks across I/O.
 */
int vfs_system_bootstrap(int (*create_root)(void *,struct vfs_mount **),void *root_context,
                            const struct vfs_boot_mount *mounts,size_t count)
{
    if(system_namespace.root)return -EBUSY;
    if(!create_root || (count && !mounts))return -EINVAL;
    for(size_t i=0;i<count;++i)
        if(!mounts[i].create || !mounts[i].target || mounts[i].target[0]!='/')return -EINVAL;
    system_published=false;
    struct vfs_mount *mount=NULL;
    int rc=create_root(root_context,&mount);
    if(!rc)rc=vfs_namespace_init(&system_namespace,mount);
    vfs_mount_put(mount);
    for(size_t i=0;!rc && i<count;++i) {
        struct vfs_path root={0},point={0};
        rc=vfs_namespace_root(&system_namespace,&root);
        if(!rc)rc=initial_mountpoint(&root,&mounts[i],&point);
        mount=NULL;
        if(!rc && point.dentry->node.type!=RELIEFOS_FS_TYPE_DIR)rc=-ENOTDIR;
        if(!rc)rc=mounts[i].create(mounts[i].context,&mount);
        if(!rc)rc=vfs_mount_attach(&system_namespace,mount,&point);
        vfs_mount_put(mount);vfs_path_put(&point);vfs_path_put(&root);
    }
    if(rc) {
        int cleanup=vfs_namespace_destroy(&system_namespace);
        return cleanup ? cleanup : rc;
    }
    system_published=true;return 0;
}
