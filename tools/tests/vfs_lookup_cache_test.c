/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/mount.h>
#include <linux/errno.h>
#include <reliefnt/lookup_cache.h>

static size_t allocations;
static unsigned fail_after;
void *kernel_malloc(size_t bytes)
{
    if(fail_after && !--fail_after)return NULL;
    void *p=calloc(1,bytes);if(p)++allocations;return p;
}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}

struct backend {uint64_t generation,size;unsigned lookups,pins;int error;bool exists,enabled;};
static struct backend backend={.generation=1,.size=7,.exists=true,.enabled=true};
static unsigned retain_stale;
static struct storage_node node(unsigned id,uint32_t type)
{return (struct storage_node){.first_cluster=id,.type=type,.mount_generation=1,.size=backend.size};}
static int lookup(void *ctx,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    struct backend *b=ctx;++b->lookups;
    if(b->error)return b->error;
    if(!strcmp(name,"dir")){*out=node(2,RELIEFOS_FS_TYPE_DIR);return 0;}
    if(!strcmp(name,"file") && b->exists){*out=node(parent->first_cluster==1 ? 3 : 4,RELIEFOS_FS_TYPE_FILE);return 0;}
    return -ENOENT;
}
static int retain(void *ctx,const struct storage_node *n,void **pin)
{(void)n;if(retain_stale){--retain_stale;return -ESTALE;}struct backend *b=ctx;++b->pins;*pin=b;return 0;}
static void release(void *ctx,const struct storage_node *n,void *pin)
{(void)n;struct backend *b=ctx;assert(pin==b && b->pins);--b->pins;}
static bool same(void *ctx,const struct storage_node *a,const struct storage_node *b)
{(void)ctx;return a->first_cluster==b->first_cluster && a->mount_generation==b->mount_generation;}
static uint64_t generation(void *ctx)
{struct backend *b=ctx;return b->enabled ? b->generation : 0;}
static const struct vfs_backend_ops ops={.lookup=lookup,.retain=retain,.release=release,.same_node=same,
    .lookup_generation=generation,
};
static struct vfs_namespace ns;
static struct vfs_path root;
static void setup(void)
{
    struct storage_node n=node(1,RELIEFOS_FS_TYPE_DIR);struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&ops,&backend,NULL,&n,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns,&root));
}
static void teardown(void)
{
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    assert(!backend.pins && !allocations);
}
static void find(const struct vfs_path *parent,const char *name,unsigned id,uint64_t size)
{
    struct vfs_path path={0};assert(!vfs_path_lookup_child(parent,name,&path));
    assert(vfs_path_node(&path)->first_cluster==id && vfs_path_node(&path)->size==size);
    vfs_path_put(&path);
}
static void missing(const struct vfs_path *parent,const char *name,int error)
{struct vfs_path path={0};assert(vfs_path_lookup_child(parent,name,&path)==error && !path.mount);}
static int denied;
static int search(void *ctx,const struct vfs_path *path)
{(void)ctx;(void)path;return denied ? -EACCES : 0;}

int main(int argc,char **argv)
{
    assert(argc==2);setup();
    if(!strcmp(argv[1],"positive")) {
        for(unsigned i=0;i<64;++i)find(&root,"file",3,7);
        assert(backend.lookups==1); /* Previously one native lookup per call. */
        assert(backend.pins==1); /* Cached metadata must not pin the file. */
    } else if(!strcmp(argv[1],"negative")) {
        for(unsigned i=0;i<64;++i)missing(&root,"absent",-ENOENT);
        assert(backend.lookups==1 && backend.pins==1);
    } else if(!strcmp(argv[1],"generation")) {
        find(&root,"file",3,7);missing(&root,"absent",-ENOENT);
        backend.size=91;++backend.generation;find(&root,"file",3,91);
        backend.exists=false;++backend.generation;missing(&root,"file",-ENOENT);
        backend.exists=true;++backend.generation;find(&root,"file",3,91);
        assert(backend.lookups==5);
    } else if(!strcmp(argv[1],"parents")) {
        struct vfs_path directory={0};assert(!vfs_path_lookup_child(&root,"dir",&directory));
        for(unsigned i=0;i<16;++i){find(&root,"file",3,7);find(&directory,"file",4,7);}
        assert(backend.lookups==3);vfs_path_put(&directory);
    } else if(!strcmp(argv[1],"dynamic")) {
        backend.enabled=false;
        for(unsigned i=0;i<16;++i){backend.size=i;find(&root,"file",3,i);}
        assert(backend.lookups==16);
    } else if(!strcmp(argv[1],"stale_retain")) {
        find(&root,"file",3,7);retain_stale=1;
        find(&root,"file",3,7);assert(backend.lookups==2 && !retain_stale);
        find(&root,"file",3,7);assert(backend.lookups==2);
    } else if(!strcmp(argv[1],"errors")) {
        const int errors[]={-EIO,-EACCES,-ENOMEM,-EAGAIN,-ESTALE};
        for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
            backend.error=errors[i];missing(&root,"file",errors[i]);missing(&root,"file",errors[i]);
        }
        assert(backend.lookups==10);backend.error=0;find(&root,"file",3,7);assert(backend.lookups==11);
    } else if(!strcmp(argv[1],"permissions")) {
        struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_FOLLOW,.search=search};
        struct vfs_lookup_result result={0};
        assert(!vfs_path_resolve(&ns,&root,&root,"file",&policy,&result));vfs_path_put(&result.path);
        denied=1;assert(vfs_path_resolve(&ns,&root,&root,"file",&policy,&result)==-EACCES);
        assert(!result.path.mount && backend.lookups==1);
    } else if(!strcmp(argv[1],"bounds")) {
        missing(&root,"",-EINVAL);missing(&root,".",-EINVAL);missing(&root,"..",-EINVAL);
        missing(&root,"a/b",-EINVAL);char name[VFS_NAME_MAX+2];
        memset(name,'a',sizeof(name));name[VFS_NAME_MAX+1]=0;missing(&root,name,-ENAMETOOLONG);
        assert(!backend.lookups);name[VFS_NAME_MAX]=0;
        missing(&root,name,-ENOENT);missing(&root,name,-ENOENT);assert(backend.lookups==1);
    } else if(!strcmp(argv[1],"oom")) {
        fail_after=1;missing(&root,"missing",-ENOENT);assert(!fail_after);
        fail_after=3;find(&root,"file",3,7);assert(!fail_after);
        find(&root,"file",3,7);
    } else if(!strcmp(argv[1],"budget")) {
        char name[32];for(unsigned i=0;i<8192;++i){snprintf(name,sizeof(name),"absent-%u",i);missing(&root,name,-ENOENT);}
        struct vfs_lookup_cache_stats stats;vfs_lookup_cache_stats(&stats);
        assert(stats.entries && stats.bytes<=stats.budget && stats.evictions);
        uint64_t before=stats.entries;assert(vfs_lookup_cache_reclaim(5)==5);
        vfs_lookup_cache_stats(&stats);assert(stats.entries==before-5);
        assert(vfs_lookup_cache_reclaim(0)==0);
    } else assert(!"unknown test");
    teardown();printf("PASS VFS lookup cache %s\n",argv[1]);return 0;
}
