/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

/* Detached mounts reuse next only after losing namespace membership. The
 * private queue retains context/ops/module lifetime without any public path. */
static struct vfs_mount *retired_mounts,**retired_tail=&retired_mounts;
static uint64_t retired_count;
static bool reclaim_active;

/** @brief Append one detached, rootless mount to the execution-owned FIFO.
 * @param mount Privately owned final mount, not already queued; next is unused.
 * @return None. Transfers ownership to the FIFO without allocation/callbacks.
 */
static void vfs_queue_retirement(struct vfs_mount *mount)
{
    mount->next=NULL;*retired_tail=mount;retired_tail=&mount->next;++retired_count;
}

/** @brief Finish a rootless mount without releasing failed provider state.
 * @param mount Privately owned detached mount retaining its code/context.
 * @return True after successful final destroy/free/module put, false when
 * finish asks for retry. Execution ownership required; no graph/module lock
 * spans callbacks. Failure consumes no ownership and never invokes destroy.
 */
static bool vfs_finish_retirement(struct vfs_mount *mount)
{
    if(mount->ops->finish && mount->ops->finish(mount->context))return false;
    struct module *owner=mount->owner;
    if(mount->ops->destroy)mount->ops->destroy(mount->context);
    kernel_free(mount->source);kernel_free(mount);module_put(owner);return true;
}

/** @brief Retry the initial bounded FIFO batch, retaining each failed owner.
 * @param budget Maximum attempts, zero leaves the queue unchanged.
 * @return Number successfully reclaimed; each initially pending mount is
 * attempted at most once. Execution ownership required, no allocation or
 * graph/module lock. Reentrant calls return zero while callbacks are active.
 */
uint32_t vfs_reclaim_mounts(uint32_t budget)
{
    if(!budget || !retired_mounts || reclaim_active)return 0;
    uint64_t attempts=retired_count<budget ? retired_count : budget;
    uint32_t reclaimed=0;reclaim_active=true;
    while(attempts-- && retired_mounts) {
        struct vfs_mount *mount=retired_mounts;retired_mounts=mount->next;--retired_count;
        if(!retired_mounts)retired_tail=&retired_mounts;
        mount->next=NULL;
        if(vfs_finish_retirement(mount))++reclaimed;
        else vfs_queue_retirement(mount);
    }
    reclaim_active=false;return reclaimed;
}

bool vfs_path_valid(const struct vfs_path *path)
{return path && path->mount && path->dentry;}

bool vfs_path_equal(const struct vfs_path *left,const struct vfs_path *right)
{return vfs_path_valid(left) && vfs_path_valid(right) && left->mount==right->mount && left->dentry==right->dentry;}

const struct storage_node *vfs_path_node(const struct vfs_path *path)
{return vfs_path_valid(path) ? &path->dentry->node : NULL;}
void *vfs_path_context(const struct vfs_path *path)
{return vfs_path_valid(path) ? path->mount->context : NULL;}

void vfs_path_get(const struct vfs_path *source,struct vfs_path *out)
{
    if(!out || source==out)return;
    *out=(struct vfs_path){0};if(!vfs_path_valid(source))return;
    *out=*source;++out->mount->references;++out->dentry->references;
}

/* The root has one permanent mount-owned reference. A child retains its
 * parent dentry once, independently of the number of paths to the child. */
void vfs_dentry_put(struct vfs_mount *mount,struct vfs_dentry *dentry)
{
    while(dentry && !--dentry->references) {
        struct vfs_dentry *parent=dentry->parent;
        if(parent) {
            struct vfs_dentry **link=&parent->children;
            while(*link && *link!=dentry)link=&(*link)->next;
            if(*link)*link=dentry->next;
        }
        mount->ops->release(mount->context,&dentry->node,dentry->backend_pin);
        if(parent)kernel_free(dentry->name);
        kernel_free(dentry);dentry=parent;
    }
}

/** @brief Release final root ownership before finish or deferred reclamation.
 * @param mount Owned live reference or NULL, under execution ownership.
 * @return None. Final failure transfers the rootless backend and provider pin
 * to the private FIFO; no allocation or graph/module lock spans callbacks.
 */
void vfs_mount_put(struct vfs_mount *mount)
{
    if(!mount || --mount->references)return;
    vfs_dentry_put(mount,mount->root);
    mount->root=NULL;
    if(!vfs_finish_retirement(mount))vfs_queue_retirement(mount);
}

void vfs_path_put(struct vfs_path *path)
{
    if(!vfs_path_valid(path))return;
    struct vfs_path old=*path;*path=(struct vfs_path){0};
    vfs_dentry_put(old.mount,old.dentry);vfs_mount_put(old.mount);
}

int vfs_mount_describe(struct vfs_mount *mount,const char *source,const char *filesystem,uint64_t device)
{
    if(!mount || !source || !source[0] || !filesystem || !filesystem[0])return -EINVAL;
    if(mount->ns || mount->point.mount)return -EBUSY;
    size_t source_length=0,type_length=0;
    while(source[source_length])if(++source_length>=VFS_PATH_MAX)return -ENAMETOOLONG;
    while(filesystem[type_length]) {
        unsigned char c=filesystem[type_length];
        if(!((c>='a' && c<='z') || (c>='A' && c<='Z') ||
             (c>='0' && c<='9') || c=='_' || c=='-' || c=='.'))return -EINVAL;
        if(++type_length>VFS_NAME_MAX)return -ENAMETOOLONG;
    }
    char *identity=kernel_malloc(source_length+type_length+2);if(!identity)return -ENOMEM;
    __builtin_memcpy(identity,source,source_length+1);
    __builtin_memcpy(identity+source_length+1,filesystem,type_length+1);
    char *old=mount->source;mount->source=identity;
    mount->filesystem=identity+source_length+1;mount->device=device;kernel_free(old);return 0;
}

/** @brief Construct an owned detached root after live provider admission.
 * @param ops Persistent callbacks with lookup/retain/release/same_node required.
 * @param context Borrowed backend transferred to final cleanup on success.
 * @param owner Provider code owner, NULL for builtin; must admit new references.
 * @param root Borrowed directory metadata, retained on success.
 * @param out Required owned mount output, empty on failure.
 * @return Zero or negative input/admission/allocation/provider errno. Captures
 * initial attachment defaults only after code/root admission. Execution
 * ownership required; no module/graph lock spans callbacks; rolls back its
 * own root/code pins, leaving context cleanup to caller on creation failure.
 */
int vfs_mount_new(const struct vfs_backend_ops *ops,void *context,struct module *owner,
                   const struct storage_node *root,struct vfs_mount **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!ops || !ops->lookup || !ops->retain || !ops->release || !ops->same_node ||
       !root || root->type!=RELIEFOS_FS_TYPE_DIR)return -EINVAL;
    if(!try_module_get(owner))return -ENODEV;
    struct vfs_mount *mount=kernel_malloc(sizeof(*mount));
    if(!mount){module_put(owner);return -ENOMEM;}
    *mount=(struct vfs_mount){.ops=ops,.context=context,.owner=owner,.references=1};
    mount->root=kernel_malloc(sizeof(*mount->root)+1);
    if(!mount->root){kernel_free(mount);module_put(owner);return -ENOMEM;}
    *mount->root=(struct vfs_dentry){.node=*root,.references=1,
        .name=(char *)(mount->root+1)};mount->root->name[0]=0;
    int rc=ops->retain(context,root,&mount->root->backend_pin);
    if(rc){kernel_free(mount->root);kernel_free(mount);module_put(owner);return rc;}
    mount->policy=ops->mount_flags ? vfs_policy_bits(ops->mount_flags(context)) : 0;
    *out=mount;return 0;
}

int vfs_path_anonymous(struct vfs_mount *mount,const struct storage_node *node,struct vfs_path *out)
{
    if(!out)return -EINVAL;*out=(struct vfs_path){0};
    if(!mount || !node || node->type!=RELIEFOS_FS_TYPE_FILE)return -EINVAL;
    struct vfs_dentry *dentry=kernel_malloc(sizeof(*dentry));if(!dentry)return -ENOMEM;
    *dentry=(struct vfs_dentry){.parent=mount->root,.node=*node,.disconnected=true};
    int rc=mount->ops->retain(mount->context,node,&dentry->backend_pin);
    if(rc){kernel_free(dentry);return rc;}
    ++mount->root->references;
    struct vfs_path value={mount,dentry};vfs_path_get(&value,out);return 0;
}

int vfs_namespace_init(struct vfs_namespace *ns,struct vfs_mount *root)
{
    if(!ns || ns->root || ns->mounts || !root || root->ns || root->point.mount)return -EINVAL;
    ++root->references;root->ns=ns;root->id=1;root->next=NULL;
    ns->root=root;ns->mounts=root;ns->next_id=2;return 0;
}

/* Mountpoint identity is a retained parent mount + dentry, never text. */
static struct vfs_mount *vfs_covering(const struct vfs_namespace *ns,const struct vfs_path *path)
{
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next)
        if(vfs_path_equal(&mount->point,path))return mount;
    return NULL;
}

void vfs_follow_mounts(const struct vfs_namespace *ns,struct vfs_path *path)
{
    struct vfs_mount *cover;
    while((cover=vfs_covering(ns,path))) {
        struct vfs_path next={cover,cover->root},owned={0};vfs_path_get(&next,&owned);
        vfs_path_put(path);*path=owned;
    }
}

int vfs_namespace_root(const struct vfs_namespace *ns,struct vfs_path *out)
{
    if(!out)return -EINVAL;*out=(struct vfs_path){0};
    if(!ns || !ns->root)return -ENOENT;
    struct vfs_path root={ns->root,ns->root->root};vfs_path_get(&root,out);
    vfs_follow_mounts(ns,out);return 0;
}

int vfs_mount_attach(struct vfs_namespace *ns,struct vfs_mount *mount,const struct vfs_path *target)
{
    if(!ns || !ns->root || !mount || !vfs_path_valid(target))return -EINVAL;
    if(target->mount->ns!=ns)return -EXDEV;
    if(target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(mount->ns || mount->point.mount || vfs_covering(ns,target))return -EBUSY;
    if(!ns->next_id)return -EOVERFLOW;
    vfs_path_get(target,&mount->point);++mount->references;
    mount->ns=ns;mount->id=ns->next_id++;
    mount->next=ns->mounts;ns->mounts=mount;return 0;
}

int vfs_mount_move(struct vfs_namespace *ns,const struct vfs_path *source,const struct vfs_path *target)
{
    if(!ns || !vfs_path_valid(source) || !vfs_path_valid(target))return -EINVAL;
    struct vfs_mount *mount=source->mount;
    if(mount->ns!=ns || target->mount->ns!=ns)return -EXDEV;
    if(source->dentry!=mount->root)return -EINVAL;
    if(mount==ns->root || !mount->point.mount)return -EBUSY;
    if(target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    for(struct vfs_mount *ancestor=target->mount;ancestor;ancestor=ancestor->point.mount)
        if(ancestor==mount)return -ELOOP;
    if(vfs_covering(ns,target))return -EBUSY;
    struct vfs_path replacement={0};vfs_path_get(target,&replacement);
    struct vfs_path old=mount->point;mount->point=replacement;vfs_path_put(&old);return 0;
}

static void vfs_mount_detach(struct vfs_namespace *ns,struct vfs_mount *mount)
{
    struct vfs_mount **link=&ns->mounts;
    while(*link && *link!=mount)link=&(*link)->next;
    if(*link)*link=mount->next;
    struct vfs_path old=mount->point;mount->point=(struct vfs_path){0};
    mount->ns=NULL;mount->next=NULL;vfs_path_put(&old);vfs_mount_put(mount);
}

int vfs_mount_unmount(struct vfs_namespace *ns,const struct vfs_path *path)
{
    if(!ns || !vfs_path_valid(path))return -EINVAL;
    struct vfs_mount *mount=path->mount;
    if(mount->ns!=ns)return -EXDEV;
    if(path->dentry!=mount->root)return -EINVAL;
    if(mount==ns->root || !mount->point.mount || mount->references!=2)return -EBUSY;
    if(mount->ops->can_unmount) {
        int rc=mount->ops->can_unmount(mount->context,mount->root->backend_pin);if(rc)return rc;
    }
    vfs_mount_detach(ns,mount);return 0;
}

int vfs_namespace_destroy(struct vfs_namespace *ns)
{
    if(!ns)return -EINVAL;
    /* Preflight the entire graph so a busy child cannot cause partial detach. */
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        uint64_t expected=1;
        for(struct vfs_mount *child=ns->mounts;child;child=child->next)
            if(child->point.mount==mount)++expected;
        if(mount->references!=expected)return -EBUSY;
        if(mount->ops->can_unmount) {
            int rc=mount->ops->can_unmount(mount->context,mount->root->backend_pin);if(rc)return rc;
        }
    }
    while(ns->mounts) {
        struct vfs_mount *leaf=ns->mounts;
        for(;;) {
            struct vfs_mount *child=ns->mounts;
            while(child && child->point.mount!=leaf)child=child->next;
            if(!child)break;leaf=child;
        }
        vfs_mount_detach(ns,leaf);
    }
    *ns=(struct vfs_namespace){0};return 0;
}

int vfs_path_lookup_child(const struct vfs_path *parent,const char *name,struct vfs_path *out)
{
    if(!out)return -EINVAL;*out=(struct vfs_path){0};
    if(!vfs_path_valid(parent) || !name || !name[0] || !__builtin_strcmp(name,".") || !__builtin_strcmp(name,".."))
        return -EINVAL;
    if(parent->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(parent->dentry->disconnected)return -ENOENT;
    for(size_t i=0;name[i];++i) {
        if(name[i]=='/')return -EINVAL;
        if(i>=VFS_NAME_MAX)return -ENAMETOOLONG;
    }
    struct vfs_mount *mount=parent->mount;struct storage_node node;
    int rc=mount->ops->lookup(mount->context,&parent->dentry->node,name,&node);
    return rc ? rc : vfs_bind_child(parent,name,&node,out);
}

int vfs_bind_child(const struct vfs_path *parent,const char *name,
                       const struct storage_node *metadata,struct vfs_path *out)
{
    struct vfs_mount *mount=parent->mount;struct storage_node node=*metadata;
    int rc;
    struct vfs_dentry *dentry=parent->dentry->children;
    for(;dentry;dentry=dentry->next)
        if(!dentry->disconnected && !__builtin_strcmp(dentry->name,name) &&
           mount->ops->same_node(mount->context,&dentry->node,&node))break;
    if(!dentry) {
        size_t length=__builtin_strlen(name);
        dentry=kernel_malloc(sizeof(*dentry));if(!dentry)return -ENOMEM;
        *dentry=(struct vfs_dentry){.parent=parent->dentry,.node=node};
        dentry->name=kernel_malloc(length+1);
        if(!dentry->name){kernel_free(dentry);return -ENOMEM;}
        __builtin_memcpy(dentry->name,name,length+1);
        rc=mount->ops->retain(mount->context,&node,&dentry->backend_pin);
        if(rc){kernel_free(dentry->name);kernel_free(dentry);return rc;}
        ++parent->dentry->references;
        dentry->next=parent->dentry->children;parent->dentry->children=dentry;
    }
    /* The inode identity stays pinned, while size/type/permission metadata
     * may have changed since an older directory handle was acquired. */
    dentry->node=node;
    struct vfs_path value={mount,dentry};vfs_path_get(&value,out);return 0;
}

void vfs_path_parent(const struct vfs_path *root,struct vfs_path *path)
{
    if(vfs_path_equal(root,path))return;
    struct vfs_path next=*path;
    while(next.dentry==next.mount->root && next.mount->point.mount) {
        next=next.mount->point;
        if(vfs_path_equal(root,&next))break;
    }
    if(!vfs_path_equal(root,&next) && next.dentry->parent)next.dentry=next.dentry->parent;
    struct vfs_path owned={0};vfs_path_get(&next,&owned);vfs_path_put(path);*path=owned;
}
