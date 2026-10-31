/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/pseudo_fs.h>
#include <reliefnt/heap.h>
#include <reliefnt/module.h>
#include <linux/errno.h>
#include "mount_internal.h"
#ifdef CONFIG_LOCK_STAT
#include "../proc/internal.h"
#endif

struct pseudo_key {
    struct pseudo_key *next;
    void *identity;
    uint64_t references,id;
    struct storage_node node;
    char name[];
};
struct pseudo_mount {const struct vfs_pseudo_provider *provider;struct pseudo_key *keys,*pending;uint64_t next_id;};

static struct pseudo_key *pseudo_key_find(struct pseudo_mount *context,uint64_t id)
{
    for(struct pseudo_key *key=context->keys;key;key=key->next)if(key->id==id)return key;
    return NULL;
}
static bool pseudo_key_live(const struct pseudo_mount *context,const struct pseudo_key *key)
{return key && (!key->identity || context->provider->identity_alive(key->identity));}
static void pseudo_identity_put(const struct pseudo_mount *context,void *identity)
{if(identity)context->provider->put_identity(identity);}
static void pseudo_key_free(struct pseudo_mount *context,struct pseudo_key *key)
{
    struct pseudo_key **link=&context->keys;
    while(*link && *link!=key)link=&(*link)->next;
    if(*link)*link=key->next;
    if(context->pending==key)context->pending=NULL;
    pseudo_identity_put(context,key->identity);kernel_free(key);
}

/* lookup returns borrowed metadata. Keep at most one unretained result for
 * the immediate dentry-retain step, including allocation-failure rollback.
 * Live inode keys have no fixed capacity and retain provider identity pins. */
static int pseudo_key_lookup(struct pseudo_mount *context,const char *name,struct storage_node *out)
{
    const struct vfs_pseudo_provider *provider=context->provider;
    struct storage_node node;int rc=provider->lookup(name,&node);if(rc)return rc;
    void *identity=NULL;bool magic=false;
    if(provider->get_identity) {
        rc=provider->get_identity(name,&identity,&magic);if(rc)return rc;
    }
    for(struct pseudo_key *key=context->keys;key;key=key->next)
        if(key->identity==identity && !__builtin_strcmp(key->name,name)) {
            pseudo_identity_put(context,identity);node.entry_id=key->id;
            if(magic)node.flags|=STORAGE_NODE_FLAG_MAGIC_LINK;
            key->node=node;*out=node;return 0;
        }
    if(context->pending)pseudo_key_free(context,context->pending);
    if(!context->next_id){pseudo_identity_put(context,identity);return -EOVERFLOW;}
    size_t length=__builtin_strlen(name);
    struct pseudo_key *key=kernel_malloc(sizeof(*key)+length+1);
    if(!key){pseudo_identity_put(context,identity);return -ENOMEM;}
    node.entry_id=context->next_id++;
    if(magic)node.flags|=STORAGE_NODE_FLAG_MAGIC_LINK;
    *key=(struct pseudo_key){.next=context->keys,.identity=identity,.id=node.entry_id,.node=node};
    __builtin_memcpy(key->name,name,length+1);
    context->keys=key;context->pending=key;*out=node;return 0;
}
static int pseudo_node_lookup(void *value,const struct storage_node *parent,
                              const char *name,struct storage_node *out)
{
    struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,parent->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    size_t prefix=__builtin_strlen(key->name),length=__builtin_strlen(name);
    if(prefix+length+1>=VFS_PATH_MAX)return -ENAMETOOLONG;
    char *joined=kernel_malloc(prefix+length+2);if(!joined)return -ENOMEM;
    __builtin_memcpy(joined,key->name,prefix);joined[prefix]='/';
    __builtin_memcpy(joined+prefix+1,name,length+1);
    int rc=pseudo_key_lookup(context,joined,out);kernel_free(joined);return rc;
}
static int pseudo_node_retain(void *value,const struct storage_node *node,void **out)
{
    struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    ++key->references;if(context->pending==key)context->pending=NULL;*out=key;return 0;
}
static void pseudo_node_release(void *value,const struct storage_node *node,void *pin)
{
    (void)node;struct pseudo_key *key=pin;
    if(!--key->references)pseudo_key_free(value,key);
}
static bool pseudo_node_same(void *value,const struct storage_node *left,const struct storage_node *right)
{(void)value;return left->entry_id==right->entry_id;}
static int pseudo_node_readlink(void *value,const struct storage_node *node,
                                char *buffer,size_t capacity,size_t *length)
{
    *length=0;struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    if(capacity>UINT32_MAX)return -EINVAL;
    if(!context->provider->readlink)return -EINVAL;
    int rc=context->provider->readlink(key->name,buffer,(uint32_t)capacity);
    if(rc<0)return rc;
    *length=(size_t)rc;return 0;
}
static int pseudo_node_follow(void *value,const struct storage_node *node,struct vfs_path *out)
{
    *out=(struct vfs_path){0};struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->follow ? context->provider->follow(key->name,out) : -EOPNOTSUPP;
}
/** @brief Read a live generated inode without fabricating filesystem atime.
 * @param value Borrowed pinned pseudo context. @param node Retained intrinsic key.
 * @param offset Byte position. @param buffer Destination. @param capacity Available bytes.
 * @param read Required count output. @param mount_flags Current policy, no atime here.
 * @return Provider status, ENOENT for retired identity or EOPNOTSUPP. Serialized
 * execution ownership required; no graph/module lock spans the callback.
 */
static int pseudo_node_read(void *value,const struct storage_node *node,uint64_t offset,
                            void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{
    (void)mount_flags;*read=0;struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->read ? context->provider->read(key->name,offset,buffer,capacity,read) : -EOPNOTSUPP;
}

/** @brief Capture optional generated-file state for an authorized live key.
 * @param value Borrowed pseudo mount context pinned by the caller's path.
 * @param node Retained intrinsic inode key, required.
 * @param flags Sanitized status flags; O_PATH never reaches this callback.
 * @param state Required owned output, NULL for ordinary generated files.
 * @return Zero or negative errno. Any returned state is released by file_close,
 * including on error. Nonblocking, execution-owned; no graph lock is held.
 */
static int pseudo_file_open(void *value,const struct storage_node *node,uint32_t flags,void **state)
{
    *state=NULL;struct pseudo_mount *context=value;
    struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->open ? context->provider->open(key->name,flags,state) : 0;
}

/** @brief Write only through a live retained generated inode's provider.
 * @param value Pinned pseudo mount context.
 * @param node Retained intrinsic inode with original instance token.
 * @param offset Byte position supplied to provider.
 * @param buffer Validated input bytes.
 * @param length Byte count.
 * @param written Required output, cleared on failure.
 * @return Zero or negative errno. Execution ownership required; retired
 * identities never retarget new modules. No graph/module lock spans store.
 */
static int pseudo_node_write(void *value,struct storage_node *node,uint64_t offset,
                                 const void *buffer,uint32_t length,uint32_t *written)
{
    *written=0;struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->write ? context->provider->write(key->name,offset,buffer,length,written) : -EROFS;
}
#ifdef CONFIG_LOCK_STAT
/** @brief Identify the immediate diagnostic control through its held provider.
 * @param path Borrowed valid path with a retained inode and provider context.
 * @return True only for the live proc lock_stats key, never an alias in a
 * different provider. Execution-owned; no lookup, allocation, or I/O. */
bool vfs_pseudo_lock_stats_control(const struct vfs_path *path)
{
    if (!vfs_path_valid(path) || path->mount->ops->write != pseudo_node_write) return false;
    struct pseudo_mount *context = path->mount->context;
    struct pseudo_key *key = pseudo_key_find(context, path->dentry->node.entry_id);
    return pseudo_key_live(context, key) && context->provider->write == proc_lock_stats_write &&
           !__builtin_strcmp(key->name, "/proc/reliefnt/lock_stats");
}
#endif

/** @brief Forward truncation only to the exact live generated provider.
 * @param value Pinned pseudo context.
 * @param node Retained intrinsic inode with original identity.
 * @param length Requested size.
 * @return Provider result or negative errno. Execution ownership required;
 * no namespace replay, callback under graph lock, or new identity lookup.
 */
static int pseudo_node_truncate(void *value,struct storage_node *node,uint64_t length)
{
    struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->truncate ? context->provider->truncate(key->name,length) : -EROFS;
}

/** @brief Read captured state without consulting a subsequently retired PID.
 * @param value Pinned pseudo provider context, borrowed.
 * @param node Intrinsic inode retained by the open handle.
 * @param state Borrowed successful capture, independent of identity liveness.
 * @param offset Byte position in the captured view.
 * @param buffer Validated kernel-accessible destination.
 * @param capacity Available bytes.
 * @param read Required actual byte output, cleared on validation failure.
 * @param mount_flags Current attachment/SB policy; generated views have no atime.
 * @return Provider status or EINVAL for an inconsistent retained handle.
 * Execution ownership required; no graph/module lock spans the callback.
 */
static int pseudo_file_read(void *value,const struct storage_node *node,void *state,
                              uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{
    (void)mount_flags;*read=0;struct pseudo_mount *context=value;
    if(!pseudo_key_find(context,node->entry_id) || !context->provider->read_open)return -EINVAL;
    return context->provider->read_open(state,offset,buffer,capacity,read);
}

/** @brief Consume captured generated-file state before its provider retires.
 * @param value Pinned pseudo provider context, borrowed.
 * @param node Intrinsic inode kept alive by the handle, borrowed.
 * @param state Owned successful or failed-open capture.
 * @return None. Identity may already be dead; release never retargets it.
 * Execution ownership required; no graph/module lock spans final cleanup.
 */
static void pseudo_file_close(void *value,const struct storage_node *node,void *state)
{
    (void)node;struct pseudo_mount *context=value;context->provider->close(state);
}
/** @brief Enumerate a live generated directory through its retained key.
 * @param value Borrowed pinned pseudo context. @param node Retained intrinsic directory.
 * @param cursor Required opaque cursor. @param entry Required entry output.
 * @param mount_flags Current policy; generated directories do not track atime.
 * @return One entry, zero EOF or negative liveness/provider errno. Execution
 * ownership required; no graph/module lock spans callbacks or ownership change.
 */
static int pseudo_node_readdir(void *value,const struct storage_node *node,uint64_t *cursor,
                                  struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)mount_flags;
    struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->readdir ? context->provider->readdir(key->name,cursor,entry) : -EOPNOTSUPP;
}
static void pseudo_context_destroy(void *value)
{
    struct pseudo_mount *context=value;
    while(context->keys)pseudo_key_free(context,context->keys);
    kernel_free(context);
}

static int pseudo_node_permissions(void *value,const struct vfs_path *path,
                                     struct reliefos_permissions *permissions,bool write)
{
    struct pseudo_mount *context=value;
    const struct storage_node *node=vfs_path_node(path);
    struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    return context->provider->permissions ?
        context->provider->permissions(key->name,node,permissions,write) : -EOPNOTSUPP;
}

static uint64_t pseudo_mount_flags(void *value)
{return ((const struct pseudo_mount *)value)->provider->mount_flags;}

/** @brief Admit proc diagnostic readonly changes without altering providers.
 * @param value Required borrowed pinned pseudo mount context.
 * @param flags Requested shared policy bits.
 * @return Zero or unsupported/readonly errno. Other pseudo filesystems retain
 * their prior remount restrictions; VFS publishes attachment policy under
 * execution ownership, without allocation or graph locks across callbacks. */
static int pseudo_remount(void *value, uint64_t flags)
{
    const struct pseudo_mount *mount = value;
#ifdef CONFIG_LOCK_STAT
    if (mount->provider->filesystem && !__builtin_strcmp(mount->provider->filesystem, "proc")) {
        if (flags & ~MS_RDONLY) return -EOPNOTSUPP;
        return 0;
    }
#endif
    uint64_t current = mount->provider->mount_flags & VFS_SUPER_POLICY_MASK;
    if (flags == current) return 0;
    return (current & MS_RDONLY) && !(flags & MS_RDONLY) ? -EROFS : -EOPNOTSUPP;
}

static int pseudo_node_getattr(void *value,const struct vfs_path *path,struct linux_stat_abi *out)
{
    struct pseudo_mount *context=value;const struct storage_node *node=vfs_path_node(path);
    struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    int rc=vfs_stat_basic(node,out);struct reliefos_permissions mode;
    if(!rc)rc=pseudo_node_permissions(value,path,&mode,false);
    if(!rc) {
        uint64_t ino=UINT64_C(1469598103934665603);
        for(const unsigned char *p=(const unsigned char *)key->name;*p;++p){ino^=*p;ino*=UINT64_C(1099511628211);}
        if(key->identity)ino^=key->id;
        out->st_ino=ino&INT64_MAX;if(!out->st_ino)out->st_ino=1;
        out->st_dev=node->flags&STORAGE_NODE_FLAG_SYSFS ? STORAGE_SYSFS_DEVICE :
            node->flags&STORAGE_NODE_FLAG_PROC ? STORAGE_PROCFS_DEVICE : STORAGE_DEVFS_DEVICE;
        out->st_mode|=mode.mode&07777;out->st_uid=mode.uid;out->st_gid=mode.gid;
    }
    return rc;
}

static int pseudo_node_statfs(void *value,const struct storage_node *node,struct linux_statfs_abi *out)
{
    struct pseudo_mount *context=value;struct pseudo_key *key=pseudo_key_find(context,node->entry_id);
    if(!pseudo_key_live(context,key))return -ENOENT;
    *out=(struct linux_statfs_abi){.f_type=node->flags&STORAGE_NODE_FLAG_SYSFS ? 0x62656572 :
        node->flags&STORAGE_NODE_FLAG_PROC ? 0x9fa0 : 0x858458f6,
        .f_bsize=4096,.f_frsize=4096,.f_namelen=255,.f_flags=LINUX_ST_VALID};
    return 0;
}

/** @brief Build a generated-file mount with retained provider and inode keys.
 * @param provider Required borrowed persistent descriptor with root and lookup.
 * @param out Required empty output receiving one owned detached mount.
 * @return Zero or negative errno with context, keys and provider pins balanced.
 * Execution-owned task context; callbacks run without graph/module spinlocks. */
int vfs_pseudo_mount_new(const struct vfs_pseudo_provider *provider,struct vfs_mount **out)
{
    if(!out)return -EINVAL;
    *out=NULL;
    if(!provider || !provider->root || provider->root[0]!='/' || !provider->lookup ||
       (provider->get_identity && (!provider->identity_alive || !provider->put_identity)) ||
       ((provider->open || provider->read_open || provider->close) &&
        (!provider->open || !provider->read_open || !provider->close)))return -EINVAL;
    if(!try_module_get(provider->owner))return -ENODEV;
    static const struct vfs_backend_ops ops={.lookup=pseudo_node_lookup,.retain=pseudo_node_retain,
        .release=pseudo_node_release,.same_node=pseudo_node_same,.readlink=pseudo_node_readlink,
        .follow_link=pseudo_node_follow,.destroy=pseudo_context_destroy,
        .read=pseudo_node_read,.write=pseudo_node_write,.truncate=pseudo_node_truncate,
        .readdir=pseudo_node_readdir,.permissions=pseudo_node_permissions,
        .file_open=pseudo_file_open,.file_read=pseudo_file_read,.file_close=pseudo_file_close,
        .mount_flags=pseudo_mount_flags,.remount=pseudo_remount,.getattr=pseudo_node_getattr,.statfs=pseudo_node_statfs};
    struct pseudo_mount *context=kernel_malloc(sizeof(*context));int rc=-ENOMEM;
    if(context) {
        *context=(struct pseudo_mount){.provider=provider,.next_id=1};struct storage_node root;
        rc=pseudo_key_lookup(context,provider->root,&root);
        if(!rc)rc=vfs_mount_new(&ops,context,provider->owner,&root,out);
        if(rc)pseudo_context_destroy(context);
        else if(provider->filesystem) {
            uint64_t device=root.flags&STORAGE_NODE_FLAG_SYSFS ? STORAGE_SYSFS_DEVICE :
                root.flags&STORAGE_NODE_FLAG_PROC ? STORAGE_PROCFS_DEVICE : STORAGE_DEVFS_DEVICE;
            rc=vfs_mount_describe(*out,provider->filesystem,provider->filesystem,device);
            if(rc){vfs_mount_put(*out);*out=NULL;}
        }
    }
    module_put(provider->owner);return rc;
}
