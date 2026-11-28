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
        if(mount->propagation_backing)--backing.mount->propagation_pins;
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
    ns->root=root;ns->mounts=root;ns->next_id=2;ns->next_peer_id=1;return 0;
}

/** @brief Find the attachment immediately covering an exact retained slot.
 * @param ns Borrowed execution-owned namespace.
 * @param path Borrowed parent mount/dentry pair, never a textual pathname.
 * @return Borrowed immediate cover or NULL; does not follow the mount stack.
 */
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

/** @brief Check mount ancestry by retained topology rather than pathname.
 * @param root Borrowed candidate subtree root.
 * @param mount Borrowed attachment to test.
 * @return True if mount is root or reaches it through parent edges.
 */
static bool vfs_mount_below(const struct vfs_mount *root,const struct vfs_mount *mount)
{
    for(;mount;mount=mount->point.mount)if(mount==root)return true;
    return false;
}

/** @brief Leave a peer group without orphaning its downstream attachments.
 * @param ns Borrowed execution-owned namespace.
 * @param mount Live departing member (a nonmember is unchanged).
 * @return A live peer, or the old master if no peer remains. If the group
 * survives, its receivers stay with the group; otherwise they inherit its
 * master. These weak edges never pin a mount or a backend.
 */
static struct vfs_mount *vfs_leave_peer_group(struct vfs_namespace *ns,struct vfs_mount *mount)
{
    struct vfs_mount *upstream=mount->master;
    if(!mount->peer_group)return upstream;
    for(struct vfs_mount *peer=ns->mounts;peer;peer=peer->next)
        if(peer!=mount && peer->peer_group==mount->peer_group){upstream=peer;break;}
    for(struct vfs_mount *other=ns->mounts;other;other=other->next)
        if(other->master==mount)other->master=upstream;
    mount->peer_group=0;
    return upstream;
}

/** @brief Apply one supported propagation type to selected live attachments.
 * @param ns Borrowed namespace under execution ownership.
 * @param path Retained exact root of the selected live mount.
 * @param flags One propagation selector and optional MS_REC.
 * @return Zero or negative errno. The whole change is validated before
 * editing any mount or advancing the group ID counter. Making a slave retains
 * an upstream group if available; private and unbindable clear the selected
 * attachment's propagation membership.
 * No allocation, backend callback or access-policy change.
 */
int vfs_mount_set_propagation(struct vfs_namespace *ns,const struct vfs_path *path,uint64_t flags)
{
    uint64_t type=flags&~MS_REC;
    if(!type || (type&(type-1)) || (type&~VFS_PROPAGATION_MASK))return -EINVAL;
    if(!ns || !vfs_path_valid(path) || path->mount->ns!=ns || path->dentry!=path->mount->root)
        return -EINVAL;
    struct vfs_mount *root=path->mount;
    bool recursive=(flags&MS_REC)!=0;
    uint64_t next=ns->next_peer_id;
    if(type==MS_SHARED) {
        for(struct vfs_mount *mount=recursive ? ns->mounts : root;mount;
            mount=recursive ? mount->next : NULL) {
            if(mount->peer_group || !vfs_mount_below(root,mount))continue;
            if(!next)return -EOVERFLOW;
            ++next;
        }
    }
    for(struct vfs_mount *mount=recursive ? ns->mounts : root;mount;
        mount=recursive ? mount->next : NULL) {
        if(!vfs_mount_below(root,mount))continue;
        if(type==MS_SHARED) {
            if(!mount->peer_group)mount->peer_group=ns->next_peer_id++;
            mount->unbindable=false;
        } else if(type==MS_SLAVE) {
            if(mount->peer_group)mount->master=vfs_leave_peer_group(ns,mount);
        } else {
            vfs_leave_peer_group(ns,mount);
            mount->master=NULL;
            mount->unbindable=type==MS_UNBINDABLE;
        }
    }
    return 0;
}

/** @brief Prepare an unattached directory view of an existing filesystem.
 * @param view Valid source directory; supplies the root and attachment policy.
 * @param backing Retained path to the same dentry, pinned for this view's lifetime.
 * @param propagation True for a replica-only backing reference.
 * @param out Receives the privately owned clone on success, NULL on failure.
 * @return Zero or negative provider/admission/allocation errno. No new backend
 * is created, and the clone has no namespace membership or inotify registration.
 */
static int vfs_bind_clone(const struct vfs_path *view,const struct vfs_path *backing,
                           bool propagation,struct vfs_mount **out)
{
    *out=NULL;
    struct vfs_mount *original=view->mount;
    if(!try_module_get(original->owner))return -ENODEV;
    struct vfs_mount *mount=kernel_malloc(sizeof(*mount));
    if(!mount){module_put(original->owner);return -ENOMEM;}
    *mount=(struct vfs_mount){.ops=original->ops,.context=original->context,
        .owner=original->owner,.references=1,.root=view->dentry,.policy=original->policy,
        .propagation_backing=propagation};
    vfs_path_get(backing,&mount->bind_source);
    if(propagation)++backing->mount->propagation_pins;
    int rc=vfs_mount_describe(mount,original->source ? original->source : "none",
        original->filesystem ? original->filesystem : "none",original->device);
    if(rc)vfs_mount_put(mount);
    else *out=mount;
    return rc;
}

struct vfs_mount_batch {
    struct vfs_mount *pending;
    uint64_t next_id,next_peer_id;
};

/** @brief Add one privately owned mount reference to an unpublished batch.
 * @param batch Staging list ordered from newest to oldest for rollback.
 * @param mount Prepared mount; its reference transfers to the batch.
 * @param point Borrowed exact destination, retained by the staged mount.
 */
static void vfs_stage_mount(struct vfs_mount_batch *batch,struct vfs_mount *mount,
                            const struct vfs_path *point)
{
    mount->id=batch->next_id++;
    vfs_path_get(point,&mount->point);
    mount->next=batch->pending;batch->pending=mount;
}

/** @brief Transfer an attachment to another exact parent slot.
 * @param mount Live or staged mount whose old mountpoint is retained.
 * @param point Borrowed new slot, retained before releasing the old one.
 * Caller excludes topology cycles and holds both graphs through the update.
 */
static void vfs_replace_mountpoint(struct vfs_mount *mount,const struct vfs_path *point)
{
    struct vfs_path replacement={0},old=mount->point;
    vfs_path_get(point,&replacement);mount->point=replacement;vfs_path_put(&old);
}

/** @brief Roll back all staged mounts or publish them together without allocation.
 * @param ns Borrowed execution-owned namespace.
 * @param batch Owned unpublished mounts and tentative ID counters.
 * @param status Nonzero to discard, zero to commit all changes.
 * @return The supplied status. Rollback releases descendants before parents;
 * neither graph nor namespace counters change on error. At commit, rehang
 * any existing receiver stack on its newly inserted mount's root.
 */
static int vfs_finish_mount_batch(struct vfs_namespace *ns,struct vfs_mount_batch *batch,int status)
{
    if(status) {
        while(batch->pending) {
            struct vfs_mount *mount=batch->pending;batch->pending=mount->next;
            mount->next=NULL;vfs_path_put(&mount->point);vfs_mount_put(mount);
        }
        return status;
    }
    struct vfs_mount **tail=&batch->pending;
    for(;*tail;tail=&(*tail)->next) {
        struct vfs_mount *mount=*tail;mount->ns=ns;
        if(mount->staged_cover) {
            struct vfs_path root={mount,mount->root};
            vfs_replace_mountpoint(mount->staged_cover,&root);
            mount->staged_cover=NULL;
        }
        if(!mount->notify_link && vfs_notify_mount_created)vfs_notify_mount_created(mount);
    }
    if(batch->pending){*tail=ns->mounts;ns->mounts=batch->pending;}
    ns->next_id=batch->next_id;ns->next_peer_id=batch->next_peer_id;
    return 0;
}

/** @brief Test whether one connected dentry lies inside a directory view.
 * @param dentry Borrowed candidate. @param root Borrowed directory boundary.
 * @return True for the boundary itself or a descendant connected to it.
 */
static bool vfs_dentry_below(const struct vfs_dentry *dentry,const struct vfs_dentry *root)
{
    for(;dentry && !dentry->disconnected;dentry=dentry->parent)
        if(dentry==root)return true;
    return false;
}

/** @brief Measure the downward propagation distance from a shared origin.
 * @param origin Borrowed attachment sending the event.
 * @param receiver Borrowed candidate attachment in the same namespace.
 * @param depth Receives zero for peers, positive for downstream groups.
 * @return True for a peer or downstream receiver, including origin itself
 * if shared. Following upstream edges cannot route an event back upstream.
 */
static bool vfs_propagation_depth(const struct vfs_mount *origin,const struct vfs_mount *receiver,
                                  uint64_t *depth)
{
    *depth=0;
    if(!origin->peer_group)return false;
    for(;receiver;receiver=receiver->master,++*depth)
        if(receiver->peer_group==origin->peer_group)return true;
    return false;
}

/** @brief Find a staged view from a particular receiver's peer group.
 * @param batch Borrowed batch for this single propagation event.
 * @param receiver Borrowed live shared destination parent.
 * @param origin Event root; a move leaves it live at its old mountpoint until commit.
 * @param target Exact future mountpoint of the event root.
 * @return The event root for the origin group, a prepared receiver, or NULL.
 * Submounts of staged trees cannot match a live destination parent.
 */
static struct vfs_mount *vfs_staged_peer(const struct vfs_mount_batch *batch,
                                         const struct vfs_mount *receiver,struct vfs_mount *origin,
                                         const struct vfs_path *target)
{
    if(receiver->peer_group==target->mount->peer_group)return origin;
    for(struct vfs_mount *mount=batch->pending;mount;mount=mount->next) {
        const struct vfs_mount *parent=mount->point.mount;
        if(mount->point.dentry==target->dentry && parent->ns==receiver->ns &&
           parent->peer_group==receiver->peer_group)return mount;
    }
    return NULL;
}

/** @brief Find the next direct submount within the selected directory view.
 * @param view The original mount and the directory bounding its subtree.
 * @param candidate Start of a suffix of the unchanged namespace mount list.
 * @return Borrowed matching bindable attachment or NULL. An unbindable
 * attachment prunes its branch; a mount through another attachment is not
 * part of the view even when its backing dentry is shared.
 */
static struct vfs_mount *vfs_bind_child_mount(const struct vfs_path *view,struct vfs_mount *candidate)
{
    for(;candidate;candidate=candidate->next)
        if(candidate->point.mount==view->mount && !candidate->unbindable &&
           vfs_dentry_below(candidate->point.dentry,view->dentry))return candidate;
    return NULL;
}

enum vfs_mount_copy { VFS_COPY_BIND,VFS_COPY_PEER,VFS_COPY_SLAVE };

/** @brief Stage one bind tree using previously resolved directory identities.
 * @param batch Transaction owning all clones, including partial trees on error.
 * @param source Source view in the live graph or a previously staged tree.
 * @param target Borrowed retained destination for this tree.
 * @param mounts Fixed inventory of source attachments for a recursive walk.
 * @param recursive True to copy bindable child mounts.
 * @param shared True if the destination parent is shared.
 * @param kind A direct bind, or a replay sharing or following the source group.
 * @param out Receives the staged tree root, possibly NULL on failure.
 * @return Zero or negative errno. No backend graph changes or path replay.
 */
static int vfs_stage_bind_tree(struct vfs_mount_batch *batch,const struct vfs_path *source,
                              const struct vfs_path *target,struct vfs_mount *mounts,
                              bool recursive,bool shared,enum vfs_mount_copy kind,
                              struct vfs_mount **out)
{
    struct vfs_path view=*source,point=*target;
    *out=NULL;
    for(;;) {
        if(!batch->next_id)return -EOVERFLOW;
        uint64_t group=kind==VFS_COPY_SLAVE ? 0 :
            (view.mount->peer_group ? view.mount->peer_group : view.mount->staged_peer_group);
        if(shared && !group) {
            if(!batch->next_peer_id)return -EOVERFLOW;
            group=batch->next_peer_id++;
        }
        struct vfs_mount *copy;
        /* Parallel views retain the same backing with the same pin type.
         * A replica of a native tree holds an internal lifetime pin, not an
         * ordinary bind that would make the moved mount busy. */
        bool replay=kind!=VFS_COPY_BIND && view.mount->bind_source.mount;
        bool propagation=kind!=VFS_COPY_BIND && (!replay || view.mount->propagation_backing);
        const struct vfs_path *backing=replay ? &view.mount->bind_source : &view;
        int rc=vfs_bind_clone(&view,backing,propagation,&copy);
        if(rc)return rc;
        copy->peer_group=group;
        copy->master=kind==VFS_COPY_SLAVE ? view.mount : view.mount->master;
        copy->propagation_copy=kind!=VFS_COPY_BIND;
        vfs_stage_mount(batch,copy,&point);
        if(!*out)*out=copy;
        if(!recursive)return 0;

        /* Walk the two trees in step. Their mountpoint dentries are shared;
         * only the parent attachment changes. The inventory stays fixed. */
        struct vfs_mount *child=vfs_bind_child_mount(&view,mounts);
        while(!child && view.mount!=source->mount) {
            struct vfs_mount *previous=view.mount;
            copy=copy->point.mount;
            view=(struct vfs_path){previous->point.mount,copy->root};
            child=vfs_bind_child_mount(&view,previous->next);
        }
        if(!child)return 0;
        view=(struct vfs_path){child,child->root};
        point=(struct vfs_path){copy,child->point.dentry};
    }
}

/** @brief Stage the peers and downstream receivers of one mount tree.
 * @param ns Borrowed execution-owned namespace; live inventory stays fixed.
 * @param batch Owns all prepared replicas and, for an attach or bind, the origin.
 * @param origin Borrowed event root, live at its old location for a move.
 * @param target Exact destination for the event root.
 * @param recursive True to copy the origin's bindable descendants.
 * @return Zero or negative errno. Parents are visited before their slaves;
 * each receiving peer group shares one new group per copied attachment.
 * A group with no matching directory view is skipped as a recipient but
 * does not block eligible descendants from receiving the event. An existing
 * cover is reparented only at commit so it remains visible above the copy.
 */
static int vfs_stage_propagation(struct vfs_namespace *ns,struct vfs_mount_batch *batch,
                                 struct vfs_mount *origin,const struct vfs_path *target,bool recursive)
{
    if(!target->mount->peer_group)return 0;
    for(uint64_t level=0;;++level) {
        bool deeper=false;
        for(struct vfs_mount *peer=ns->mounts;peer;peer=peer->next) {
            uint64_t depth;
            if(peer==target->mount || !vfs_propagation_depth(target->mount,peer,&depth) ||
               !vfs_dentry_below(target->dentry,peer->root))continue;
            if(depth>level){deeper=true;continue;}
            if(depth!=level)continue;
            struct vfs_path point={peer,target->dentry};

            struct vfs_mount *prior=peer->peer_group ? vfs_staged_peer(batch,peer,origin,target) : NULL;
            enum vfs_mount_copy kind=prior ? VFS_COPY_PEER : VFS_COPY_SLAVE;
            for(struct vfs_mount *master=peer->master;!prior && master;master=master->master)
                prior=vfs_staged_peer(batch,master,origin,target);
            /* Depth guarantees a staged ancestor; the origin always exists. */
            if(!prior)return -EINVAL;
            struct vfs_path view={prior,prior->root};
            struct vfs_mount *copy;
            struct vfs_mount *inventory=prior->ns ? ns->mounts : batch->pending;
            int rc=vfs_stage_bind_tree(batch,&view,&point,inventory,recursive,peer->peer_group!=0,
                                        kind,&copy);
            if(rc)return rc;
            struct vfs_mount *cover=vfs_covering(ns,&point);
            if(cover!=origin)copy->staged_cover=cover;
        }
        if(!deeper)return 0;
    }
}

/** @brief Publish a new mount and its downstream copies in one transaction.
 * @param ns Borrowed execution-owned namespace.
 * @param mount Caller-retained unpublished mount; membership takes its own ref.
 * @param target Borrowed retained directory path in ns.
 * @return Zero or negative errno, without consuming IDs or modifying the
 * mount on failure. An existing receiving stack stays above the new copy.
 */
int vfs_mount_attach(struct vfs_namespace *ns,struct vfs_mount *mount,const struct vfs_path *target)
{
    if(!ns || !ns->root || !mount || !vfs_path_valid(target))return -EINVAL;
    if(target->mount->ns!=ns)return -EXDEV;
    if(target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(target->dentry->disconnected)return -ENOENT;
    if(mount->ns || mount->point.mount || mount->lazy_detached || vfs_covering(ns,target))return -EBUSY;
    if(!ns->next_id)return -EOVERFLOW;
    struct vfs_mount_batch batch={.next_id=ns->next_id,.next_peer_id=ns->next_peer_id};
    uint64_t old_id=mount->id,old_group=mount->peer_group;
    if(target->mount->peer_group) {
        if(!batch.next_peer_id)return -EOVERFLOW;
        mount->peer_group=batch.next_peer_id++;
    }
    ++mount->references;
    vfs_stage_mount(&batch,mount,target);
    int rc=vfs_stage_propagation(ns,&batch,mount,target,false);
    rc=vfs_finish_mount_batch(ns,&batch,rc);
    if(rc){mount->id=old_id;mount->peer_group=old_group;}
    return rc;
}

/** @brief Publish a directory bind and all its downstream copies atomically.
 * @param ns Borrowed execution-owned namespace.
 * @param source Borrowed retained source directory in ns.
 * @param target Borrowed retained destination directory in ns.
 * @param recursive True to copy the source's bindable descendant branches.
 * @return Zero or negative errno without consuming IDs or partially publishing.
 */
static int vfs_bind_tree(struct vfs_namespace *ns,const struct vfs_path *source,
                         const struct vfs_path *target,bool recursive)
{
    if(!ns || !ns->root || !vfs_path_valid(source) || !vfs_path_valid(target))return -EINVAL;
    if(source->mount->ns!=ns || target->mount->ns!=ns)return -EXDEV;
    if(source->mount->unbindable)return -EINVAL;
    if(source->dentry->node.type!=RELIEFOS_FS_TYPE_DIR || target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(source->dentry->disconnected || target->dentry->disconnected)return -ENOENT;
    if(vfs_covering(ns,target))return -EBUSY;

    struct vfs_mount_batch batch={.next_id=ns->next_id,.next_peer_id=ns->next_peer_id};
    struct vfs_mount *first;
    bool shared=target->mount->peer_group!=0;
    int rc=vfs_stage_bind_tree(&batch,source,target,ns->mounts,recursive,shared,VFS_COPY_BIND,&first);
    if(!rc)rc=vfs_stage_propagation(ns,&batch,first,target,recursive);
    return vfs_finish_mount_batch(ns,&batch,rc);
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

/** @brief Publish a directory view with independent copies of its bindable subtrees.
 * @param ns Execution-owned namespace. @param source Retained subtree root.
 * @param target Retained destination directory.
 * @return Zero or negative errno without partial publication.
 */
int vfs_mount_bind_recursive(struct vfs_namespace *ns,const struct vfs_path *source,
                              const struct vfs_path *target)
{
    return vfs_bind_tree(ns,source,target,true);
}

/** @brief Stage and publish a move into a shared parent and all its receivers.
 * @param ns Borrowed execution-owned namespace.
 * @param origin Live move root in ns; its old parent must not be shared.
 * @param target Retained uncovered directory on a shared mount outside the tree.
 * @return Zero or negative errno; no mountpoints, live groups or ID counters
 * change on failure. The original mounts retain their IDs, policy and pins.
 */
static int vfs_move_into_shared(struct vfs_namespace *ns,struct vfs_mount *origin,
                                const struct vfs_path *target)
{
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next)
        if(mount->unbindable && vfs_mount_below(origin,mount))return -EINVAL;

    struct vfs_mount_batch batch={.next_id=ns->next_id,.next_peer_id=ns->next_peer_id};
    int rc=0;
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        if(mount->peer_group || !vfs_mount_below(origin,mount))continue;
        if(!batch.next_peer_id){rc=-EOVERFLOW;break;}
        mount->staged_peer_group=batch.next_peer_id++;
    }
    if(!rc)rc=vfs_stage_propagation(ns,&batch,origin,target,true);
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        if(!mount->staged_peer_group)continue;
        if(!rc)mount->peer_group=mount->staged_peer_group;
        mount->staged_peer_group=0;
    }
    if(!rc)vfs_replace_mountpoint(origin,target);
    return vfs_finish_mount_batch(ns,&batch,rc);
}

/** @brief Relocate a mount tree intact, propagating it under a shared parent.
 * @param ns Borrowed execution-owned namespace.
 * @param source Retained mounted root, excluding the namespace anchor.
 * @param target Retained destination directory in ns.
 * @return Zero or negative errno before mutation. Moving out of a shared
 * parent or moving any unbindable mount into a shared parent is invalid.
 */
int vfs_mount_move(struct vfs_namespace *ns,const struct vfs_path *source,const struct vfs_path *target)
{
    if(!ns || !vfs_path_valid(source) || !vfs_path_valid(target))return -EINVAL;
    struct vfs_mount *mount=source->mount;
    if(mount->ns!=ns || target->mount->ns!=ns)return -EXDEV;
    if(source->dentry!=mount->root)return -EINVAL;
    if(mount==ns->root || !mount->point.mount)return -EBUSY;
    if(target->dentry->node.type!=RELIEFOS_FS_TYPE_DIR)return -ENOTDIR;
    if(mount->point.mount->peer_group)return -EINVAL;
    if(target->dentry->disconnected)return -ENOENT;
    for(struct vfs_mount *ancestor=target->mount;ancestor;ancestor=ancestor->point.mount)
        for(struct vfs_mount *backing=ancestor;backing;backing=backing->bind_source.mount)
            if(backing==mount)return -ELOOP;
    if(vfs_covering(ns,target))return -EBUSY;
    if(target->mount->peer_group)return vfs_move_into_shared(ns,mount,target);
    vfs_replace_mountpoint(mount,target);return 0;
}

/** @brief Disconnect a single leaf and relink its propagation receivers.
 * @param ns Borrowed execution-owned namespace.
 * @param mount Live leaf with no remaining dependents in ns.
 */
static void vfs_mount_detach(struct vfs_namespace *ns,struct vfs_mount *mount)
{
    vfs_leave_peer_group(ns,mount);
    struct vfs_mount **link=&ns->mounts;
    while(*link && *link!=mount)link=&(*link)->next;
    if(*link)*link=mount->next;
    struct vfs_path old=mount->point;mount->point=(struct vfs_path){0};
    mount->ns=NULL;mount->next=NULL;mount->master=NULL;mount->unbindable=false;
    mount->propagation_copy=false;
    vfs_path_put(&old);vfs_mount_put(mount);
}

/** @brief Identify a surviving cover that can be lifted out of an unmount batch.
 * @param mount Borrowed candidate mount, whose visible parent might detach.
 * @return True if the candidate covers the root of a propagation receiver and
 * its retained parent edge can be transferred to the receiver's old slot.
 */
static bool vfs_removable_cover_edge(const struct vfs_mount *mount)
{
    const struct vfs_mount *parent=mount->point.mount;
    return parent && parent->propagation_copy && mount->point.dentry==parent->root;
}

/** @brief Mark origin mounts and the matching slots of propagation receivers.
 * @param ns Borrowed namespace under execution ownership.
 * @param root Root of the requested unmount.
 * @param subtree True to send events for the entire original subtree.
 * The parents' peer groups and downstream edges determine delivery. A receiver
 * with an unrelated child stays, except a root cover that can be lifted.
 */
static void vfs_select_unmounts(struct vfs_namespace *ns,struct vfs_mount *root,bool subtree)
{
    bool propagated=false;
    for(struct vfs_mount *event=subtree ? ns->mounts : root;event;
        event=subtree ? event->next : NULL) {
        if(subtree && !vfs_mount_below(root,event))continue;
        event->unmount_selected=true;
        struct vfs_mount *parent=event->point.mount;
        if(!parent || !parent->peer_group)continue;
        for(struct vfs_mount *receiver=ns->mounts;receiver;receiver=receiver->next) {
            const struct vfs_path *point=&receiver->point;
            uint64_t depth;
            if(point->mount && point->mount!=parent && point->dentry==event->point.dentry &&
               vfs_propagation_depth(parent,point->mount,&depth)) {
                receiver->unmount_selected=true;propagated=true;
            }
        }
    }
    if(propagated) {
        for(struct vfs_mount *child=ns->mounts;child;child=child->next) {
            if(child->unmount_selected)continue;
            struct vfs_mount *parent=child->point.mount;
            if(!vfs_removable_cover_edge(child))
                while(parent && parent->unmount_selected) {
                    parent->unmount_selected=false;
                    if(vfs_removable_cover_edge(parent))break;
                    parent=parent->point.mount;
                }
        }
        for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
            mount->lift_cover=!mount->unmount_selected && vfs_removable_cover_edge(mount) &&
                mount->point.mount->unmount_selected;
        }
    }
}

/** @brief Reject busy mounts before changing any member of an unmount batch.
 * @param ns Borrowed execution-owned namespace with selected mounts marked.
 * @param root Origin mount with the caller's lookup reference.
 * @return Zero or negative busy/provider errno. Replica lifetime pins and
 * edges disappearing or being reparented in this batch are not external path
 * references. A regular bind outside the batch still pins its source as before.
 */
static int vfs_unmount_preflight(const struct vfs_namespace *ns,const struct vfs_mount *root)
{
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        if(!mount->unmount_selected)continue;
        uint64_t expected=1+(mount==root)+mount->propagation_pins;
        struct vfs_mount *backing=mount->propagation_backing ? mount->bind_source.mount : mount;
        uint64_t replicas=0;
        bool first_replica=true;
        for(struct vfs_mount *other=ns->mounts;other;other=other->next) {
            if((other->unmount_selected || other->lift_cover) && other->point.mount==mount)++expected;
            if(!other->unmount_selected)continue;
            if(other->bind_source.mount==mount && !other->propagation_backing)++expected;
            if(other->bind_source.mount==backing && other->propagation_backing) {
                ++replicas;
                if(other->id<mount->id)first_replica=false;
            }
        }
        if(mount->references!=expected)return -EBUSY;
        /* Check native backing users once only when origin and all lifetime
         * replicas are going away. The origin may already be detached. */
        bool final_check=mount->propagation_backing ? !backing->ns && first_replica &&
            backing->references==backing->propagation_pins : !mount->bind_source.mount;
        if(final_check && replicas==backing->propagation_pins && backing->ops->can_unmount) {
            int rc=backing->ops->can_unmount(backing->context,backing->root->backend_pin);
            if(rc)return rc;
        }
    }
    return 0;
}

/** @brief Unlink the entire selected graph before dropping retained edges.
 * @param ns Borrowed execution-owned namespace with selected mounts marked.
 * @param lazy True to keep external paths walkable and defer final teardown.
 * Reparent preserved covers and repair weak propagation edges before removing
 * each member. No allocation, and lazy unmount invokes no backend finish.
 */
static void vfs_disconnect_unmounts(struct vfs_namespace *ns,bool lazy)
{
    for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next) {
        if(!mount->lift_cover)continue;
        const struct vfs_path *point=&mount->point.mount->point;
        while(point->mount->unmount_selected)point=&point->mount->point;
        vfs_replace_mountpoint(mount,point);
        mount->lift_cover=false;
    }
    struct vfs_mount *detached=NULL,**link=&ns->mounts;
    while(*link) {
        struct vfs_mount *mount=*link;
        if(!mount->unmount_selected){link=&mount->next;continue;}
        vfs_leave_peer_group(ns,mount);
        *link=mount->next;mount->next=detached;detached=mount;
        mount->ns=NULL;mount->lazy_detached=lazy;
        mount->master=NULL;mount->unbindable=false;
        mount->unmount_selected=false;mount->propagation_copy=false;
    }
    while(detached) {
        struct vfs_mount *mount=detached;detached=mount->next;mount->next=NULL;
        vfs_path_put(&mount->point);vfs_mount_put(mount);
    }
}

/** @brief Detach one idle mount and all removable counterparts atomically.
 * @param ns Borrowed execution-owned namespace.
 * @param path Retained origin root, excluding the permanent anchor.
 * @return Zero or negative errno; failures leave all attachments intact.
 */
int vfs_mount_unmount(struct vfs_namespace *ns,const struct vfs_path *path)
{
    if(!ns || !vfs_path_valid(path))return -EINVAL;
    struct vfs_mount *root=path->mount;
    if(root->ns!=ns)return -EXDEV;
    if(path->dentry!=root->root)return -EINVAL;
    if(root==ns->root || !root->point.mount)return -EBUSY;
    for(struct vfs_mount *child=ns->mounts;child;child=child->next)
        if(child->point.mount==root)return -EBUSY;

    vfs_select_unmounts(ns,root,false);
    int rc=vfs_unmount_preflight(ns,root);
    if(!rc)vfs_disconnect_unmounts(ns,false);
    else for(struct vfs_mount *mount=ns->mounts;mount;mount=mount->next)
        mount->unmount_selected=mount->lift_cover=false;
    return rc;
}

/** @brief Disconnect a visible subtree and its removable propagated counterparts.
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
    vfs_select_unmounts(ns,root,true);
    vfs_disconnect_unmounts(ns,true);return 0;
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
