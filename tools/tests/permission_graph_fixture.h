/* SPDX-License-Identifier: Apache-2.0 */
/* Only the inode provider is a fixture. Production code owns every component
 * walk, symlink, mount/path reference, DAC check and creation/sticky policy. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/syscall/internal.h"

static struct fixture {
    const char *path;
    uint32_t type;
    struct reliefos_permissions value;
    const char *target;
    unsigned parent,pins;
    char name[VFS_NAME_MAX+1];
    uint32_t flags,kind,volume;
} entries[32]={
    {"/",RELIEFOS_FS_TYPE_DIR,{0755,0,0}},
    {"/data",RELIEFOS_FS_TYPE_DIR,{0777,100,200}},
    {"/data/file",RELIEFOS_FS_TYPE_FILE,{0640,100,200}},
    {"/data/dir",RELIEFOS_FS_TYPE_DIR,{0755,100,200}},
    {"/jump",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"data/dir"},
    {"/data/link",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"file"},
    {"/absolute",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"/data/link"},
    {"/loop",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"loop"},
    {"/self",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"."},
    {"/dangling",RELIEFOS_FS_TYPE_SYMLINK,{0777,100,200},"missing"},
    {"/dev",RELIEFOS_FS_TYPE_DIR,{0755,0,0}},
    {"/dev/snd",RELIEFOS_FS_TYPE_DIR,{0755,0,0}},
    {.path="/dev/snd/pcmC0D0c",.type=RELIEFOS_FS_TYPE_DEVICE,
     .flags=STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_AUDIO_PCM,
     .kind=STORAGE_DEV_KIND_AUDIO,.volume=0x123401},
    {"/dev/dri",RELIEFOS_FS_TYPE_DIR,{0755,0,0}},
    {.path="/dev/dri/renderD128",.type=RELIEFOS_FS_TYPE_DEVICE,
     .flags=STORAGE_NODE_FLAG_DEV_NODE,.kind=STORAGE_DEV_KIND_DRM_RENDERD128},
    {"/dev/pts",RELIEFOS_FS_TYPE_DIR,{0755,0,0}},
    {.path="/dev/pts/1",.type=RELIEFOS_FS_TYPE_DEVICE,
     .flags=STORAGE_NODE_FLAG_PTY|STORAGE_NODE_FLAG_DEV_NODE}
};
static unsigned entry_count=17;
static size_t allocations;
static struct vfs_namespace fixture_ns;
static struct vfs_path fixture_root;
static struct reliefos_permissions pty_permissions={0600,100,200};
void *kernel_malloc(size_t size)
{void *p=calloc(1,size);if(p)++allocations;return p;}
void kernel_free(void *p)
{if(p){assert(allocations);--allocations;free(p);}}
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{assert(node->flags&STORAGE_NODE_FLAG_PTY);if(write)pty_permissions=*value;else *value=pty_permissions;return 0;}
static struct fixture *inode_entry(const struct storage_node *node)
{assert(node && node->inode_generation && node->inode_generation<=entry_count);return &entries[node->inode_generation-1];}
static struct storage_node entry_node(unsigned index)
{
    struct fixture *f=&entries[index];
    return (struct storage_node){.type=f->type,.inode_generation=index+1,
        .first_cluster=f->flags ? f->kind : index+1,.flags=f->flags,.volume_id=f->volume};
}
static int fixture_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    assert(context==entries);
    for(unsigned i=1;i<entry_count;++i)
        if(entries[i].parent==parent->inode_generation && !strcmp(entries[i].name,name))
            {*out=entry_node(i);return 0;}
    return -ENOENT;
}
static int fixture_retain(void *context,const struct storage_node *node,void **pin)
{assert(context==entries);struct fixture *f=inode_entry(node);++f->pins;*pin=f;return 0;}
static void fixture_release(void *context,const struct storage_node *node,void *pin)
{assert(context==entries);struct fixture *f=inode_entry(node);assert(f==pin && f->pins);--f->pins;}
static bool fixture_same(void *context,const struct storage_node *a,const struct storage_node *b)
{assert(context==entries);return a->inode_generation==b->inode_generation;}
static int fixture_permissions(void *context,const struct vfs_path *path,struct reliefos_permissions *value,bool write)
{
    assert(context==entries);const struct storage_node *node=vfs_path_node(path);struct fixture *f=inode_entry(node);
    if(f->flags)return fs_virtual_permissions(f->name,node,value,write);
    if(write)f->value=*value;else *value=f->value;return 0;
}
static int fixture_link_bytes(void *context,const struct storage_node *node,char *buffer,size_t capacity,size_t *length)
{
    assert(context==entries);const char *target=inode_entry(node)->target;if(!target)return -EINVAL;
    *length=strlen(target);if(*length>=capacity)return -ENAMETOOLONG;memcpy(buffer,target,*length);return 0;
}
static int fixture_remount(void *context,uint64_t flags)
{assert(context==entries);(void)flags;return 0;}
static int fixture_create(void *context,const struct storage_node *parent,const char *name,uint32_t type,
                          const struct reliefos_permissions *value,const char *target,struct storage_node *out,void **pin)
{
    assert(context==entries);struct storage_node existing;
    if(!fixture_lookup(context,parent,name,&existing))return -EEXIST;
    if(entry_count==sizeof(entries)/sizeof(entries[0]))return -ENOSPC;
    unsigned index=entry_count++;struct fixture *f=&entries[index];
    *f=(struct fixture){.parent=parent->inode_generation,.value=*value,.target=target};
    f->type=type==LINUX_S_IFDIR ? RELIEFOS_FS_TYPE_DIR :
        type==LINUX_S_IFLNK ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_FILE;
    assert(strlen(name)<sizeof(f->name));strcpy(f->name,name);*out=entry_node(index);
    if(pin){++f->pins;*pin=f;}return 0;
}
static const struct vfs_backend_ops fixture_ops={.lookup=fixture_lookup,.retain=fixture_retain,
    .release=fixture_release,.same_node=fixture_same,.readlink=fixture_link_bytes,
    .permissions=fixture_permissions,.remount=fixture_remount,.create_node=fixture_create};
static void fixture_start(void)
{
    for(unsigned i=1;i<entry_count;++i) {
        const char *slash=strrchr(entries[i].path,'/');assert(slash);strcpy(entries[i].name,slash+1);
        size_t length=(size_t)(slash-entries[i].path);if(!length)length=1;
        bool found=false;
        for(unsigned p=0;p<entry_count;++p)
            if(strlen(entries[p].path)==length && !memcmp(entries[p].path,entries[i].path,length))
                {entries[i].parent=p+1;found=true;break;}
        assert(found);
    }
    struct storage_node node=entry_node(0);struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&fixture_ops,entries,NULL,&node,&mount));
    assert(!vfs_namespace_init(&fixture_ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&fixture_ns,&fixture_root));
}
static void fixture_finish(void)
{
    vfs_path_put(&fixture_root);assert(!vfs_namespace_destroy(&fixture_ns));
    for(unsigned i=0;i<entry_count;++i)assert(!entries[i].pins);
    assert(!allocations);
}
static struct fixture *find(const char *name)
{
    struct vfs_path path={0};assert(!vfs_path_lookup(&fixture_ns,&fixture_root,&fixture_root,name,0,&path));
    struct fixture *f=inode_entry(vfs_path_node(&path));vfs_path_put(&path);return f;
}
struct fixture_actor {const struct task *task;bool real_ids;};
static int fixture_search(void *context,const struct vfs_path *directory)
{struct fixture_actor *a=context;return fs_permissions_check_path(a->task,directory,FS_ACCESS_EXEC,a->real_ids);}
static int file_resolve(const struct task *task,const char *base,const char *input,bool real_ids,
                        unsigned flags,struct vfs_lookup_result *out)
{
    struct vfs_path start={0};int rc=vfs_path_lookup(&fixture_ns,&fixture_root,&fixture_root,base,VFS_LOOKUP_FOLLOW,&start);
    if(rc)return rc;
    const struct vfs_path *root=&fixture_root;
    if(task && sched_task_paths(task)->root.mount)root=&sched_task_paths(task)->root;
    struct fixture_actor actor={task,real_ids};struct vfs_lookup_policy policy={flags,fixture_search,&actor};
    rc=vfs_path_resolve(&fixture_ns,root,&start,input,&policy,out);vfs_path_put(&start);return rc;
}
static int file_check(const struct task *task,const char *name,uint32_t access,bool real_ids)
{
    struct vfs_lookup_result found={0};int rc=file_resolve(task,"/",name,real_ids,VFS_LOOKUP_FOLLOW,&found);
    if(!rc)rc=fs_permissions_check_path(task,&found.path,access,real_ids);vfs_path_put(&found.path);return rc;
}
static int file_chmod(const struct task *task,const char *name,unsigned flags,uint32_t mode)
{
    struct vfs_lookup_result found={0};int rc=file_resolve(task,"/",name,false,flags,&found);
    if(!rc)rc=fs_permissions_chmod_path(task,&found.path,mode);vfs_path_put(&found.path);return rc;
}
static int file_chown(const struct task *task,const char *name,unsigned flags,uint32_t uid,uint32_t gid)
{
    struct vfs_lookup_result found={0};int rc=file_resolve(task,"/",name,false,flags,&found);
    if(!rc)rc=fs_permissions_chown_path(task,&found.path,uid,gid);vfs_path_put(&found.path);return rc;
}
/* Formatted text is asserted only after actual resolution; it is never fed
 * back into an inode/provider operation or used to enforce a task root. */
static int resolved_path(const struct task *task,const char *base,const char *input,char *out,uint32_t capacity,
                          bool real_ids,unsigned flags)
{
    struct vfs_lookup_result found={0};int rc=file_resolve(task,base,input,real_ids,flags|VFS_LOOKUP_MISSING,&found);
    if(!rc)rc=vfs_path_format(&fixture_ns,&fixture_root,&found.path,out,capacity);
    if(!rc && ((flags&VFS_LOOKUP_PARENT) || found.missing)) {
        size_t length=strlen(out),last=strlen(found.last);bool slash=length>1;
        if(length+slash+last+found.trailing+1>capacity)rc=-ENAMETOOLONG;
        else {if(slash)out[length++]='/';memcpy(out+length,found.last,last);length+=last;
            if(found.trailing)out[length++]='/';out[length]=0;}
    }
    vfs_path_put(&found.path);return rc;
}
static int resolved_follow_path(const struct task *task,const char *base,const char *input,
                                char *out,uint32_t capacity,bool real_ids)
{return resolved_path(task,base,input,out,capacity,real_ids,VFS_LOOKUP_FOLLOW);}
static int mutation_parent(struct task *task,const char *name,bool deleting)
{
    struct vfs_lookup_result parent={0};struct vfs_path member={0};
    int rc=file_resolve(task,"/",name,false,VFS_LOOKUP_PARENT,&parent);
    if(!rc && deleting)rc=vfs_path_lookup_child(&parent.path,parent.last,&member);
    if(!rc)rc=syscall_mutation_parent(task,&parent.path,deleting ? &member : NULL);
    vfs_path_put(&member);vfs_path_put(&parent.path);return rc;
}
static int create_file(struct task *task,const char *name,uint32_t type,uint32_t mode,const char *target)
{
    struct vfs_lookup_result parent={0};int rc=file_resolve(task,"/",name,false,VFS_LOOKUP_PARENT,&parent);
    struct reliefos_permissions value;if(!rc)rc=syscall_creation_mode(task,&parent.path,type,mode,&value);
    if(!rc)rc=vfs_path_create_node(&parent.path,parent.last,type,&value,target,NULL);
    vfs_path_put(&parent.path);return rc;
}
