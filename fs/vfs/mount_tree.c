/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <reliefnt/syscall_internal.h>
#include <reliefnt/deferred.h>

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
    if(!reclaim_active)kernel_deferred_schedule(DEFERRED_VFS);
}

/** @brief Query execution-owned detached mounts awaiting a successful finish.
 * @return True while the retirement FIFO retains at least one mount/provider.
 */
bool vfs_reclaim_pending(void) {return retired_count!=0;}

/** @brief Finish a rootless mount without releasing failed provider state.
 * @param mount Privately owned detached mount retaining its code/context.
 * @return True after successful final destroy/free/module put, false when
 * finish asks for retry. Execution ownership required; no graph/module lock
 * spans callbacks. Failure consumes no ownership and never invokes destroy.
 */
static bool vfs_finish_retirement(struct vfs_mount *mount)
{
    if(mount->bind_source.mount) {
        struct vfs_path backing=mount->bind_source;
        if(vfs_notify_mount_retired)vfs_notify_mount_retired(mount);
        struct module *owner=mount->owner;
        kernel_free(mount->source);kernel_free(mount);
        vfs_path_put(&backing);module_put(owner);return true;
    }
    if(mount->ops->finish && mount->ops->finish(mount->context))return false;
    if(vfs_notify_mount_retired)vfs_notify_mount_retired(mount);
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

/** @brief Admit a writer on the exact attachment held by a regular-file OFD.
 * @param path Borrowed retained path, held until the matching release.
 * @return Zero, EINVAL for an invalid path, EROFS for either readonly layer,
 * or EOVERFLOW. Execution ownership serializes admission with remount; this
 * acquires no path, inode, backend write lease or allocation of its own.
 */
int vfs_path_write_get(const struct vfs_path *path)
{
    if(!vfs_path_valid(path))return -EINVAL;
    struct vfs_mount *mount=path->mount;
    if((mount->policy|vfs_super_flags(mount))&MS_RDONLY)return -EROFS;
    if(mount->writers==UINT64_MAX)return -EOVERFLOW;
    ++mount->writers;return 0;
}

/** @brief Retire one attachment writer before giving up its holding path.
 * @param path Live retained path whose attachment owns the writer charge.
 * Execution ownership is required; no inode or path reference is consumed.
 */
void vfs_path_write_put(const struct vfs_path *path)
{
    --path->mount->writers;
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
        if(vfs_notify_dentry_retired)vfs_notify_dentry_retired(mount,dentry);
        mount->ops->release(mount->context,&dentry->node,dentry->backend_pin);
        if(parent)kernel_free(dentry->name);
        kernel_free(dentry);dentry=parent;
    }
}

/** @brief Release final root ownership before finish or deferred reclamation.
 * @param mount Owned live reference or NULL, under execution ownership.
 * @return None. Lazy detach and final finish failure transfer the rootless
 * mount/provider pin to the FIFO; lazy detach never flushes in this path.
 * No allocation or graph/module lock spans callbacks.
 */
void vfs_mount_put(struct vfs_mount *mount)
{
    if(!mount || --mount->references)return;
    vfs_lookup_cache_purge(mount);
    if(!mount->bind_source.mount)vfs_dentry_put(mount,mount->root);
    mount->root=NULL;
    if(mount->lazy_detached || !vfs_finish_retirement(mount))vfs_queue_retirement(mount);
}

void vfs_path_put(struct vfs_path *path)
{
    if(!vfs_path_valid(path))return;
    struct vfs_path old=*path;*path=(struct vfs_path){0};
    vfs_dentry_put(old.mount,old.dentry);vfs_mount_put(old.mount);
}

/** @brief Set diagnostic identity on an unpublished mount.
 * @param mount Owned mount never published in a namespace.
 * @param source Nonempty backing label. @param filesystem Nonempty type name.
 * @param device Linux encoded device number.
 * @return Zero or negative errno; a lazily detached mount cannot be renamed.
 */
int vfs_mount_describe(struct vfs_mount *mount,const char *source,const char *filesystem,uint64_t device)
{
    if(!mount || !source || !source[0] || !filesystem || !filesystem[0])return -EINVAL;
    if(mount->ns || mount->point.mount || mount->lazy_detached)return -EBUSY;
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
    if(vfs_notify_mount_created)vfs_notify_mount_created(mount);
    if(vfs_notify_dentry_created)vfs_notify_dentry_created(mount,mount->root);
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
    if(vfs_notify_dentry_created)vfs_notify_dentry_created(mount,dentry);
    struct vfs_path value={mount,dentry};vfs_path_get(&value,out);return 0;
}

/** @brief Retain an unpublished mount as the permanent namespace anchor.
 * @param ns Borrowed empty namespace. @param root Borrowed unpublished root.
 * @return Zero or negative errno. Previously lazily detached roots are rejected.
 */
int vfs_namespace_init(struct vfs_namespace *ns,struct vfs_mount *root)
{
    if(!ns || ns->root || ns->mounts || !root || root->ns || root->point.mount || root->lazy_detached)return -EINVAL;
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

/** @brief Publish an unused mount at an exact directory in the namespace.
 * @param ns Borrowed execution-owned namespace.
 * @param mount Caller-retained unpublished mount; membership takes its own ref.
 * @param target Borrowed retained directory path in ns.
 * @return Zero or negative errno, with no graph change on failure. A mount
 * that was lazily detached is not eligible for reattachment.
 */
int vfs_mount_attach(struct vfs_namespace *ns,struct vfs_mount *mount,const struct vfs_path *target)
{
    if(!ns || !ns->root || !mount || !vfs_path_valid(target))return -EINVAL;
    if(target->mount->ns!=ns)return -EXDEV;
    if(target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(mount->ns || mount->point.mount || mount->lazy_detached || vfs_covering(ns,target))return -EBUSY;
    if(!ns->next_id)return -EOVERFLOW;
    vfs_path_get(target,&mount->point);++mount->references;
    mount->ns=ns;mount->id=ns->next_id++;
    mount->next=ns->mounts;ns->mounts=mount;return 0;
}

/** @brief Prepare an unattached directory view of an existing filesystem.
 * @param source Valid retained source directory; its mount and inode are shared.
 * @param out Receives the privately owned clone on success, NULL on failure.
 * @return Zero or negative provider/admission/allocation errno. No new backend
 * is created, and the clone has no namespace membership or inotify registration.
 */
static int vfs_bind_clone(const struct vfs_path *source,struct vfs_mount **out)
{
    *out=NULL;
    struct vfs_mount *original=source->mount;
    if(!try_module_get(original->owner))return -ENODEV;
    struct vfs_mount *mount=kernel_malloc(sizeof(*mount));
    if(!mount){module_put(original->owner);return -ENOMEM;}
    *mount=(struct vfs_mount){.ops=original->ops,.context=original->context,
        .owner=original->owner,.references=1,.root=source->dentry,.policy=original->policy};
    vfs_path_get(source,&mount->bind_source);
    int rc=vfs_mount_describe(mount,original->source ? original->source : "none",
        original->filesystem ? original->filesystem : "none",original->device);
    if(rc)vfs_mount_put(mount);
    else *out=mount;
    return rc;
}

/** @brief Find the next direct submount within the selected directory view.
 * @param view The original mount and the directory bounding its subtree.
 * @param candidate Start of a suffix of the unchanged namespace mount list.
 * @return Borrowed matching attachment or NULL. A mount on the same backing
 * dentry through another attachment is not part of this view.
 */
static struct vfs_mount *vfs_bind_child_mount(const struct vfs_path *view,struct vfs_mount *candidate)
{
    for(;candidate;candidate=candidate->next) {
        if(candidate->point.mount!=view->mount)continue;
        for(const struct vfs_dentry *d=candidate->point.dentry;d && !d->disconnected;d=d->parent)
            if(d==view->dentry)return candidate;
    }
    return NULL;
}

/** @brief Clone and publish a directory attachment and optionally its submounts.
 * @param ns Borrowed namespace under execution ownership.
 * @param source Borrowed retained source directory in ns.
 * @param target Borrowed retained destination directory in ns.
 * @param recursive True to copy the source's exact descendant attachments.
 * @return Zero or negative errno. Staging traverses the original graph without
 * recursion or pathname replay; on failure the graph and next ID are unchanged.
 */
static int vfs_bind_tree(struct vfs_namespace *ns,const struct vfs_path *source,
                         const struct vfs_path *target,bool recursive)
{
    if(!ns || !ns->root || !vfs_path_valid(source) || !vfs_path_valid(target))return -EINVAL;
    if(source->mount->ns!=ns || target->mount->ns!=ns)return -EXDEV;
    if(source->dentry->node.type!=RELIEFOS_FS_TYPE_DIR || target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(source->dentry->disconnected || target->dentry->disconnected)return -ENOENT;
    if(vfs_covering(ns,target))return -EBUSY;

    struct vfs_path view=*source,point=*target;
    struct vfs_mount *pending=NULL;
    uint64_t next_id=ns->next_id;
    int rc=0;
    for(;;) {
        if(!next_id){rc=-EOVERFLOW;break;}
        struct vfs_mount *copy;
        rc=vfs_bind_clone(&view,&copy);
        if(rc)break;
        copy->id=next_id++;
        vfs_path_get(&point,&copy->point);
        copy->next=pending;pending=copy;
        if(!recursive)break;

        /* Walk the two trees in step. Their mountpoint dentries are shared;
         * only the parent attachment changes. Never visit a staged clone. */
        struct vfs_mount *child=vfs_bind_child_mount(&view,ns->mounts);
        while(!child && view.mount!=source->mount) {
            struct vfs_mount *previous=view.mount;
            copy=copy->point.mount;
            view=(struct vfs_path){previous->point.mount,copy->root};
            child=vfs_bind_child_mount(&view,previous->next);
        }
        if(!child)break;
        view=(struct vfs_path){child,child->root};
        point=(struct vfs_path){copy,child->point.dentry};
    }
    if(rc) {
        /* Reverse creation order always releases descendants before parents. */
        while(pending) {
            struct vfs_mount *copy=pending;pending=copy->next;
            vfs_path_put(&copy->point);vfs_mount_put(copy);
        }
        return rc;
    }

    /* Transfer the staging references to the namespace in one commit. */
    struct vfs_mount **tail=&pending;
    for(;*tail;tail=&(*tail)->next) {
        (*tail)->ns=ns;
        if(vfs_notify_mount_created)vfs_notify_mount_created(*tail);
    }
    *tail=ns->mounts;ns->mounts=pending;ns->next_id=next_id;
    return 0;
}

/** @brief Publish one directory view without copying submounts.
 * @param ns Execution-owned namespace. @param source Retained source directory.
 * @param target Retained destination directory.
 * @return Zero or negative errno without partial publication.
 */
int vfs_mount_bind(struct vfs_namespace *ns,const struct vfs_path *source,const struct vfs_path *target)
{
    return vfs_bind_tree(ns,source,target,false);
}

/** @brief Publish a directory view with independent copies of its submounts.
 * @param ns Execution-owned namespace. @param source Retained subtree root.
 * @param target Retained destination directory.
 * @return Zero or negative errno without partial publication.
 */
int vfs_mount_bind_recursive(struct vfs_namespace *ns,const struct vfs_path *source,
                              const struct vfs_path *target)
{
    return vfs_bind_tree(ns,source,target,true);
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
        for(struct vfs_mount *backing=ancestor;backing;backing=backing->bind_source.mount)
            if(backing==mount)return -ELOOP;
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
    if(!mount->bind_source.mount && mount->ops->can_unmount) {
        int rc=mount->ops->can_unmount(mount->context,mount->root->backend_pin);if(rc)return rc;
    }
    vfs_mount_detach(ns,mount);return 0;
}

/** @brief Disconnect a visible subtree before releasing any association.
 * @param ns Borrowed namespace under execution ownership.
 * @param path Held selected mount root, excluding the permanent anchor.
 * @return Zero or negative errno before mutation. Retained paths keep their
 * objects; no allocation, sync, busy preflight or final backend finish here.
 */
int vfs_mount_unmount_lazy(struct vfs_namespace *ns,const struct vfs_path *path)
{
    if(!ns || !vfs_path_valid(path))return -EINVAL;
    struct vfs_mount *root=path->mount;
    if(root->ns!=ns || path->dentry!=root->root)return -EINVAL;
    if(root==ns->root || !root->point.mount)return -EBUSY;

    struct vfs_mount *detached=NULL,**link=&ns->mounts;
    while(*link) {
        struct vfs_mount *mount=*link,*ancestor=mount;
        while(ancestor && ancestor!=root)ancestor=ancestor->point.mount;
        if(!ancestor){link=&mount->next;continue;}
        *link=mount->next;mount->next=detached;detached=mount;
        mount->ns=NULL;mount->lazy_detached=true;
    }
    while(detached) {
        struct vfs_mount *mount=detached;detached=mount->next;mount->next=NULL;
        vfs_path_put(&mount->point);vfs_mount_put(mount);
    }
    return 0;
}

int vfs_namespace_destroy(struct vfs_namespace *ns)
{
    if(!ns)return -EINVAL;
    /* Preflight the entire graph so a busy child cannot cause partial detach. */
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        uint64_t expected=1;
        for(struct vfs_mount *child=ns->mounts;child;child=child->next)
            {if(child->point.mount==mount)++expected;if(child->bind_source.mount==mount)++expected;}
        if(mount->references!=expected)return -EBUSY;
        if(!mount->bind_source.mount && mount->ops->can_unmount) {
            int rc=mount->ops->can_unmount(mount->context,mount->root->backend_pin);if(rc)return rc;
        }
    }
    while(ns->mounts) {
        struct vfs_mount *leaf=ns->mounts;
        for(;;) {
            struct vfs_mount *child=ns->mounts;
            while(child && child->point.mount!=leaf && child->bind_source.mount!=leaf)child=child->next;
            if(!child)break;leaf=child;
        }
        vfs_mount_detach(ns,leaf);
    }
    *ns=(struct vfs_namespace){0};return 0;
}

int vfs_path_lookup_child_for_walk(const struct vfs_path *parent,const char *name,struct vfs_path *out)
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
    uint64_t generation=mount->ops->lookup_generation ? mount->ops->lookup_generation(mount->context) : 0;
    int cached=vfs_lookup_cache_get(parent,name,generation,&node);
    if(cached<0)return cached;
    if(cached) {
        int rc=vfs_bind_child(parent,name,&node,out);
        if(rc!=-ENOENT && rc!=-ESTALE)return rc;
        /* Clustered providers may recycle an unused inode-key cache record
         * without changing the directory. Revalidate the name rather than
         * exposing a stale cached incarnation as a missing existing file. */
        vfs_lookup_cache_purge(mount);
        generation=mount->ops->lookup_generation(mount->context);
    }
    int rc=mount->ops->lookup(mount->context,&parent->dentry->node,name,&node);
    bool absent=rc==-ENOENT;
    if(!rc)rc=vfs_bind_child(parent,name,&node,out);
    if(generation && (!rc || absent) && generation==mount->ops->lookup_generation(mount->context))
        vfs_lookup_cache_store(parent,name,generation,absent ? NULL : &node);
    return rc;
}

int vfs_path_lookup_child(const struct vfs_path *parent,const char *name,struct vfs_path *out)
{
    int rc=vfs_path_lookup_child_for_walk(parent,name,out);
    return rc==VFS_NATIVE_LOOKUP_RECLAIM ? -ENOMEM : rc;
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
        dentry=kernel_malloc(sizeof(*dentry));
        if(!dentry){return -ENOMEM;}
        *dentry=(struct vfs_dentry){.parent=parent->dentry,.node=node};
        dentry->name=kernel_malloc(length+1);
        if(!dentry->name){kernel_free(dentry);return -ENOMEM;}
        __builtin_memcpy(dentry->name,name,length+1);
        rc=mount->ops->retain(mount->context,&node,&dentry->backend_pin);
        if(rc){kernel_free(dentry->name);kernel_free(dentry);return rc;}
        ++parent->dentry->references;
        dentry->next=parent->dentry->children;parent->dentry->children=dentry;
        if(vfs_notify_dentry_created)vfs_notify_dentry_created(mount,dentry);
    }
    /* The inode identity stays pinned, while size/type/permission metadata
     * may have changed since an older directory handle was acquired. */
    dentry->node=node;
    struct vfs_path value={mount,dentry};vfs_path_get(&value,out);return 0;
}

/** @brief Walk one parent step across visible edges or stop at a detached root.
 * @param root Borrowed task lookup boundary.
 * @param path Owned current path, replaced with an equally owned parent.
 * Unattached bind roots do not expose the backing dentry's outside parent.
 */
void vfs_path_parent(const struct vfs_path *root,struct vfs_path *path)
{
    if(vfs_path_equal(root,path))return;
    struct vfs_path next=*path;
    while(next.dentry==next.mount->root && next.mount->point.mount) {
        next=next.mount->point;
        if(vfs_path_equal(root,&next))break;
    }
    if(!vfs_path_equal(root,&next) && next.dentry!=next.mount->root && next.dentry->parent)
        next.dentry=next.dentry->parent;
    struct vfs_path owned={0};vfs_path_get(&next,&owned);vfs_path_put(path);*path=owned;
}
