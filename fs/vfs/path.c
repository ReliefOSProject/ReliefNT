/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"

/* Copy a bounded userspace-style pathname already owned by the kernel. */
static size_t vfs_path_length(const char *name)
{
    size_t length=0;if(!name)return VFS_PATH_MAX;
    while(length<VFS_PATH_MAX && name[length])++length;
    return length;
}

enum vfs_walk_phase {VFS_WALK_SCAN,VFS_WALK_SEARCH,VFS_WALK_LOOKUP,VFS_WALK_LINK};
struct vfs_walk {
    struct vfs_namespace *ns;
    struct vfs_path root,current,child;
    char *pending,*replacement;
    size_t cursor,after,used;
    unsigned flags,links;
    enum vfs_walk_phase phase;
    bool last;
    char component[VFS_NAME_MAX+1];
};

void vfs_walk_cancel(struct vfs_walk **state)
{
    if(!state || !*state)return;
    struct vfs_walk *walk=*state;*state=NULL;
    kernel_free(walk->pending);kernel_free(walk->replacement);
    vfs_path_put(&walk->child);vfs_path_put(&walk->current);vfs_path_put(&walk->root);
    kernel_free(walk);
}

/* Own all cursor text and objects before the first backend search/lookup.
 * Only per-call policy callbacks are borrowed; no callback context survives
 * EAGAIN. A retained current object never follows a newly added overmount. */
static int vfs_walk_new(struct vfs_namespace *ns,const struct vfs_path *root,
                           const struct vfs_path *start,const char *name,unsigned flags,
                           struct vfs_walk **state)
{
    if(!ns || !vfs_path_valid(root) || !vfs_path_valid(start) || !name)return -EINVAL;
    if(root->mount->ns!=ns || start->mount->ns!=ns)return -EXDEV;
    size_t length=vfs_path_length(name);if(length>=VFS_PATH_MAX)return -ENAMETOOLONG;
    if(!length)return -ENOENT;
    struct vfs_walk *walk=kernel_malloc(sizeof(*walk));if(!walk)return -ENOMEM;
    *walk=(struct vfs_walk){.ns=ns,.flags=flags};
    walk->pending=kernel_malloc(VFS_PATH_MAX);
    if(!walk->pending){kernel_free(walk);return -ENOMEM;}
    __builtin_memcpy(walk->pending,name,length+1);
    vfs_path_get(root,&walk->root);vfs_path_get(name[0]=='/' ? root : start,&walk->current);
    *state=walk;return 0;
}

int vfs_path_resolve_resume(struct vfs_namespace *ns,const struct vfs_path *root,
                              const struct vfs_path *start,const char *name,
                              const struct vfs_lookup_policy *policy,struct vfs_walk **state,
                              struct vfs_lookup_result *result)
{
    if(!state || !result)return -EINVAL;
    *result=(struct vfs_lookup_result){0};
    unsigned flags=policy ? policy->flags : VFS_LOOKUP_FOLLOW;
    int rc=flags&~(VFS_LOOKUP_FOLLOW|VFS_LOOKUP_PARENT|VFS_LOOKUP_MISSING) ? -EINVAL : 0;
    if(!rc && *state && ((*state)->ns!=ns || (*state)->flags!=flags))rc=-EINVAL;
    if(!rc && !*state)rc=vfs_walk_new(ns,root,start,name,flags,state);
    if(rc){vfs_walk_cancel(state);return rc;}
    struct vfs_walk *walk=*state;bool done=false;
    while(!rc && !done) {
        switch(walk->phase) {
        case VFS_WALK_SCAN: {
            size_t begin=walk->cursor;
            while(walk->pending[walk->cursor]=='/')++walk->cursor;
            if(!walk->pending[walk->cursor]) {
                if(walk->cursor!=begin && walk->current.dentry->node.type!=RELIEFOS_FS_TYPE_DIR)rc=-ENOTDIR;
                else done=true;
                break;
            }
            if(walk->current.dentry->node.type!=RELIEFOS_FS_TYPE_DIR){rc=-ENOTDIR;break;}
            walk->phase=VFS_WALK_SEARCH;break;
        }
        case VFS_WALK_SEARCH: {
            if(policy && policy->search)rc=policy->search(policy->context,&walk->current);
            if(rc)break;
            size_t used=0;
            const char *cursor=walk->pending+walk->cursor;
            while(cursor[used] && cursor[used]!='/') {
                if(used==VFS_NAME_MAX){rc=-ENAMETOOLONG;break;}
                walk->component[used]=cursor[used];++used;
            }
            if(rc)break;
            walk->component[used]=0;walk->used=used;walk->after=walk->cursor+used;
            const char *rest=walk->pending+walk->after;while(*rest=='/')++rest;
            walk->last=!*rest;
            if((flags&VFS_LOOKUP_PARENT) && walk->last) {
                __builtin_memcpy(result->last,walk->component,used+1);
                result->trailing=walk->pending[walk->after]!=0;done=true;break;
            }
            if(used==1 && walk->component[0]=='.') {
                walk->cursor=walk->after;walk->phase=VFS_WALK_SCAN;break;
            }
            if(used==2 && walk->component[0]=='.' && walk->component[1]=='.') {
                vfs_path_parent(&walk->root,&walk->current);
                walk->cursor=walk->after;walk->phase=VFS_WALK_SCAN;break;
            }
            walk->phase=VFS_WALK_LOOKUP;break;
        }
        case VFS_WALK_LOOKUP: {
            rc=vfs_path_lookup_child(&walk->current,walk->component,&walk->child);
            if(rc==-ENOENT && (flags&VFS_LOOKUP_MISSING) && walk->last) {
                __builtin_memcpy(result->last,walk->component,walk->used+1);
                result->trailing=walk->pending[walk->after]!=0;result->missing=true;rc=0;done=true;break;
            }
            if(rc)break;
            vfs_follow_mounts(ns,&walk->child);
            if(walk->child.dentry->node.type==RELIEFOS_FS_TYPE_SYMLINK &&
               (walk->pending[walk->after] || flags&VFS_LOOKUP_FOLLOW)) {
                if(++walk->links>VFS_SYMLINK_MAX){rc=-ELOOP;break;}
                walk->phase=VFS_WALK_LINK;break;
            }
            vfs_path_put(&walk->current);walk->current=walk->child;walk->child=(struct vfs_path){0};
            walk->cursor=walk->after;walk->phase=VFS_WALK_SCAN;break;
        }
        case VFS_WALK_LINK: {
            if(walk->child.dentry->node.flags&STORAGE_NODE_FLAG_MAGIC_LINK) {
                struct vfs_path target={0};
                rc=walk->child.mount->ops->follow_link ? walk->child.mount->ops->follow_link(
                    walk->child.mount->context,&walk->child.dentry->node,&target) : -EOPNOTSUPP;
                if(!rc && !vfs_path_valid(&target))rc=-EINVAL;
                if(rc){vfs_path_put(&target);break;}
                /* Like nd_jump_link, an authorized magic target is exact:
                 * never redirect an already retained FD through a cover. */
                vfs_path_put(&walk->child);vfs_path_put(&walk->current);walk->current=target;
                walk->cursor=walk->after;walk->phase=VFS_WALK_SCAN;break;
            }
            if(!walk->child.mount->ops->readlink){rc=-EINVAL;break;}
            if(!walk->replacement)walk->replacement=kernel_malloc(VFS_PATH_MAX);
            if(!walk->replacement){rc=-ENOMEM;break;}
            size_t length=0;
            rc=walk->child.mount->ops->readlink(walk->child.mount->context,&walk->child.dentry->node,
                walk->replacement,VFS_PATH_MAX,&length);
            if(rc)break;
            size_t suffix=__builtin_strlen(walk->pending+walk->after);
            if(!length || length>=VFS_PATH_MAX || suffix>=VFS_PATH_MAX-length) {
                rc=length ? -ENAMETOOLONG : -ENOENT;break;
            }
            __builtin_memcpy(walk->replacement+length,walk->pending+walk->after,suffix+1);
            if(walk->replacement[0]=='/') {
                vfs_path_put(&walk->current);vfs_path_get(&walk->root,&walk->current);
            }
            vfs_path_put(&walk->child);kernel_free(walk->pending);
            walk->pending=walk->replacement;walk->replacement=NULL;
            walk->cursor=0;walk->phase=VFS_WALK_SCAN;break;
        }
        }
    }
    if(rc==-EAGAIN)return rc;
    if(!rc){result->path=walk->current;walk->current=(struct vfs_path){0};}
    vfs_walk_cancel(state);return rc;
}

int vfs_path_resolve(struct vfs_namespace *ns,const struct vfs_path *root,
                       const struct vfs_path *start,const char *name,
                       const struct vfs_lookup_policy *policy,struct vfs_lookup_result *result)
{
    struct vfs_walk *state=NULL;
    int rc=vfs_path_resolve_resume(ns,root,start,name,policy,&state,result);
    /* Trusted unscoped callers retain the historical all-or-nothing API. */
    vfs_walk_cancel(&state);return rc;
}

int vfs_path_lookup(struct vfs_namespace *ns,const struct vfs_path *root,
                     const struct vfs_path *start,const char *name,unsigned flags,
                     struct vfs_path *out)
{
    if(!out)return -EINVAL;*out=(struct vfs_path){0};
    if(flags&~VFS_LOOKUP_FOLLOW)return -EINVAL;
    struct vfs_lookup_policy policy={.flags=flags};struct vfs_lookup_result result={0};
    int rc=vfs_path_resolve(ns,root,start,name,&policy,&result);
    if(!rc)*out=result.path;return rc;
}

static int vfs_path_format_walk(const struct vfs_path *root,const struct vfs_path *path,
                                  char *buffer,size_t capacity,bool diagnostic)
{
    if(!buffer || !capacity)return -EINVAL;buffer[0]=0;
    struct vfs_path cursor=*path;size_t at=capacity-1;buffer[at]=0;
    while(!vfs_path_equal(root,&cursor)) {
        if(cursor.dentry->disconnected && !diagnostic){buffer[0]=0;return -ENOENT;}
        if(cursor.dentry==cursor.mount->root) {
            if(!cursor.mount->point.mount){buffer[0]=0;return -ENOENT;}
            cursor=cursor.mount->point;continue;
        }
        if(!cursor.dentry->name){buffer[0]=0;return -ENOENT;}
        size_t length=__builtin_strlen(cursor.dentry->name);
        if(length+1>at){buffer[0]=0;return -ENAMETOOLONG;}
        at-=length;__builtin_memcpy(buffer+at,cursor.dentry->name,length);
        buffer[--at]='/';cursor.dentry=cursor.dentry->parent;
    }
    if(at==capacity-1) {
        if(!at){buffer[0]=0;return -ENAMETOOLONG;}
        buffer[--at]='/';
    }
    __builtin_memmove(buffer,buffer+at,capacity-at);return 0;
}

int vfs_path_format(const struct vfs_namespace *ns,const struct vfs_path *root,
                     const struct vfs_path *path,char *buffer,size_t capacity)
{
    if(!buffer || !capacity)return -EINVAL;buffer[0]=0;
    if(!ns || !vfs_path_valid(root) || !vfs_path_valid(path))return -EINVAL;
    if(root->mount->ns!=ns || path->mount->ns!=ns)return -EXDEV;
    return vfs_path_format_walk(root,path,buffer,capacity,false);
}

int vfs_path_format_link(const struct vfs_namespace *ns,const struct vfs_path *root,
                            const struct vfs_path *path,char *buffer,size_t capacity)
{
    if(!buffer || !capacity)return -EINVAL;buffer[0]=0;
    if(!vfs_path_valid(path))return -EINVAL;
    if(path->dentry!=path->mount->root && !path->dentry->name)
        return path->mount->ops->display_name ? path->mount->ops->display_name(path->mount->context,
            &path->dentry->node,buffer,capacity) : -EOPNOTSUPP;
    if(!ns || !ns->root || !vfs_path_valid(root))return -EINVAL;
    if(root->mount->ns!=ns || path->mount->ns!=ns)return -EXDEV;
    int rc=vfs_path_format_walk(root,path,buffer,capacity,true);
    if(rc==-ENOENT) {
        struct vfs_path anchor={ns->root,ns->root->root};
        rc=vfs_path_format_walk(&anchor,path,buffer,capacity,true);
    }
    if(!rc && path->dentry->disconnected) {
        static const char marker[]=" (deleted)";size_t length=__builtin_strlen(buffer);
        if(sizeof(marker)>capacity-length){buffer[0]=0;return -ENAMETOOLONG;}
        __builtin_memcpy(buffer+length,marker,sizeof(marker));
    }
    return rc;
}

int vfs_path_backend_name(const struct vfs_path *path,char *buffer,size_t capacity)
{
    if(!buffer || !capacity)return -EINVAL;buffer[0]=0;
    if(!vfs_path_valid(path))return -EINVAL;
    struct vfs_path root={path->mount,path->mount->root};
    return vfs_path_format_walk(&root,path,buffer,capacity,false);
}

int vfs_path_getcwd(const struct vfs_namespace *ns,const struct vfs_path *root,
                       const struct vfs_path *cwd,char *buffer,size_t capacity)
{
    int rc=vfs_path_format(ns,root,cwd,buffer,capacity);
    if(!rc && cwd->dentry->disconnected){buffer[0]=0;return -ENOENT;}
    if(rc!=-ENOENT)return rc;
    static const char marker[]="(unreachable)";
    size_t prefix=sizeof(marker)-1;
    if(capacity<=prefix)return -ENAMETOOLONG;
    struct vfs_path anchor={ns->root,ns->root->root};
    rc=vfs_path_format(ns,&anchor,cwd,buffer+prefix,capacity-prefix);
    if(!rc)__builtin_memcpy(buffer,marker,prefix);
    else buffer[0]=0;
    return rc;
}
