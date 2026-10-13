/* Explicit-volume VFS adapters. Only the compatibility wrappers consult the
 * active-volume selector; the backend and this operation table never do. */
#include "../drivers/bootstrap/storage/storage_internal.h"

static uint32_t ev_type(uint16_t mode)
{
    switch (mode&0170000) { case 0040000:return RELIEFOS_FS_TYPE_DIR;
        case 0120000:return RELIEFOS_FS_TYPE_SYMLINK; case 0010000:return RELIEFOS_FS_TYPE_FIFO;
        case 0140000:return RELIEFOS_FS_TYPE_SOCKET; default:return RELIEFOS_FS_TYPE_FILE; }
}
static void ev_node(struct storage_volume *v,uint64_t ino,const struct ext4_inode_view *in,struct storage_node *out)
{
    if (out) *out=(struct storage_node){.type=ev_type(in->mode),.flags=STORAGE_NODE_FLAG_EXT_FAMILY|
        (ino==2?STORAGE_NODE_FLAG_ROOT:0),.first_cluster=ino,.volume_id=v->volume_id,.size=in->size,
        .mount_generation=v->mount_generation,.inode_generation=in->generation};
}
int storage_ext4_check_node(struct storage_volume *v,const struct storage_node *node,struct ext4_inode_view *in)
{
    if (!v || !node || node->volume_id!=v->volume_id) return -RELIEFOS_EINVAL;
    if (node->mount_generation && node->mount_generation!=v->mount_generation) return -116;
    int ret=storage_ext4_read_inode(v,node->first_cluster,in); if (ret<0) return ret==-2?-116:ret;
    if (node->inode_generation && node->inode_generation!=in->generation) return -116;
    return 0;
}
static int ev_lookup(struct storage_volume *v,const char *path,struct storage_node *out)
{
    if (!v || !path || path[0]!='/') return -RELIEFOS_EINVAL;
    uint64_t ino=2; struct ext4_inode_view in; unsigned pos=1; char name[256];
    while (path[pos]) {
        while (path[pos]=='/') pos++;
        if (!path[pos]) break;
        unsigned len=0;
        while (path[pos] && path[pos]!='/') { if (len==255) return -36; name[len++]=path[pos++]; }
        name[len]=0; int ret=storage_ext4_dir_lookup(v,ino,name,&ino,NULL); if (ret<0) return ret;
    }
    int ret=storage_ext4_read_inode(v,ino,&in); if (!ret) ev_node(v,ino,&in,out); return ret;
}
static int ev_parent(struct storage_volume *v,const char *path,uint64_t *ino,char name[256])
{
    char parent[RELIEFOS_FS_PATH_LEN];
    int ret=storage_parent_path(path,parent,sizeof(parent),name,256); if (ret<0) return ret;
    struct storage_node n; ret=ev_lookup(v,parent,&n);
    if (!ret && n.type!=RELIEFOS_FS_TYPE_DIR) return -20;
    if (!ret) *ino=n.first_cluster; return ret;
}
static int ev_read(struct storage_volume *v,const struct storage_node *node,uint64_t offset,void *data,uint32_t len,uint32_t *done)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    return storage_ext4_read_file_range(v,node->first_cluster,&in,offset,data,len,done);
}
static int ev_drop_privileges(struct storage_volume *v,uint64_t ino,struct ext4_inode_view *in)
{
    struct task *task=sched_current_task();
    if (!task || (task->cap_effective&(1ULL<<CAP_FSETID)) || (in->mode&0170000)!=0100000) return 0;
    uint16_t remove=04000; if ((in->mode&0010) || !task_in_group(task,in->gid,false)) remove|=02000;
    if (!(in->mode&remove)) return 0;
    struct storage_ext4_handle h; int ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    in->mode&=~remove; ret=storage_ext4_write_inode(v,ino,in); return ed_stop(&h,ret);
}
static int ev_write(struct storage_volume *v,const struct storage_node *node,uint64_t offset,const void *data,uint32_t len,uint32_t *done)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    if ((in.mode&0170000)!=0100000) return -21;
    ret=ev_drop_privileges(v,node->first_cluster,&in); if (ret<0) return ret;
    ret=storage_ext4_write_file_range(v,node->first_cluster,offset,data,len,done);
    storage_cache_invalidate(); return ret;
}
static int ev_truncate(struct storage_volume *v,const struct storage_node *node,uint64_t size)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    if ((in.mode&0170000)!=0100000) return -21;
    ret=ev_drop_privileges(v,node->first_cluster,&in); if (ret<0) return ret;
    ret=storage_ext4_truncate(v,node->first_cluster,size); storage_cache_invalidate(); return ret;
}
static int ev_readdir(struct storage_volume *v,const struct storage_node *node,uint64_t *cursor,struct reliefos_dir_entry *out)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    struct storage_ext4_dirent entry; ret=storage_ext4_dir_iterate(v,node->first_cluster,cursor,&entry);
    if (ret) return ret==-RELIEFOS_ENOENT?0:ret;
    ret=storage_ext4_read_inode(v,entry.ino,&in); if (ret<0) return ret;
    storage_memzero(out,sizeof(*out)); out->type=ev_type(in.mode);
    storage_copy_text(out->name,sizeof(out->name),entry.name); return 1;
}
static int ev_create(struct storage_volume *v,const char *path,uint16_t mode,uint32_t uid,uint32_t gid,struct storage_node *out)
{
    uint64_t parent,ino; char name[256]; int ret=ev_parent(v,path,&parent,name); if (ret<0) return ret;
    ret=storage_ext4_create(v,parent,name,mode,uid,gid,NULL,&ino);
    storage_cache_invalidate(); return ret<0?ret:ev_lookup(v,path,out);
}
static int ev_mkdir(struct storage_volume *v,const char *path)
{ return ev_create(v,path,0040755,0,0,NULL); }
static int ev_link(struct storage_volume *v,const char *oldpath,const char *newpath)
{
    struct storage_node source; int ret=ev_lookup(v,oldpath,&source); if (ret<0) return ret;
    uint64_t parent; char name[256]; ret=ev_parent(v,newpath,&parent,name); if (ret<0) return ret;
    ret=storage_ext4_link(v,parent,name,source.first_cluster); storage_cache_invalidate(); return ret;
}
static int ev_unlink_kind(struct storage_volume *v,const char *path,bool dir)
{
    uint64_t parent; char name[256]; int ret=ev_parent(v,path,&parent,name); if (ret<0) return ret;
    struct storage_node node; ret=ev_lookup(v,path,&node); if (ret<0) return ret;
    struct storage_inode_ref *ref=storage_inode_find(v->volume_id,node.first_cluster);
    bool held=ref && ref->references;
    ret=storage_ext4_unlink_held(v,parent,name,dir,held);
    if (!ret && held) ref->unlinked=true;
    if (!ret && !held) page_cache_invalidate_node(&node);
    storage_cache_invalidate(); return ret;
}
static int ev_unlink(struct storage_volume *v,const char *path) { return ev_unlink_kind(v,path,false); }
static int ev_rmdir(struct storage_volume *v,const char *path) { return ev_unlink_kind(v,path,true); }
static int ev_rename(struct storage_volume *v,const char *oldpath,const char *newpath)
{
    uint64_t op,np; char on[256],nn[256]; int ret=ev_parent(v,oldpath,&op,on); if (ret<0) return ret;
    ret=ev_parent(v,newpath,&np,nn); if (ret<0) return ret;
    struct storage_node replaced; ret=ev_lookup(v,newpath,&replaced); bool exists=!ret;
    if (ret<0 && ret!=-2) return ret;
    struct storage_inode_ref *ref=exists?storage_inode_find(v->volume_id,replaced.first_cluster):NULL;
    bool held=ref && ref->references;
    ret=storage_ext4_rename_held(v,op,on,np,nn,held);
    if (!ret && held) ref->unlinked=true;
    if (!ret && exists && !held) page_cache_invalidate_node(&replaced);
    storage_cache_invalidate(); return ret;
}
static int ev_symlink(struct storage_volume *v,const char *target,const char *path)
{
    if (!target || !target[0]) return -RELIEFOS_ENOENT;
    if (storage_strlen(target)>=v->ext4.block_size) return -36;
    uint64_t parent,ino; char name[256]; int ret=ev_parent(v,path,&parent,name); if (ret<0) return ret;
    ret=storage_ext4_create(v,parent,name,0120777,0,0,target,&ino); storage_cache_invalidate(); return ret;
}
static int ev_readlink(struct storage_volume *v,const struct storage_node *node,char *out,uint32_t capacity,uint32_t *done)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    if ((in.mode&0170000)!=0120000) return -RELIEFOS_EINVAL;
    return storage_ext4_read_file_range(v,node->first_cluster,&in,0,out,capacity,done);
}
static int ev_fsync(struct storage_volume *v)
{
    int ret=storage_ext4_journal_commit(v,true);
    if (!ret) ret=storage_ext4_journal_checkpoint(v);
    if (!ret) ret=storage_ext4_cache_flush(v);
    if (!ret) ret=storage_ext4_device_flush(v); return ret;
}
static int ev_statfs(struct storage_volume *v,struct linux_statfs_abi *out)
{
    const struct storage_ext4_super_view *s=&v->ext4.super_view;
    *out=(struct linux_statfs_abi){.f_type=0xef53,.f_bsize=s->block_size,.f_frsize=s->block_size,
        .f_blocks=s->blocks_count,.f_bfree=s->free_blocks_count,
        .f_bavail=s->free_blocks_count>s->reserved_blocks_count?s->free_blocks_count-s->reserved_blocks_count:0,
        .f_files=s->inodes_count,.f_ffree=s->free_inodes_count,.f_namelen=255,.f_flags=LINUX_ST_VALID};
    out->f_fsid[0]=ext4_get_le32(s->uuid)^ext4_get_le32(s->uuid+8);
    out->f_fsid[1]=ext4_get_le32(s->uuid+4)^ext4_get_le32(s->uuid+12);
    uint8_t *raw; int ret=storage_ext4_cache_get(v,1024/s->block_size,&raw,false); if (ret<0) return ret;
    uint64_t overhead=ext4_get_le32(raw+1024%s->block_size+0x248);
    if (!overhead) {
        overhead=s->first_data_block;
        for (uint64_t group=0;group<s->group_count;group++)
            overhead+=ext4_base_meta_blocks(v,group)+2+((uint64_t)s->inodes_per_group*s->inode_size+s->block_size-1)/s->block_size;
        if (s->journal_inum) { struct ext4_inode_view in; ret=storage_ext4_read_inode(v,s->journal_inum,&in);
            if (ret<0) return ret; overhead+=in.blocks/(s->block_size/512); }
    }
    if (overhead>s->blocks_count) return -RELIEFOS_EIO;
    out->f_blocks-=overhead;
    if (v->read_only_reason || v->ext4.fs_error) out->f_flags|=LINUX_ST_RDONLY;
    return 0;
}
int storage_ext4_replace(struct storage_volume *v,const char *path,const void *data,uint32_t len)
{
    struct storage_node node; int ret=ev_lookup(v,path,&node);
    if (ret==-2) ret=ev_create(v,path,0100644,0,0,&node);
    else if (!ret) ret=ev_truncate(v,&node,0);
    if (ret<0) return ret;
    uint32_t done; return len?ev_write(v,&node,0,data,len,&done):0;
}
int storage_ext4_mark_special(struct storage_volume *v,const char *path,const struct storage_node *node,uint32_t mode)
{
    struct ext4_inode_view in; int ret=storage_ext4_check_node(v,node,&in); if (ret<0) return ret;
    if (in.size || in.blocks) return -RELIEFOS_EINVAL;
    uint64_t parent; char name[256]; ret=ev_parent(v,path,&parent,name); if (ret<0) return ret;
    struct storage_ext4_handle h; ret=storage_ext4_journal_start(v,EXT4_JOURNAL_CREDITS,&h); if (ret<0) return ret;
    in.mode=(in.mode&07777)|mode; in.flags&=~EXT4_EXTENTS_FL; storage_memzero(in.i_block_raw,60);
    ret=storage_ext4_write_inode(v,node->first_cluster,&in);
    if (!ret) ret=storage_ext4_dir_remove(v,parent,name,NULL);
    if (!ret) ret=storage_ext4_dir_insert(v,parent,name,node->first_cluster,ed_type(in.mode));
    ret=ed_stop(&h,ret); storage_cache_invalidate(); return ret;
}
const struct storage_ext4_operations storage_ext4_ops={
    .lookup=ev_lookup,.read=ev_read,.write=ev_write,.readdir=ev_readdir,.create=ev_create,
    .mkdir=ev_mkdir,.link=ev_link,.unlink=ev_unlink,.rmdir=ev_rmdir,.rename=ev_rename,
    .truncate=ev_truncate,.symlink=ev_symlink,.readlink=ev_readlink,.fsync=ev_fsync,
    .fallocate=storage_ext4_fallocate,.fiemap=storage_ext4_fiemap,.getxattr=storage_ext4_getxattr,
    .setxattr=storage_ext4_setxattr,.statfs=ev_statfs
};

int storage_fallocate_node(struct storage_node *node,uint32_t mode,uint64_t offset,uint64_t len)
{
    if (!node || !(node->flags&STORAGE_NODE_FLAG_EXT_FAMILY)) return -RELIEFOS_EOPNOTSUPP;
    uint64_t flags; struct storage_volume *previous=NULL; struct ext4_inode_view in;
    kernel_execution_lock_irqsave(&flags); int ret=storage_select_node_volume(node,&previous);
    if (!ret) ret=storage_ext4_check_node(&g_volumes[node->volume_id],node,&in);
    if (!ret && (in.mode&0170000)!=0100000) ret=-RELIEFOS_EINVAL;
    if (!ret) ret=storage_ext4_fallocate(&g_volumes[node->volume_id],node->first_cluster,mode,offset,len);
    if (!ret) {
        page_cache_invalidate_node(node); storage_cache_invalidate();
        ret=storage_ext4_read_inode(&g_volumes[node->volume_id],node->first_cluster,&in);
        if (!ret) node->size=in.size;
    }
    storage_restore_volume(previous); kernel_execution_unlock_irqrestore(flags); return ret;
}
int storage_fiemap_node(const struct storage_node *node,uint64_t start,uint64_t len,
                        struct storage_ext4_fiemap_extent *out,uint32_t cap,uint32_t *count)
{
    if (!node || !(node->flags&STORAGE_NODE_FLAG_EXT_FAMILY)) return -RELIEFOS_EOPNOTSUPP;
    uint64_t flags; struct storage_volume *previous=NULL; struct ext4_inode_view in;
    kernel_execution_lock_irqsave(&flags); int ret=storage_select_node_volume(node,&previous);
    if (!ret) ret=storage_ext4_check_node(&g_volumes[node->volume_id],node,&in);
    if (!ret && (in.mode&0170000)!=0100000) ret=-RELIEFOS_EINVAL;
    if (!ret) ret=storage_ext4_fiemap(&g_volumes[node->volume_id],node->first_cluster,start,len,out,cap,count);
    storage_restore_volume(previous); kernel_execution_unlock_irqrestore(flags); return ret;
}
