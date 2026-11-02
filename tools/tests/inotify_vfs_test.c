/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/fsnotify.h>
#include <reliefnt/fsnotify_vfs.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/time.h>
#include "../../fs/vfs/mount_internal.h"

/* Host physical-memory/time boundaries; namespace and filesystem behavior use
 * production VFS and tmpfs, including genuine hardlink and inode refcounts. */
static struct { uint64_t address; unsigned refs; } pages[32];
static bool fail_allocations;
void *kernel_malloc(size_t size) { return fail_allocations ? NULL : calloc(1,size); }
void kernel_free(void *p) { free(p); }
void *mm_phys_to_virt(uint64_t p) { return (void *)(uintptr_t)p; }
uint64_t mm_alloc_page(void)
{
    for(size_t n=0;n<32;++n)if(!pages[n].refs) {
        void *p=aligned_alloc(4096,4096);assert(p);
        pages[n].address=(uintptr_t)p;pages[n].refs=1;return pages[n].address;
    }
    return 0;
}
void mm_retain_page(uint64_t p)
{
    for(size_t n=0;n<32;++n)if(pages[n].refs && pages[n].address==p){++pages[n].refs;return;}
    abort();
}
void mm_free_page(uint64_t p)
{
    for(size_t n=0;n<32;++n)if(pages[n].refs && pages[n].address==p) {
        if(!--pages[n].refs)free((void *)(uintptr_t)p);return;
    }
    abort();
}
uint64_t mm_total_memory_kib(void) { return 65536; }
int time_wall_clock(struct reliefos_time_info *out)
{ *out=(struct reliefos_time_info){.unix_seconds=1234};return 0; }
void sched_truncate_file_mappings(const struct storage_node *node,uint64_t length)
{ (void)node;(void)length; }
void page_cache_invalidate_node(const struct storage_node *node) { (void)node; }
void page_cache_forget_node(const struct storage_node *node) { (void)node; }
int page_cache_has_inode(const struct storage_node *node) { (void)node;return 0; }
void page_cache_forget_volume(uint32_t volume) { (void)volume; }
int storage_inode_cache_node(struct storage_node *node);
int storage_inode_purge_volume(uint32_t volume);

static int lookup(void *ctx,const struct storage_node *parent,const char *name,struct storage_node *out)
{ return tmpfs_lookup_child(ctx,parent->first_cluster,name,out); }
static int retain(void *ctx,const struct storage_node *node,void **pin)
{
    int rc=tmpfs_hold(ctx,node->first_cluster);
    if(!rc)*pin=(void *)(uintptr_t)node->first_cluster;
    return rc;
}
static void release(void *ctx,const struct storage_node *node,void *pin)
{ assert((uintptr_t)pin==node->first_cluster);tmpfs_drop(ctx,node->first_cluster); }
static bool same_node(void *ctx,const struct storage_node *a,const struct storage_node *b)
{ (void)ctx;return a->first_cluster==b->first_cluster; }
static int permissions(void *ctx,const struct vfs_path *path,struct reliefos_permissions *p,bool write)
{ return tmpfs_permissions(ctx,vfs_path_node(path)->first_cluster,p,write); }
static unsigned getattr_errors;
static int getattr(void *ctx,const struct vfs_path *path,struct linux_stat_abi *out)
{
    if(getattr_errors){--getattr_errors;return -EIO;}
    return tmpfs_stat(ctx,vfs_path_node(path)->first_cluster,out);
}
static int create(void *ctx,const struct storage_node *parent,const char *name,uint32_t type,
                  const struct reliefos_permissions *p,const char *target,struct storage_node *out,void **pin)
{
    int rc=tmpfs_create_child(ctx,parent->first_cluster,name,type|(p->mode&07777),target,p,out);
    if(!rc && pin)rc=retain(ctx,out,pin);
    return rc;
}
static int remove_node(void *ctx,const struct vfs_path *path,bool directory)
{ return tmpfs_unlink_child(ctx,path->dentry->parent->node.first_cluster,path->dentry->name,directory); }
static bool rename_error_after_commit,write_error_after_commit;
static int rename_node(void *ctx,const struct vfs_path *source,const struct vfs_path *parent,
                       const char *name,const struct vfs_path *target,bool *committed)
{
    (void)target;
    int rc=tmpfs_rename_child(ctx,source->dentry->parent->node.first_cluster,source->dentry->name,
                              parent->dentry->node.first_cluster,name);
    *committed=!rc;return !rc && rename_error_after_commit ? -EIO : rc;
}
static int link_node(void *ctx,const struct storage_node *source,const struct storage_node *parent,const char *name)
{ return tmpfs_link_child(ctx,source->first_cluster,parent->first_cluster,name); }
static int write_node(void *ctx,struct storage_node *node,uint64_t off,const void *data,uint32_t len,uint32_t *done)
{
    int rc=tmpfs_write(ctx,node->first_cluster,off,data,len,done);
    return !rc && write_error_after_commit ? -EIO : rc;
}
static int truncate_node(void *ctx,struct storage_node *node,uint64_t size)
{ return tmpfs_truncate(ctx,node->first_cluster,size); }
static int fallocate_node(void *ctx,struct storage_node *node,uint32_t mode,uint64_t off,uint64_t len)
{ return tmpfs_fallocate(ctx,node->first_cluster,mode,off,len); }
static int utimens_node(void *ctx,const struct storage_node *node,int64_t a,int64_t m,bool sa,bool sm)
{ return tmpfs_utimens(ctx,node->first_cluster,a,m,sa,sm); }
static unsigned finish_failures;
static int finish(void *ctx)
{
    (void)ctx;
    if(finish_failures){--finish_failures;return -EIO;}
    return 0;
}
static const struct vfs_backend_ops ops={.lookup=lookup,.retain=retain,.release=release,
    .same_node=same_node,.finish=finish,.permissions=permissions,.getattr=getattr,.create_node=create,
    .remove_node=remove_node,.rename_node=rename_node,.link_node=link_node,
    .write=write_node,.truncate=truncate_node,.fallocate=fallocate_node,.utimens=utimens_node};

static struct fsnotify_instance *new_watch(const struct vfs_path *path,uint32_t mask,int *wd)
{
    struct fsnotify_instance *instance=NULL;struct fsnotify_key key;
    assert(vfs_notify_key && vfs_notify_event);
    assert(!fsnotify_create(1000,NULL,NULL,&instance));
    assert(!vfs_notify_key(path,&key));
    *wd=fsnotify_add_watch(instance,&key,mask);assert(*wd>0);return instance;
}
static uint32_t take(struct fsnotify_instance *instance,int wd,uint32_t mask,const char *name)
{
    const struct inotify_event *event=NULL;
    size_t size=fsnotify_peek(instance,&event);assert(size && event);
    assert(event->wd==wd && event->mask==mask);
    if(name){assert(event->len==16 && !strcmp(event->name,name));}
    else assert(event->len==0);
    uint32_t cookie=event->cookie;fsnotify_pop(instance);return cookie;
}
static void empty_at(struct fsnotify_instance *instance,int line)
{
    if(fsnotify_ready(instance)) {
        const struct inotify_event *event=NULL;fsnotify_peek(instance,&event);
        fprintf(stderr,"unexpected event at line %d: wd=%d mask=%08x\n",line,event->wd,event->mask);
        abort();
    }
}
#define empty(instance) empty_at(instance,__LINE__)
static struct vfs_mount *existing_mount(struct tmpfs_super *fs)
{
    struct storage_node root;struct vfs_mount *mount=NULL;
    assert(!tmpfs_lookup(fs,"/",&root));
    assert(!vfs_mount_new(&ops,fs,NULL,&root,&mount));return mount;
}
static struct vfs_mount *new_mount(struct tmpfs_super **fs)
{
    assert(!tmpfs_new(7,0,"size=0,nr_inodes=0",0,0,fs));
    return existing_mount(*fs);
}
static void watched_cluster_cache(void)
{
    for(uint32_t n=0;n<128;++n) {
        struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
                                  .volume_id=123,.entry_id=n+1,.first_cluster=n+1};
        assert(!storage_inode_cache_node(&node));
    }
    struct storage_node watched={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
                                  .volume_id=123,.entry_id=1000,.first_cluster=1000};
    assert(!storage_inode_cache_node(&watched));
    struct fsnotify_instance *instance=NULL;struct fsnotify_key key={.ops=&ops,.node=watched};
    assert(!fsnotify_create(1000,NULL,NULL,&instance));
    assert(fsnotify_add_watch(instance,&key,IN_MODIFY)>0);
    struct storage_node other={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,
                                .volume_id=123,.entry_id=1001,.first_cluster=1001};
    assert(!storage_inode_cache_node(&other));
    struct storage_node refreshed=watched;refreshed.inode_generation=0;
    assert(!storage_inode_cache_node(&refreshed));
    assert(refreshed.inode_generation==watched.inode_generation);
    fsnotify_put(instance);assert(!storage_inode_purge_volume(123));
}
int main(void)
{
    watched_cluster_cache();
    struct tmpfs_super *fs=NULL;struct vfs_mount *mount=new_mount(&fs);
    struct vfs_namespace ns={0};assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0};assert(!vfs_namespace_root(&ns,&root));
    int dwd,fwd;struct fsnotify_instance *dirs=new_watch(&root,IN_ALL_EVENTS|IN_EXCL_UNLINK,&dwd);
    struct reliefos_permissions mode={0644,0,0};struct vfs_path alpha={0};
    assert(!vfs_path_create_node(&root,"alpha",LINUX_S_IFREG,&mode,NULL,&alpha));
    take(dirs,dwd,IN_CREATE,"alpha");empty(dirs);
    struct fsnotify_instance *files=new_watch(&alpha,IN_ALL_EVENTS,&fwd);
    uint32_t written=0;assert(!vfs_path_write(&alpha,0,"x",1,&written) && written==1);
    take(dirs,dwd,IN_MODIFY,"alpha");take(files,fwd,IN_MODIFY,NULL);
    fail_allocations=true;
    assert(!vfs_path_write(&alpha,0,"q",1,&written) && written==1);
    fail_allocations=false;
    take(dirs,-1,IN_Q_OVERFLOW,NULL);take(files,-1,IN_Q_OVERFLOW,NULL);
    write_error_after_commit=true;
    assert(vfs_path_write(&alpha,1,"y",1,&written)==-EIO && written==1);
    write_error_after_commit=false;
    take(dirs,dwd,IN_MODIFY,"alpha");take(files,fwd,IN_MODIFY,NULL);
    assert(!vfs_path_truncate(&alpha,1));
    take(dirs,dwd,IN_MODIFY,"alpha");take(files,fwd,IN_MODIFY,NULL);
    assert(!vfs_path_fallocate(&alpha,0,0,8));
    take(dirs,dwd,IN_MODIFY,"alpha");take(files,fwd,IN_MODIFY,NULL);
    mode.mode=0600;assert(!vfs_path_permissions(&alpha,&mode,true));
    take(dirs,dwd,IN_ATTRIB,"alpha");take(files,fwd,IN_ATTRIB,NULL);
    assert(!vfs_path_utimens(&alpha,100,200,true,true));
    take(dirs,dwd,IN_ATTRIB,"alpha");take(files,fwd,IN_ATTRIB,NULL);
    assert(!vfs_path_utimens(&alpha,100,200,true,false));
    take(dirs,dwd,IN_ACCESS,"alpha");take(files,fwd,IN_ACCESS,NULL);
    assert(!vfs_path_utimens(&alpha,100,200,false,true));
    take(dirs,dwd,IN_MODIFY,"alpha");take(files,fwd,IN_MODIFY,NULL);
    assert(!vfs_path_link(&ns,&alpha,&root,"beta"));
    take(dirs,dwd,IN_CREATE,"beta");take(files,fwd,IN_ATTRIB,NULL);
    struct vfs_path beta={0};assert(!vfs_path_lookup_child(&root,"beta",&beta));
    assert(!vfs_path_remove(&ns,&alpha,false));
    take(dirs,dwd,IN_DELETE,"alpha");take(files,fwd,IN_ATTRIB,NULL);
    assert(!vfs_path_write(&alpha,0,"z",1,&written));
    take(files,fwd,IN_MODIFY,NULL);empty(dirs);
    vfs_notify_event(&alpha,IN_CLOSE_WRITE);take(files,fwd,IN_CLOSE_WRITE,NULL);empty(dirs);
    empty(files);
    assert(!vfs_path_rename(&ns,&beta,&root,"gamma"));
    uint32_t cookie=take(dirs,dwd,IN_MOVED_FROM,"beta");assert(cookie);
    assert(take(dirs,dwd,IN_MOVED_TO,"gamma")==cookie);
    take(files,fwd,IN_MOVE_SELF,NULL);
    assert(!vfs_path_remove(&ns,&beta,false));
    take(dirs,dwd,IN_DELETE,"gamma");take(files,fwd,IN_ATTRIB,NULL);
    vfs_notify_event(&beta,IN_CLOSE_WRITE);take(files,fwd,IN_CLOSE_WRITE,NULL);empty(dirs);
    vfs_path_put(&beta);empty(files);
    vfs_path_put(&alpha);
    take(files,fwd,IN_DELETE_SELF,NULL);take(files,fwd,IN_IGNORED,NULL);empty(files);
    fsnotify_put(files);
    struct vfs_path directory={0};mode.mode=0755;
    assert(!vfs_path_create_node(&root,"directory",LINUX_S_IFDIR,&mode,NULL,&directory));
    take(dirs,dwd,IN_CREATE|IN_ISDIR,"directory");
    struct fsnotify_instance *sub=new_watch(&directory,IN_ALL_EVENTS,&fwd);
    assert(!vfs_path_remove(&ns,&directory,true));take(dirs,dwd,IN_DELETE|IN_ISDIR,"directory");
    vfs_path_put(&directory);take(sub,fwd,IN_DELETE_SELF,NULL);take(sub,fwd,IN_IGNORED,NULL);
    fsnotify_put(sub);
    struct vfs_path src={0},dst={0},alias={0};mode.mode=0644;
    assert(!vfs_path_create_node(&root,"source",LINUX_S_IFREG,&mode,NULL,&src));
    assert(!vfs_path_create_node(&root,"victim",LINUX_S_IFREG,&mode,NULL,&dst));
    take(dirs,dwd,IN_CREATE,"source");take(dirs,dwd,IN_CREATE,"victim");
    struct fsnotify_instance *victim=new_watch(&dst,IN_ALL_EVENTS,&fwd);
    assert(!vfs_path_link(&ns,&dst,&root,"alias"));
    take(victim,fwd,IN_ATTRIB,NULL);take(dirs,dwd,IN_CREATE,"alias");
    assert(!vfs_path_lookup_child(&root,"alias",&alias));
    assert(!vfs_path_rename(&ns,&dst,&root,"alias"));
    empty(dirs);empty(victim);
    rename_error_after_commit=true;
    assert(vfs_path_rename(&ns,&src,&root,"victim")==-EIO);
    rename_error_after_commit=false;
    cookie=take(dirs,dwd,IN_MOVED_FROM,"source");assert(cookie);
    assert(take(dirs,dwd,IN_MOVED_TO,"victim")==cookie);
    take(victim,fwd,IN_ATTRIB,NULL);
    assert(!vfs_path_remove(&ns,&alias,false));
    take(dirs,dwd,IN_DELETE,"alias");take(victim,fwd,IN_ATTRIB,NULL);
    vfs_path_put(&alias);empty(victim);
    vfs_path_put(&dst);take(victim,fwd,IN_DELETE_SELF,NULL);take(victim,fwd,IN_IGNORED,NULL);
    fsnotify_put(victim);
    assert(!vfs_path_remove(&ns,&src,false));take(dirs,dwd,IN_DELETE,"victim");vfs_path_put(&src);

    /* A failed pre-unlink stat must not invalidate a still-linked inode when
     * its last cached alias is dropped and a fresh stat can resolve nlink. */
    struct vfs_path keep={0},keep2={0};mode.mode=0644;
    assert(!vfs_path_create_node(&root,"keep",LINUX_S_IFREG,&mode,NULL,&keep));
    take(dirs,dwd,IN_CREATE,"keep");
    struct fsnotify_instance *keep_watch=new_watch(&keep,IN_ALL_EVENTS,&fwd);
    assert(!vfs_path_link(&ns,&keep,&root,"keep2"));
    take(keep_watch,fwd,IN_ATTRIB,NULL);take(dirs,dwd,IN_CREATE,"keep2");
    assert(!vfs_path_lookup_child(&root,"keep2",&keep2));
    getattr_errors=1;
    assert(!vfs_path_remove(&ns,&keep,false));
    take(keep_watch,fwd,IN_ATTRIB,NULL);take(dirs,dwd,IN_DELETE,"keep");
    vfs_path_put(&keep);vfs_path_put(&keep2);empty(keep_watch);
    assert(!vfs_path_lookup_child(&root,"keep2",&keep2));
    assert(!vfs_path_write(&keep2,0,"x",1,&written));
    take(keep_watch,fwd,IN_MODIFY,NULL);take(dirs,dwd,IN_MODIFY,"keep2");
    assert(!vfs_path_remove(&ns,&keep2,false));
    take(keep_watch,fwd,IN_ATTRIB,NULL);take(dirs,dwd,IN_DELETE,"keep2");
    vfs_path_put(&keep2);take(keep_watch,fwd,IN_DELETE_SELF,NULL);take(keep_watch,fwd,IN_IGNORED,NULL);
    fsnotify_put(keep_watch);

    struct vfs_path point={0};mode.mode=0755;
    assert(!vfs_path_create_node(&root,"mnt",LINUX_S_IFDIR,&mode,NULL,&point));
    take(dirs,dwd,IN_CREATE|IN_ISDIR,"mnt");
    struct tmpfs_super *childfs=NULL;struct vfs_mount *child=new_mount(&childfs);
    assert(!vfs_mount_attach(&ns,child,&point));
    struct vfs_path child_value={child,child->root},child_root={0};
    vfs_path_get(&child_value,&child_root);vfs_mount_put(child);
    struct fsnotify_instance *mount_watch=new_watch(&child_root,IN_ALL_EVENTS,&fwd);
    assert(!vfs_mount_unmount(&ns,&child_root));empty(mount_watch);
    vfs_path_put(&child_root);
    take(mount_watch,fwd,IN_UNMOUNT,NULL);take(mount_watch,fwd,IN_IGNORED,NULL);
    fsnotify_put(mount_watch);tmpfs_destroy(childfs);
    assert(!vfs_path_remove(&ns,&point,true));take(dirs,dwd,IN_DELETE|IN_ISDIR,"mnt");vfs_path_put(&point);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    take(dirs,dwd,IN_UNMOUNT,NULL);take(dirs,dwd,IN_IGNORED,NULL);empty(dirs);
    fsnotify_put(dirs);tmpfs_destroy(fs);

    /* Multiple mount objects may share a backend domain. Its watches survive
     * the first retirement and a deferred, failed final provider finish. */
    struct tmpfs_super *shared=NULL;struct vfs_mount *first=new_mount(&shared);
    struct vfs_mount *second=existing_mount(shared);
    struct vfs_path shared_value={first,first->root},shared_root={0};
    vfs_path_get(&shared_value,&shared_root);
    struct fsnotify_instance *shared_watch=new_watch(&shared_root,IN_ALL_EVENTS,&fwd);
    vfs_path_put(&shared_root);vfs_mount_put(first);empty(shared_watch);
    finish_failures=1;vfs_mount_put(second);empty(shared_watch);
    assert(vfs_reclaim_mounts(1)==1);
    take(shared_watch,fwd,IN_UNMOUNT,NULL);take(shared_watch,fwd,IN_IGNORED,NULL);
    fsnotify_put(shared_watch);tmpfs_destroy(shared);

    /* A backend without getattr uses VFS's default nlink=1. Once a known
     * last link is removed, that default cannot resurrect the removed watch. */
    struct tmpfs_super *basicfs=NULL;struct vfs_mount *basic=NULL;
    struct vfs_backend_ops basic_ops=ops;basic_ops.getattr=NULL;
    struct storage_node basic_node;struct vfs_namespace basic_ns={0};
    assert(!tmpfs_new(9,0,"size=0,nr_inodes=0",0,0,&basicfs));
    assert(!tmpfs_lookup(basicfs,"/",&basic_node));
    assert(!vfs_mount_new(&basic_ops,basicfs,NULL,&basic_node,&basic));
    assert(!vfs_namespace_init(&basic_ns,basic));vfs_mount_put(basic);
    struct vfs_path basic_root={0},basic_file={0};
    assert(!vfs_namespace_root(&basic_ns,&basic_root));mode.mode=0644;
    assert(!vfs_path_create_node(&basic_root,"file",LINUX_S_IFREG,&mode,NULL,&basic_file));
    struct fsnotify_instance *basic_watch=new_watch(&basic_file,IN_ALL_EVENTS,&fwd);
    assert(!vfs_path_remove(&basic_ns,&basic_file,false));
    take(basic_watch,fwd,IN_ATTRIB,NULL);vfs_path_put(&basic_file);
    take(basic_watch,fwd,IN_DELETE_SELF,NULL);take(basic_watch,fwd,IN_IGNORED,NULL);
    fsnotify_put(basic_watch);vfs_path_put(&basic_root);assert(!vfs_namespace_destroy(&basic_ns));
    tmpfs_destroy(basicfs);
    for(size_t n=0;n<32;++n)assert(!pages[n].refs);
    puts("inotify VFS and tmpfs integration: all tests passed");return 0;
}
