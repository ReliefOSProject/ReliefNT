/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "../../block/core/storage_core_api.h"
#include "devtmpfs_internal.h"
#include <reliefnt/devfs.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

/** @brief Allocate one intrinsic inventory entry with root-owned defaults.
 * @param path Absolute intrinsic /dev path. @param native Optional provider node.
 * @param target Optional symlink payload. @param out Owned result on success.
 * @return Zero or allocation/type errno. No device is opened or retained.
 */
static int devtmpfs_snapshot_node(const char *path,const struct storage_node *native,
                                   const char *target,struct devtmpfs_node **out)
{
    struct devtmpfs_node *node=kernel_malloc(sizeof(*node));
    if(!node)return -ENOMEM;
    *node=(struct devtmpfs_node){0};
    __builtin_memcpy(node->path,path,__builtin_strlen(path)+1);
    if(target) {
        node->mode=LINUX_S_IFLNK|0777;
        __builtin_memcpy(node->target,target,__builtin_strlen(target)+1);
    } else if(native->type==RELIEFOS_FS_TYPE_DIR) {
        node->mode=LINUX_S_IFDIR|0755;
        if(!__builtin_strcmp(path,"/dev/shm"))node->mode=LINUX_S_IFDIR|01777;
    } else if(native->type==RELIEFOS_FS_TYPE_SYMLINK) {
        node->mode=LINUX_S_IFLNK|0777;
        int rc=storage_devlink_target(path,node->target);
        if(rc){kernel_free(node);return rc;}
    } else if(native->type==RELIEFOS_FS_TYPE_DEVICE) {
        uint32_t type=native->flags&STORAGE_NODE_FLAG_DEV_BLOCK ? LINUX_S_IFBLK : LINUX_S_IFCHR;
        struct linux_stat_abi stat={.st_mode=type};
        devfs_stat_identity(&stat,native);
        node->device=stat.st_rdev;
        node->mode=type|0600;
        if(!__builtin_strcmp(path,"/dev/null") || !__builtin_strcmp(path,"/dev/zero") ||
           !__builtin_strcmp(path,"/dev/full") || !__builtin_strcmp(path,"/dev/random") ||
           !__builtin_strcmp(path,"/dev/urandom") || !__builtin_strcmp(path,"/dev/tty") ||
           !__builtin_strcmp(path,"/dev/ptmx"))node->mode=type|0666;
    } else {
        kernel_free(node);
        return -EOPNOTSUPP;
    }
    *out=node;
    return 0;
}

/** @brief Collect a provider directory in preorder, with no namespace lookup.
 * @param path Intrinsic provider directory. @param directory Borrowed metadata.
 * @param tail List tail insertion cursor. @return Zero or negative errno.
 * Provider directories have at most two nested levels. pts and shm are writable
 * mountpoints whose dynamic contents belong to their own filesystems.
 */
static int devtmpfs_snapshot_directory(const char *path,const struct storage_node *directory,
                                       struct devtmpfs_node ***tail)
{
    uint64_t cursor=0;
    struct reliefos_dir_entry entry;
    int rc;
    while((rc=storage_devfs_readdir_node(directory,&cursor,&entry))>0) {
        if(!__builtin_strcmp(entry.name,".") || !__builtin_strcmp(entry.name,".."))continue;
        size_t length=0,base=__builtin_strlen(path);
        while(length<sizeof(entry.name) && entry.name[length] && entry.name[length]!='/')++length;
        if(!length || length==sizeof(entry.name) || entry.name[length])return -EIO;
        char child[RELIEFOS_FS_PATH_LEN];
        if(base+length+2>sizeof(child))return -ENAMETOOLONG;
        __builtin_memcpy(child,path,base);
        child[base]='/';
        __builtin_memcpy(child+base+1,entry.name,length+1);
        const char *target=NULL;
        if(!__builtin_strcmp(child,"/dev/stdin"))target="/proc/self/fd/0";
        if(!__builtin_strcmp(child,"/dev/stdout"))target="/proc/self/fd/1";
        if(!__builtin_strcmp(child,"/dev/stderr"))target="/proc/self/fd/2";
        struct storage_node native={.type=entry.type};
        if(!target && __builtin_strcmp(child,"/dev/shm")) {
            rc=storage_devfs_lookup(child,&native);
            if(rc==-ENOENT || rc==-ENODEV)continue;
            if(rc)return rc;
        }
        struct devtmpfs_node *node=NULL;
        rc=devtmpfs_snapshot_node(child,&native,target,&node);
        if(rc==-ENOENT || rc==-ENODEV)continue;
        if(rc)return rc;
        **tail=node;
        *tail=&node->next;
        if(native.type==RELIEFOS_FS_TYPE_DIR && __builtin_strcmp(child,"/dev/pts") &&
           __builtin_strcmp(child,"/dev/shm")) {
            rc=devtmpfs_snapshot_directory(child,&native,tail);
            if(rc)return rc;
        }
    }
    return rc;
}

/** @brief Free an inventory list and release each retained backing inode.
 * @param fs Owning tmpfs, or NULL for snapshots without inode references.
 * @param nodes Owned list, consumed even when empty. Execution ownership held.
 */
void devtmpfs_nodes_free(struct tmpfs_super *fs,struct devtmpfs_node *nodes)
{
    while(nodes) {
        struct devtmpfs_node *next=nodes->next;
        if(fs && nodes->ino)tmpfs_drop(fs,nodes->ino);
        kernel_free(nodes);
        nodes=next;
    }
}

/** @brief Collect the complete intrinsic device inventory before publication.
 * @param out Owned preorder list on success, NULL on failure.
 * @return Zero or negative errno. Execution ownership serializes enumeration.
 */
int devtmpfs_snapshot(struct devtmpfs_node **out)
{
    *out=NULL;
    struct storage_node root;
    int rc=storage_devfs_lookup("/dev",&root);
    struct devtmpfs_node **tail=out;
    if(!rc)rc=devtmpfs_snapshot_directory("/dev",&root,&tail);
    if(!rc)rc=devtmpfs_snapshot_node("/dev/fd",NULL,"/proc/self/fd",tail);
    if(rc){devtmpfs_nodes_free(NULL,*out);*out=NULL;}
    return rc;
}

/** @brief Resolve a parent using only actual tmpfs directory inode identities.
 * @param fs Serialized backing. @param path Intrinsic /dev path.
 * @param parent Borrowed parent result. @param name Last component output.
 * @return Zero or negative errno. User-created parent symlinks are never followed.
 */
static int devtmpfs_parent(struct tmpfs_super *fs,const char *path,
                           struct storage_node *parent,char name[RELIEFOS_FS_NAME_LEN])
{
    int rc=tmpfs_lookup(fs,"/",parent);
    const char *component=path+5;
    while(!rc) {
        size_t length=0;
        while(component[length] && component[length]!='/')++length;
        if(!length || length>=RELIEFOS_FS_NAME_LEN)return -ENAMETOOLONG;
        __builtin_memcpy(name,component,length);
        name[length]=0;
        if(!component[length])return 0;
        struct storage_node child;
        rc=tmpfs_lookup_child(fs,parent->first_cluster,name,&child);
        if(rc)return rc;
        if(child.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
        *parent=child;
        component+=length+1;
    }
    return rc;
}

/** @brief Check kernel ownership by held inode identity, type and device number.
 * @param fs Serialized backing. @param owned Managed inventory record.
 * @param current Borrowed candidate inode. @return True only for our exact inode.
 */
static bool devtmpfs_matches(struct tmpfs_super *fs,const struct devtmpfs_node *owned,
                              const struct storage_node *current)
{
    if(!owned->ino || current->first_cluster!=owned->ino)return false;
    struct linux_stat_abi stat;
    return !tmpfs_stat(fs,owned->ino,&stat) &&
        (stat.st_mode&LINUX_S_IFMT)==(owned->mode&LINUX_S_IFMT) &&
        stat.st_rdev==owned->device;
}

/** @brief Remove a disappeared device only if its owned inode still occupies it.
 * @param fs Serialized backing. @param owned Record retaining its inode.
 * @return Zero or negative errno. Replacements and nonempty directories survive.
 */
static int devtmpfs_remove(struct tmpfs_super *fs,const struct devtmpfs_node *owned)
{
    struct storage_node parent,current;
    char name[RELIEFOS_FS_NAME_LEN];
    int rc=devtmpfs_parent(fs,owned->path,&parent,name);
    if(rc==-ENOENT || rc==-ENOTDIR)return 0;
    if(rc)return rc;
    rc=tmpfs_lookup_child(fs,parent.first_cluster,name,&current);
    if(rc==-ENOENT)return 0;
    if(rc)return rc;
    if(!devtmpfs_matches(fs,owned,&current))return 0;
    rc=tmpfs_unlink_child(fs,parent.first_cluster,name,(owned->mode&LINUX_S_IFMT)==LINUX_S_IFDIR);
    return rc==-ENOTEMPTY ? 0 : rc;
}

/** @brief Publish a new inventory entry unless userspace owns its name or parent.
 * @param fs Serialized backing. @param node New record receiving an inode hold.
 * @return Zero or negative errno. Existing names are recorded without ownership.
 */
static int devtmpfs_add(struct tmpfs_super *fs,struct devtmpfs_node *node)
{
    struct storage_node parent,current;
    char name[RELIEFOS_FS_NAME_LEN];
    int rc=devtmpfs_parent(fs,node->path,&parent,name);
    if(rc==-ENOENT || rc==-ENOTDIR)return 0;
    if(rc)return rc;
    rc=tmpfs_lookup_child(fs,parent.first_cluster,name,&current);
    if(!rc)return 0;
    if(rc!=-ENOENT)return rc;
    struct reliefos_permissions permissions={.mode=node->mode&07777,.uid=0,.gid=0};
    uint32_t type=node->mode&LINUX_S_IFMT;
    if(type==LINUX_S_IFCHR || type==LINUX_S_IFBLK)
        rc=tmpfs_mknod_child(fs,parent.first_cluster,name,node->mode,node->device,&permissions,&current);
    else rc=tmpfs_create_child(fs,parent.first_cluster,name,node->mode,
                                type==LINUX_S_IFLNK ? node->target : NULL,&permissions,&current);
    if(rc)return rc;
    rc=tmpfs_hold(fs,current.first_cluster);
    if(!rc)node->ino=current.first_cluster;
    return rc;
}

/** @brief Apply a complete device snapshot to retained backing inode records.
 * @param fs Serialized tmpfs backing. @param managed Owned persistent records.
 * @param snapshot Complete inventory, consumed on every return path.
 * @return Zero or negative errno. User metadata and replacement inodes survive.
 * Partial failures retain ownership records so the next refresh can retry.
 */
int devtmpfs_reconcile(struct tmpfs_super *fs,struct devtmpfs_node **managed,
                       struct devtmpfs_node *snapshot)
{
    for(struct devtmpfs_node *old=*managed;old;old=old->next)old->seen=false;
    int rc=0;
    while(snapshot) {
        struct devtmpfs_node *node=snapshot;
        struct devtmpfs_node *old=*managed;
        while(old && __builtin_strcmp(old->path,node->path))old=old->next;
        if(old && (old->mode&LINUX_S_IFMT)==(node->mode&LINUX_S_IFMT) && old->device==node->device) {
            if((node->mode&LINUX_S_IFMT)==LINUX_S_IFLNK && __builtin_strcmp(old->target,node->target)) {
                struct storage_node parent,current;
                char name[RELIEFOS_FS_NAME_LEN];
                rc=devtmpfs_parent(fs,old->path,&parent,name);
                if(!rc)rc=tmpfs_lookup_child(fs,parent.first_cluster,name,&current);
                if(!rc && devtmpfs_matches(fs,old,&current))rc=tmpfs_rewrite_symlink(fs,old->ino,node->target);
                if(rc==-ENOENT || rc==-ENOTDIR)rc=0;
                if(rc)break;
                __builtin_memcpy(old->target,node->target,__builtin_strlen(node->target)+1);
            }
            old->seen=true;
        } else {
            if(old) {
                rc=devtmpfs_remove(fs,old);
                if(rc)break;
            }
            rc=devtmpfs_add(fs,node);
            if(rc)break;
        }
        snapshot=node->next;
        if(!old || !old->seen) {
            node->seen=true;
            node->next=*managed;
            *managed=node;
        } else kernel_free(node);
    }
    devtmpfs_nodes_free(NULL,snapshot);
    if(rc)return rc;
    /* Records are prepended as parents/children are created, so children retire
     * first. An inode hold prevents reuse from confusing deletion ownership.
     */
    struct devtmpfs_node **link=managed;
    while(*link) {
        struct devtmpfs_node *old=*link;
        if(old->seen){link=&old->next;continue;}
        rc=devtmpfs_remove(fs,old);
        if(rc)return rc;
        *link=old->next;
        old->next=NULL;
        devtmpfs_nodes_free(fs,old);
    }
    return 0;
}
