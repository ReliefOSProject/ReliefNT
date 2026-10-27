/* SPDX-License-Identifier: Apache-2.0 */
#include "mount_internal.h"
#include <linux/mount.h>


struct mount_text {uint64_t position,offset;uint32_t count,capacity;char *bytes;};

/** @brief Describe a live backend attachment without replaying a target label.
 * @param ns Borrowed namespace under the caller's execution transaction.
 * @param context Borrowed non-NULL backend context, compared without callbacks.
 * @param buffer Required diagnostic destination, empty on failure.
 * @param capacity Available bytes including NUL; no truncation is returned.
 * @return Zero or negative input/absent/format errno. Chooses the oldest stable
 * ID for a multiply attached context. Detached but still referenced backends
 * are absent. Pure graph traversal, no allocation/I/O or ownership transfer;
 * the output must never select an inode or authorize a filesystem operation.
 */
int vfs_namespace_context_mountpoint(const struct vfs_namespace *ns,const void *context,char *buffer,size_t capacity)
{
    if(!buffer || !capacity)return -EINVAL;buffer[0]=0;
    if(!ns || !ns->root || !context)return -EINVAL;
    const struct vfs_mount *selected=NULL;
    for(const struct vfs_mount *mount=ns->mounts;mount;mount=mount->next)
        if(mount->context==context && (!selected || mount->id<selected->id))selected=mount;
    if(!selected)return -ENOENT;
    struct vfs_path root={ns->root,ns->root->root},path={(struct vfs_mount *)selected,selected->root};
    return vfs_path_format(ns,&root,&path,buffer,capacity);
}

static void mount_emit(struct mount_text *text,const char *value)
{
    for(;*value;++value) {
        if(text->position>=text->offset && text->count<text->capacity)
            text->bytes[text->count++]=*value;
        ++text->position;
    }
}
static void mount_field(struct mount_text *text,const char *value)
{
    for(;*value;++value) {
        const char *escaped=*value==' ' ? "\\040" : *value=='\t' ? "\\011" :
            *value=='\n' ? "\\012" : *value=='\\' ? "\\134" : NULL;
        char character[]={*value,0};mount_emit(text,escaped ? escaped : character);
    }
}
static void mount_number(struct mount_text *text,uint64_t number)
{
    char value[21];unsigned used=sizeof(value);value[--used]=0;
    do {value[--used]=(char)('0'+number%10);number/=10;}while(number);
    mount_emit(text,value+used);
}
static void mount_options(struct mount_text *text,uint64_t flags,bool mountinfo,bool superblock)
{
    mount_emit(text,flags&MS_RDONLY ? "ro" : "rw");
    static const struct {uint64_t flag;const char *text;bool superblock;} options[]={
        {MS_NOSUID,",nosuid",false},{MS_NODEV,",nodev",false},{MS_NOEXEC,",noexec",false},
        {MS_NOATIME,",noatime",false},{MS_NODIRATIME,",nodiratime",false},{MS_RELATIME,",relatime",false},
        {MS_SYNCHRONOUS,",sync",true},{MS_DIRSYNC,",dirsync",true},{MS_LAZYTIME,",lazytime",true}
    };
    for(size_t i=0;i<sizeof(options)/sizeof(options[0]);++i)
        if((flags&options[i].flag) && (!mountinfo || options[i].superblock==superblock))mount_emit(text,options[i].text);
}
/** @brief Format one visible attachment with separate mount/SB option fields.
 * @param ns Borrowed namespace. @param root Retained viewer root.
 * @param mount Borrowed graph attachment, pinned by execution ownership.
 * @param info True for mountinfo, false for combined mounts options.
 * @param text Borrowed bounded stream state. @param workspace Two VFS_PATH_MAX buffers.
 * @return Zero, hidden-root success or negative path-format errno. No
 * allocation/I/O/ownership change; flags are read from their actual owners.
 */
static int mount_line(const struct vfs_namespace *ns,const struct vfs_path *root,
                         const struct vfs_mount *mount,bool info,struct mount_text *text,char *workspace)
{
    char *point=workspace,*backing=workspace+VFS_PATH_MAX;int rc;
    struct vfs_path mounted_root={(struct vfs_mount *)mount,mount->root};
    rc=vfs_path_format(ns,root,&mounted_root,point,VFS_PATH_MAX);
    if(rc==-ENOENT)return 0;
    backing[0]='/';backing[1]=0;
    if(rc)return rc;
    struct vfs_path path={(struct vfs_mount *)mount,mount->root};
    uint64_t flags=vfs_path_mount_flags(&path);
    const char *source=mount->source ? mount->source : "none";
    const char *filesystem=mount->filesystem ? mount->filesystem : "none";
    if(info) {
        mount_number(text,mount->id);mount_emit(text," ");
        mount_number(text,mount->point.mount ? mount->point.mount->id : mount->id);mount_emit(text," ");
        mount_number(text,(mount->device>>8)&0xfff);mount_emit(text,":");
        mount_number(text,(mount->device&255)|((mount->device>>12)&~255ULL));mount_emit(text," ");
        mount_field(text,backing);mount_emit(text," ");mount_field(text,point);mount_emit(text," ");
        mount_options(text,mount->policy,true,false);mount_emit(text," - ");mount_emit(text,filesystem);mount_emit(text," ");
        mount_field(text,source);mount_emit(text," ");mount_options(text,vfs_super_flags(mount),true,true);mount_emit(text,"\n");
    } else {
        mount_field(text,source);mount_emit(text," ");mount_field(text,point);mount_emit(text," ");
        mount_emit(text,filesystem);mount_emit(text," ");mount_options(text,flags,false,false);mount_emit(text," 0 0\n");
    }
    return 0;
}

/** @brief Select the next attachment in a parent-before-child graph walk.
 * @param ns Borrowed namespace pinned by the caller's execution transaction.
 * @param boundary Viewing root mount; traversal never ascends beyond it.
 * @param mount Current attachment within the acyclic boundary subtree.
 * @return Borrowed next attachment or NULL at the end. Siblings use stable
 * mount IDs, but ancestry comes from retained parent pointers: MS_MOVE may
 * give an older mount a newer parent. No allocation, recursion, callbacks,
 * reference changes or graph lock; scratch space is independent of depth.
 */
static const struct vfs_mount *mount_successor(const struct vfs_namespace *ns,
                         const struct vfs_mount *boundary,const struct vfs_mount *mount)
{
    const struct vfs_mount *next=NULL;
    for(const struct vfs_mount *candidate=ns->mounts;candidate;candidate=candidate->next)
        if(candidate->point.mount==mount && (!next || candidate->id<next->id))next=candidate;
    if(next)return next;
    for(const struct vfs_mount *node=mount;node!=boundary;node=node->point.mount) {
        const struct vfs_mount *parent=node->point.mount;
        if(!parent)return NULL;
        for(const struct vfs_mount *candidate=ns->mounts;candidate;candidate=candidate->next)
            if(candidate->point.mount==parent && candidate->id>node->id &&
               (!next || candidate->id<next->id))next=candidate;
        if(next)return next;
    }
    return NULL;
}

/** @brief Stream visible mounts with every visible parent before its children.
 * @param ns Required execution-owned namespace; graph mutation is serialized.
 * @param root Retained viewing root; attachments outside it are omitted.
 * @param mountinfo True for mountinfo, false for escaped mntent records.
 * @param offset Byte offset into this live graph view.
 * @param buffer Output storage, nullable only when capacity is zero.
 * @param capacity Maximum output bytes; no terminating NUL is appended.
 * @param read Optional byte count, cleared on input/allocation/format failure.
 * @return Zero or negative input/namespace/allocation/path-format errno. Uses
 * two PATH_MAX scratch buffers and a nonrecursive graph walk; providers stay
 * pinned, no graph lock or new inode I/O spans the read. Mount IDs and parent
 * relationships remain unchanged, including after MS_MOVE or under chroot.
 */
int vfs_read_mount_table(const struct vfs_namespace *ns,const struct vfs_path *root,bool mountinfo,
                            uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{
    if(read)*read=0;
    if(!ns || !ns->root || !vfs_path_valid(root) || (!buffer && capacity))return -EINVAL;
    if(root->mount->ns!=ns)return -EXDEV;
    if(!capacity)return 0;
    char *workspace=kernel_malloc(2*VFS_PATH_MAX);if(!workspace)return -ENOMEM;
    struct mount_text text={.offset=offset,.bytes=buffer,.capacity=capacity};
    int rc=0;
    for(const struct vfs_mount *mount=root->mount;!rc && mount;
        mount=mount_successor(ns,root->mount,mount))
        rc=mount_line(ns,root,mount,mountinfo,&text,workspace);
    kernel_free(workspace);if(!rc && read)*read=text.count;return rc;
}
