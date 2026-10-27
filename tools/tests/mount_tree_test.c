/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mntent.h>
#include <reliefnt/mount.h>
#include <reliefnt/pseudo_fs.h>
#include "../../kernel/module/internal.h"

static size_t allocations;
static unsigned fail_after, allocation_attempts;
void *kernel_malloc(size_t bytes)
{
    if(fail_after && ++allocation_attempts==fail_after)return NULL;
    void *result=calloc(1,bytes);if(result)++allocations;return result;
}
void kernel_free(void *pointer)
{if(pointer){assert(allocations);--allocations;free(pointer);}}
void kernel_spin_lock(struct kernel_spinlock *lock)
{assert(!lock->state);lock->state=1;}
void kernel_spin_unlock(struct kernel_spinlock *lock)
{assert(lock->state);lock->state=0;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{*flags=0;kernel_spin_lock(lock);}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{(void)flags;kernel_spin_unlock(lock);}
void module_memory_free(struct module_memory *memory){assert(!memory);}

/* Storage backends are the boundary: ids model held directory/inode objects,
 * not global path-prefix strings. All graph/path/ref handling is production. */
struct entry {unsigned id,parent;const char *name;uint32_t type;const char *link;unsigned pins;};
struct backend {struct entry *entries;size_t count;int lookup_error,retain_error;};
static struct entry a_entries[]={
    {1,1,"",RELIEFOS_FS_TYPE_DIR,NULL,0}, {2,1,"mnt",RELIEFOS_FS_TYPE_DIR,NULL,0},
    {3,1,"other",RELIEFOS_FS_TYPE_DIR,NULL,0}, {4,2,"old",RELIEFOS_FS_TYPE_FILE,NULL,0},
    {5,1,"file",RELIEFOS_FS_TYPE_FILE,NULL,0},
    {6,1,"absolute",RELIEFOS_FS_TYPE_SYMLINK,"/mnt/new",0},
    {7,1,"relative",RELIEFOS_FS_TYPE_SYMLINK,"mnt/new",0},
    {8,1,"loop",RELIEFOS_FS_TYPE_SYMLINK,"loop",0},
    {9,1,"two",RELIEFOS_FS_TYPE_SYMLINK,"relative",0},
    {10,1,"dirlink",RELIEFOS_FS_TYPE_SYMLINK,"mnt",0},
    {11,1,"slashfile",RELIEFOS_FS_TYPE_SYMLINK,"file/",0}
};
static struct entry b_entries[]={
    {1,1,"",RELIEFOS_FS_TYPE_DIR,NULL,0}, {2,1,"new",RELIEFOS_FS_TYPE_FILE,NULL,0},
    {3,1,"child",RELIEFOS_FS_TYPE_DIR,NULL,0}, {4,1,"up",RELIEFOS_FS_TYPE_SYMLINK,"..",0},
    {5,1,"absolute",RELIEFOS_FS_TYPE_SYMLINK,"/new",0},
    {6,1,"fd",RELIEFOS_FS_TYPE_SYMLINK,NULL,0}
};
static struct entry c_entries[]={
    {1,1,"",RELIEFOS_FS_TYPE_DIR,NULL,0}, {2,1,"nested",RELIEFOS_FS_TYPE_FILE,NULL,0}
};
static struct backend a={a_entries,sizeof(a_entries)/sizeof(a_entries[0]),0,0};
static struct backend b={b_entries,sizeof(b_entries)/sizeof(b_entries[0]),0,0};
static struct backend c={c_entries,sizeof(c_entries)/sizeof(c_entries[0]),0,0};
static struct entry *entry(struct backend *backend,unsigned id)
{
    for(size_t i=0;i<backend->count;++i)if(backend->entries[i].id==id)return &backend->entries[i];
    return NULL;
}
static uint64_t test_file_size=1;
static struct storage_node node(struct entry *e)
{return (struct storage_node){.type=e->type,.first_cluster=e->id,.mount_generation=1,
    .flags=e->type==RELIEFOS_FS_TYPE_SYMLINK && !e->link ? STORAGE_NODE_FLAG_MAGIC_LINK : 0,
    .size=e->id==4 ? test_file_size : 0};}
static int lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    struct backend *backend=context;if(backend->lookup_error)return backend->lookup_error;
    for(size_t i=0;i<backend->count;++i) {
        struct entry *e=&backend->entries[i];
        if(e->parent==parent->first_cluster && !strcmp(e->name,name)){*out=node(e);return 0;}
    }
    return -ENOENT;
}
static int retain(void *context,const struct storage_node *value,void **pin)
{
    struct backend *backend=context;if(backend->retain_error)return backend->retain_error;
    struct entry *e=entry(backend,value->first_cluster);assert(e);++e->pins;*pin=e;return 0;
}
static void release(void *context,const struct storage_node *value,void *pin)
{
    struct entry *e=entry(context,value->first_cluster);assert(e && e==pin && e->pins);--e->pins;
}
static bool same(void *context,const struct storage_node *left,const struct storage_node *right)
{(void)context;return left->first_cluster==right->first_cluster;}
static int readlink(void *context,const struct storage_node *value,char *buffer,size_t capacity,size_t *length)
{
    struct entry *e=entry(context,value->first_cluster);assert(e && e->link);
    *length=strlen(e->link);if(*length>=capacity)return -ENAMETOOLONG;
    memcpy(buffer,e->link,*length);return 0;
}
static struct vfs_path magic_target;
static int magic_error;
static int follow_magic(void *context,const struct storage_node *value,struct vfs_path *out)
{
    assert(context==&b && value->first_cluster==6);
    if(magic_error)return magic_error;
    vfs_path_get(&magic_target,out);return 0;
}
static const struct vfs_backend_ops ops={.lookup=lookup,.retain=retain,.release=release,
    .same_node=same,.readlink=readlink,.follow_link=follow_magic};
static struct vfs_mount *mount_new(struct backend *backend,struct module *owner)
{
    struct storage_node root=node(&backend->entries[0]);struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&ops,backend,owner,&root,&mount));return mount;
}
static struct vfs_path find(struct vfs_namespace *ns,const struct vfs_path *root,
                            const struct vfs_path *start,const char *name,unsigned id)
{
    struct vfs_path result={0};assert(!vfs_path_lookup(ns,root,start,name,VFS_LOOKUP_FOLLOW,&result));
    assert(vfs_path_node(&result)->first_cluster==id);return result;
}
static void formatted(const struct vfs_namespace *ns,const struct vfs_path *root,
                       const struct vfs_path *path,const char *want)
{
    char buffer[128];assert(!vfs_path_format(ns,root,path,buffer,sizeof(buffer)));
    assert(!strcmp(buffer,want));
}
static void backend_formatted(const struct vfs_path *path,const char *want)
{
    char buffer[128];assert(!vfs_path_backend_name(path,buffer,sizeof(buffer)));
    assert(!strcmp(buffer,want));
}
struct access_policy {void *blocked;unsigned checked;};
static int check_search(void *context,const struct vfs_path *path)
{
    struct access_policy *policy=context;++policy->checked;
    return vfs_path_context(path)==policy->blocked ? -EACCES : 0;
}
static void search_and_parent(struct vfs_namespace *ns,const struct vfs_path *root,
                                const struct vfs_path *old_dir)
{
    struct access_policy access={.blocked=&b};
    struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_FOLLOW,.search=check_search,.context=&access};
    struct vfs_lookup_result result={0};
    assert(vfs_path_resolve(ns,root,root,"/mnt/new",&policy,&result)==-EACCES);
    assert(!result.path.mount && access.checked>=2);
    assert(!vfs_path_resolve(ns,root,old_dir,"old",&policy,&result));
    assert(vfs_path_context(&result.path)==&a);vfs_path_put(&result.path);
    policy.flags=VFS_LOOKUP_PARENT;
    assert(vfs_path_resolve(ns,root,root,"/mnt/new-file",&policy,&result)==-EACCES);
    access.blocked=NULL;
    assert(!vfs_path_resolve(ns,root,root,"/dirlink/new-file/",&policy,&result));
    assert(vfs_path_context(&result.path)==&b && !strcmp(result.last,"new-file") && result.trailing);
    vfs_path_put(&result.path);
    assert(!vfs_path_resolve(ns,root,old_dir,"../fresh",&policy,&result));
    assert(vfs_path_equal(&result.path,root) && !strcmp(result.last,"fresh") && !result.trailing);
    vfs_path_put(&result.path);
    assert(!vfs_path_resolve(ns,root,old_dir,".",&policy,&result));
    assert(vfs_path_equal(&result.path,old_dir) && !strcmp(result.last,"."));vfs_path_put(&result.path);
    assert(vfs_path_resolve(ns,root,root,"/file/fresh",&policy,&result)==-ENOTDIR);
}

static void no_pins(struct backend *backend)
{for(size_t i=0;i<backend->count;++i)assert(!backend->entries[i].pins);}

static void magic_link_objects(void)
{
    struct vfs_mount *hidden=mount_new(&c,NULL);
    struct storage_node value=node(&c_entries[1]);size_t before=allocations;
    fail_after=1;allocation_attempts=0;
    assert(vfs_path_anonymous(hidden,&value,&magic_target)==-ENOMEM);
    fail_after=0;assert(!magic_target.mount && allocations==before && !c_entries[1].pins);
    c.retain_error=-EIO;
    assert(vfs_path_anonymous(hidden,&value,&magic_target)==-EIO);
    c.retain_error=0;assert(!magic_target.mount && allocations==before);
    struct storage_node invalid=node(&c_entries[0]);
    assert(vfs_path_anonymous(hidden,&invalid,&magic_target)==-EINVAL);
    assert(!vfs_path_anonymous(hidden,&value,&magic_target));vfs_mount_put(hidden);
    struct vfs_namespace ns={0};struct vfs_mount *base=mount_new(&b,NULL);
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    struct vfs_path root={0},opened={0},link={0};assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"fd",0,&link));
    assert(vfs_path_node(&link)->type==RELIEFOS_FS_TYPE_SYMLINK);vfs_path_put(&link);
    magic_error=-EACCES;
    assert(vfs_path_lookup(&ns,&root,&root,"fd",VFS_LOOKUP_FOLLOW,&opened)==-EACCES && !opened.mount);
    magic_error=0;
    assert(!vfs_path_lookup(&ns,&root,&root,"fd",VFS_LOOKUP_FOLLOW,&opened));
    assert(vfs_path_equal(&opened,&magic_target) && vfs_path_node(&opened)->first_cluster==2);
    assert(vfs_path_lookup(&ns,&root,&root,"fd/..",VFS_LOOKUP_FOLLOW,&link)==-ENOTDIR && !link.mount);
    char buffer[64];assert(vfs_path_backend_name(&opened,buffer,sizeof(buffer))==-ENOENT);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));no_pins(&b);
    /* The namespace and proc link can disappear before the independently
     * pinned anonymous target. Its own backing/code lifetime is still valid. */
    vfs_path_put(&magic_target);assert(vfs_path_node(&opened)->first_cluster==2 && c_entries[1].pins==1);
    vfs_path_put(&opened);no_pins(&c);assert(!allocations);
    puts("PASS referenced magic-link jumps: anonymous target, permission failure, nofollow, suffix errors and independent lifetime");
}

static void renamed_paths(void)
{
    struct vfs_namespace ns={0};struct vfs_mount *base=mount_new(&a,NULL);
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    struct vfs_path root={0};assert(!vfs_namespace_root(&ns,&root));
    struct vfs_path directory=find(&ns,&root,&root,"mnt",2);
    struct vfs_path old=find(&ns,&root,&directory,"old",4);
    struct vfs_path other=find(&ns,&root,&root,"other",3);
    struct vfs_rename *change=NULL;
    size_t before=allocations;
    for(unsigned nth=1;nth<=2;++nth) {
        fail_after=nth;allocation_attempts=0;
        assert(vfs_rename_prepare(&ns,&directory,&other,"moved",&change)==-ENOMEM);
        fail_after=0;assert(!change && allocations==before);
        formatted(&ns,&root,&old,"/mnt/old");
    }
    assert(vfs_rename_prepare(&ns,&directory,&directory,"cycle",&change)==-EINVAL);
    assert(vfs_rename_prepare(&ns,&directory,&other,"..",&change)==-EINVAL);
    assert(vfs_rename_prepare(&ns,&root,&other,"base",&change)==-EBUSY);
    assert(!vfs_rename_prepare(&ns,&directory,&other,"moved",&change));
    vfs_rename_abort(change);change=NULL;
    formatted(&ns,&root,&old,"/mnt/old");assert(allocations==before);
    assert(!vfs_rename_prepare(&ns,&directory,&other,"moved",&change));
    /* This fixture's backend boundary now commits the persistent rename. */
    a_entries[1].parent=3;a_entries[1].name="moved";
    vfs_rename_commit(change);change=NULL;
    formatted(&ns,&root,&old,"/other/moved/old");
    struct vfs_path alias=find(&ns,&root,&root,"other/moved",2);
    assert(vfs_path_equal(&alias,&directory));vfs_path_put(&alias);
    alias=find(&ns,&root,&directory,"..",3);
    assert(vfs_path_equal(&alias,&other));vfs_path_put(&alias);
    struct vfs_path file=find(&ns,&root,&root,"file",5);
    assert(!vfs_rename_prepare(&ns,&file,&directory,"old",&change));
    a_entries[3].parent=0;a_entries[4].parent=2;a_entries[4].name="old";
    vfs_rename_commit(change);change=NULL;
    char buffer[128];assert(vfs_path_format(&ns,&root,&old,buffer,sizeof(buffer))==-ENOENT);
    assert(!vfs_path_format_link(&ns,&root,&old,buffer,sizeof(buffer)) &&
        !strcmp(buffer,"/other/moved/old (deleted)"));
    assert(vfs_path_node(&old)->first_cluster==4); /* original open object survives */
    alias=find(&ns,&root,&directory,"old",5);
    assert(vfs_path_equal(&alias,&file));vfs_path_put(&alias);
    formatted(&ns,&root,&file,"/other/moved/old");
    /* Renaming a file to itself is a POSIX no-op. */
    assert(!vfs_rename_prepare(&ns,&file,&directory,"old",&change));
    vfs_rename_commit(change);change=NULL;formatted(&ns,&root,&file,"/other/moved/old");
    vfs_path_unlinked(&file);
    assert(vfs_path_format(&ns,&root,&file,buffer,sizeof(buffer))==-ENOENT);
    assert(!vfs_path_format_link(&ns,&root,&file,buffer,sizeof(buffer)) &&
        !strcmp(buffer,"/other/moved/old (deleted)"));
    assert(vfs_path_lookup(&ns,&root,&file,"..",0,&alias)==-ENOTDIR);
    a_entries[4].parent=0;a_entries[1].parent=0;vfs_path_unlinked(&directory);
    alias=find(&ns,&root,&directory,"..",3);vfs_path_put(&alias);
    vfs_path_put(&file);vfs_path_put(&old);vfs_path_put(&directory);
    vfs_path_put(&other);vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    no_pins(&a);assert(!allocations);
    a_entries[1].parent=1;a_entries[1].name="mnt";a_entries[3].parent=2;
    a_entries[4].parent=1;a_entries[4].name="file";
    puts("PASS rename/unlink cache identity, held cwd parent updates, replaced-object retention and prepare rollback");
}

static unsigned context_releases;
static struct module_runtime *context_release_owner;
static void release_context(void *context)
{
    assert(context==&c);no_pins(&c);
    assert(context_release_owner->users==1);++context_releases;
}
static void mount_context_lifetime(struct module *owner)
{
    struct vfs_backend_ops owned=ops;owned.destroy=release_context;
    struct vfs_mount *mount=NULL;struct storage_node root_node=node(&c_entries[0]);
    assert(!vfs_mount_new(&owned,&c,owner,&root_node,&mount));
    struct vfs_namespace ns={0};assert(!vfs_namespace_init(&ns,mount));
    vfs_mount_put(mount);struct vfs_path root={0};assert(!vfs_namespace_root(&ns,&root));
    assert(vfs_namespace_destroy(&ns)==-EBUSY && !context_releases);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    assert(context_releases==1 && !context_release_owner->users);
}
static unsigned finish_attempts,finish_failures;
static int finish_context(void *context)
{
    assert(context==&c);no_pins(&c);assert(context_release_owner->users==1);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK);++finish_attempts;
    if(finish_failures){--finish_failures;return -EIO;}return 0;
}
static void mount_failed_teardown(struct module *owner)
{
    struct vfs_backend_ops owned=ops;owned.finish=finish_context;owned.destroy=release_context;
    struct vfs_mount *mount=NULL;struct storage_node root_node=node(&c_entries[0]);
    unsigned released=context_releases;size_t before=allocations;
    assert(!vfs_mount_new(&owned,&c,owner,&root_node,&mount));
    assert(!vfs_mount_describe(mount,"retiring-source","testfs",123));
    finish_failures=2;finish_attempts=0;vfs_mount_put(mount);
    assert(finish_attempts==1 && context_releases==released && context_release_owner->users==1);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK && allocations>before);
    assert(!vfs_reclaim_mounts(0) && finish_attempts==1);
    assert(!vfs_reclaim_mounts(8) && finish_attempts==2 && context_release_owner->users==1);
    assert(vfs_reclaim_mounts(1)==1 && finish_attempts==3 && context_releases==released+1);
    assert(!context_release_owner->users && allocations==before && !vfs_reclaim_mounts(8));
    puts("PASS failed detached backend cleanup pins actual module until bounded retry and final destroy");
}
static unsigned pseudo_lookup_calls;
static int owned_pseudo_lookup(const char *name,struct storage_node *out)
{
    assert(context_release_owner->users);++pseudo_lookup_calls;
    if(strcmp(name,"/virtual") && strcmp(name,"/virtual/file"))return -ENOENT;
    *out=(struct storage_node){.type=!strcmp(name,"/virtual") ? RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE};
    return 0;
}
static int owned_pseudo_read(const char *name,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{
    assert(!strcmp(name,"/virtual/file") && !offset && buffer && capacity>=5);
    assert(context_release_owner->users && module_unload("test_fs",0)==-EWOULDBLOCK);
    memcpy(buffer,"owned",5);*read=5;return 0;
}
static void pseudo_provider_lifetime(struct module *owner)
{
    const struct vfs_pseudo_provider provider={.root="/virtual",.owner=owner,
        .lookup=owned_pseudo_lookup,.read=owned_pseudo_read};
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};
    context_release_owner->state=MODULE_COMING;
    assert(vfs_pseudo_mount_new(&provider,&mount)==-ENODEV && !mount && !pseudo_lookup_calls);
    context_release_owner->state=MODULE_LIVE;
    assert(!vfs_pseudo_mount_new(&provider,&mount));
    assert(context_release_owner->users==1 && pseudo_lookup_calls==1);
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},file={0};assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"file",0,&file));
    char bytes[8];uint32_t read=0;
    assert(!vfs_path_read(&file,0,bytes,sizeof(bytes),&read) && read==5 && !memcmp(bytes,"owned",5));
    vfs_path_put(&root);assert(vfs_namespace_destroy(&ns)==-EBUSY);
    vfs_path_put(&file);assert(!vfs_namespace_destroy(&ns) && !context_release_owner->users);
}

/** @brief Exercise streamed proc views from actual attached and moved mounts.
 * @return None. Asserts parent-before-child ordering, chroot visibility,
 * escaping, allocation rollback and retained path ownership; frees all mounts.
 */
static void mount_table_objects(void)
{
    struct vfs_namespace ns={0};struct vfs_mount *base=mount_new(&a,NULL);
    assert(!vfs_mount_describe(base,"rootfs","testfs",0x800));
    size_t before=allocations;fail_after=1;allocation_attempts=0;
    assert(vfs_mount_describe(base,"replacement","wrong",99)==-ENOMEM);
    fail_after=0;assert(allocations==before);
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(vfs_mount_describe(base,"late","wrong",99)==-EBUSY);
    struct vfs_path root={0};assert(!vfs_namespace_root(&ns,&root));
    struct vfs_path point=find(&ns,&root,&root,"mnt",2);
    struct vfs_path other=find(&ns,&root,&root,"other",3);
    struct vfs_mount *mounted=mount_new(&b,NULL);
    assert(!vfs_mount_describe(mounted,"a b\\c\td\ne","tmpfs",2));
    assert(!vfs_mount_attach(&ns,mounted,&point));vfs_mount_put(mounted);
    struct vfs_path inside=find(&ns,&root,&root,"mnt",1);
    struct vfs_path child=find(&ns,&root,&inside,"child",3);
    mounted=mount_new(&c,NULL);assert(!vfs_mount_describe(mounted,"nested","testfs",0x110304));
    assert(!vfs_mount_attach(&ns,mounted,&child));vfs_mount_put(mounted);
    char text[4096]={0},chunks[4096]={0};uint32_t count=0,got=0;
    char diagnostic[64];
    assert(!vfs_namespace_context_mountpoint(&ns,&b,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/mnt"));
    assert(!vfs_namespace_context_mountpoint(&ns,&c,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/mnt/child"));
    assert(vfs_namespace_context_mountpoint(&ns,&ns,diagnostic,sizeof(diagnostic))==-ENOENT && !diagnostic[0]);
    assert(!vfs_read_mount_table(&ns,&root,true,0,text,sizeof(text)-1,&count));text[count]=0;
    assert(strstr(text,"1 1 8:0 / / rw - testfs rootfs rw\n"));
    assert(strstr(text,"2 1 0:2 / /mnt rw - tmpfs a\\040b\\134c\\011d\\012e rw\n"));
    assert(strstr(text,"3 2 259:260 / /mnt/child rw - testfs nested rw\n"));
    assert(strstr(text,"2 1 0:2 ")<strstr(text,"3 2 259:260 "));
    assert(!strstr(text,"/dev") && !strstr(text,"/proc") && !strstr(text,"/sys"));
    for(uint32_t position=0;position<count;position+=got)
        assert(!vfs_read_mount_table(&ns,&root,true,position,chunks+position,7,&got) && got);
    assert(!memcmp(text,chunks,count));
    assert(!vfs_read_mount_table(&ns,&root,true,UINT64_MAX,chunks,10,&got) && !got);
    assert(vfs_read_mount_table(&ns,&root,true,0,NULL,1,&got)==-EINVAL && !got);
    before=allocations;fail_after=1;allocation_attempts=0;got=42;
    assert(vfs_read_mount_table(&ns,&root,true,0,chunks,sizeof(chunks),&got)==-ENOMEM && !got);
    fail_after=0;assert(allocations==before);
    assert(!vfs_read_mount_table(&ns,&root,true,0,NULL,0,&got) && !got);
    assert(!vfs_mount_move(&ns,&inside,&other));
    assert(!vfs_namespace_context_mountpoint(&ns,&b,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/other"));
    assert(!vfs_namespace_context_mountpoint(&ns,&c,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/other/child"));
    assert(vfs_namespace_context_mountpoint(&ns,&c,diagnostic,3)==-ENAMETOOLONG && !diagnostic[0]);
    assert(!vfs_read_mount_table(&ns,&root,false,0,text,sizeof(text)-1,&count));text[count]=0;
    assert(strstr(text,"a\\040b\\134c\\011d\\012e /other tmpfs rw 0 0\n"));
    assert(strstr(text,"nested /other/child testfs rw 0 0\n") && !strstr(text,"/mnt"));
    mounted=mount_new(&b,NULL);assert(!vfs_mount_attach(&ns,mounted,&point));vfs_mount_put(mounted);
    struct vfs_path alias=find(&ns,&root,&root,"mnt",1);
    assert(!vfs_namespace_context_mountpoint(&ns,&b,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/other"));
    /* Mount IDs do not imply ancestry: move ID3 below the newer parent ID4.
     * Consumers such as GNU GRUB must encounter the parent before this child. */
    struct vfs_path newer_parent_child=find(&ns,&root,&alias,"child",3);
    struct vfs_path moved=find(&ns,&root,&root,"other/child",1);
    assert(!vfs_mount_move(&ns,&moved,&newer_parent_child));
    char ordered[4096]={0};uint32_t ordered_count=0;
    assert(!vfs_read_mount_table(&ns,&root,true,0,ordered,sizeof(ordered)-1,&ordered_count));
    ordered[ordered_count]=0;
    assert(strstr(ordered,"4 1 0:0 / /mnt ") && strstr(ordered,"3 4 259:260 / /mnt/child "));
    assert(strstr(ordered,"4 1 0:0 ")<strstr(ordered,"3 4 259:260 "));
    for(uint32_t position=0;position<ordered_count;position+=got)
        assert(!vfs_read_mount_table(&ns,&root,true,position,chunks+position,7,&got) && got);
    assert(!memcmp(ordered,chunks,ordered_count));
    assert(!vfs_read_mount_table(&ns,&alias,true,0,ordered,sizeof(ordered)-1,&ordered_count));
    ordered[ordered_count]=0;
    assert(strstr(ordered,"4 1 0:0 / / ") && strstr(ordered,"3 4 259:260 / /child "));
    assert(strstr(ordered,"4 1 0:0 ")<strstr(ordered,"3 4 259:260 "));
    assert(!vfs_mount_move(&ns,&moved,&child));
    vfs_path_put(&moved);vfs_path_put(&newer_parent_child);
    FILE *stream=tmpfile();assert(stream && fwrite(text,1,count,stream)==count);rewind(stream);
    bool decoded=false;struct mntent *entry;
    while((entry=getmntent(stream)))if(!strcmp(entry->mnt_type,"tmpfs")) {
        assert(!strcmp(entry->mnt_fsname,"a b\\c\td\ne") && !strcmp(entry->mnt_dir,"/other"));decoded=true;
    }
    assert(decoded && !fclose(stream));
    assert(!vfs_read_mount_table(&ns,&inside,true,0,text,sizeof(text)-1,&count));text[count]=0;
    assert(strstr(text,"2 1 0:2 / / rw - tmpfs") && strstr(text,"3 2 259:260 / /child rw"));
    assert(!strstr(text,"rootfs"));
    /* Linux skips mount roots outside chroot; it never invents a new mount
     * root from the process's subdirectory (Linux6.12 show_mountinfo). */
    assert(!vfs_read_mount_table(&ns,&child,true,0,text,sizeof(text)-1,&count));text[count]=0;
    assert(!strcmp(text,"3 2 259:260 / / rw - testfs nested rw\n"));
    struct vfs_path nested=find(&ns,&root,&root,"other/child",1);
    assert(!vfs_mount_unmount(&ns,&nested));vfs_path_put(&nested);vfs_path_put(&child);
    assert(!vfs_mount_unmount(&ns,&inside));vfs_path_put(&inside);
    assert(!vfs_namespace_context_mountpoint(&ns,&b,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/mnt"));
    assert(!vfs_mount_unmount(&ns,&alias));
    assert(vfs_namespace_context_mountpoint(&ns,&b,diagnostic,sizeof(diagnostic))==-ENOENT && !diagnostic[0]);
    vfs_path_put(&alias);
    assert(!vfs_read_mount_table(&ns,&root,true,0,text,sizeof(text)-1,&count));text[count]=0;
    assert(!strcmp(text,"1 1 8:0 / / rw - testfs rootfs rw\n"));
    vfs_path_put(&point);vfs_path_put(&other);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));no_pins(&a);no_pins(&b);no_pins(&c);assert(!allocations);
    puts("PASS proc mount views follow actual moves, subtree parent IDs, escaping, chroot, chunked reads and detach");
}

int main(void)
{
    mount_table_objects();
    magic_link_objects();
    renamed_paths();
    struct module owner={0};strcpy(owner.name,"test_fs");
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(runtime->name,"test_fs");runtime->owner=&owner;owner.runtime=runtime;
    runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    struct storage_node root_node=node(&b_entries[0]);struct vfs_mount *unpublished=NULL;
    assert(vfs_mount_new(&ops,&b,&owner,&root_node,&unpublished)==-ENODEV && !unpublished);
    assert(!runtime->users);runtime->state=MODULE_LIVE;
    context_release_owner=runtime;mount_context_lifetime(&owner);mount_failed_teardown(&owner);
    pseudo_provider_lifetime(&owner);
    for(unsigned nth=1;nth<=2;++nth) {
        fail_after=nth;allocation_attempts=0;
        assert(vfs_mount_new(&ops,&b,&owner,&root_node,&unpublished)==-ENOMEM && !unpublished);
        fail_after=0;assert(!runtime->users);no_pins(&b);
    }
    struct vfs_namespace ns={0};struct vfs_mount *base=mount_new(&a,NULL);
    assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    struct vfs_path root={0};assert(!vfs_namespace_root(&ns,&root));
    struct vfs_path before=find(&ns,&root,&root,"/mnt",2);
    struct vfs_path old=find(&ns,&root,&before,"old",4);
    assert(vfs_path_node(&old)->size==1);test_file_size=2;
    struct vfs_path refreshed=find(&ns,&root,&before,"old",4);
    assert(vfs_path_equal(&old,&refreshed) && vfs_path_node(&refreshed)->size==2);
    vfs_path_put(&refreshed);
    struct vfs_mount *mounted=mount_new(&b,&owner);
    assert(runtime->users==1 && module_unload("test_fs",0)==-EWOULDBLOCK);
    assert(!vfs_mount_attach(&ns,mounted,&before));vfs_mount_put(mounted);
    struct vfs_path top=find(&ns,&root,&root,"/mnt",1);
    backend_formatted(&top,"/");backend_formatted(&old,"/mnt/old");
    assert(vfs_path_context(&top)==&b && !vfs_path_equal(&top,&before));
    struct vfs_path again=find(&ns,&root,&before,"old",4);assert(vfs_path_equal(&again,&old));vfs_path_put(&again);
    again=find(&ns,&root,&before,".",2);assert(vfs_path_equal(&again,&before));vfs_path_put(&again);
    again=find(&ns,&root,&root,"/mnt/new",2);assert(vfs_path_context(&again)==&b);vfs_path_put(&again);
    again=find(&ns,&root,&root,"/absolute",2);vfs_path_put(&again);
    again=find(&ns,&root,&root,"/relative",2);vfs_path_put(&again);
    again=find(&ns,&root,&root,"/two",2);vfs_path_put(&again);
    assert(vfs_path_lookup(&ns,&root,&root,"/loop",VFS_LOOKUP_FOLLOW,&again)==-ELOOP);
    assert(vfs_path_lookup(&ns,&root,&root,"/file/..",VFS_LOOKUP_FOLLOW,&again)==-ENOTDIR);
    assert(vfs_path_lookup(&ns,&root,&root,"/file/",VFS_LOOKUP_FOLLOW,&again)==-ENOTDIR);
    assert(vfs_path_lookup(&ns,&root,&root,"/slashfile",VFS_LOOKUP_FOLLOW,&again)==-ENOTDIR);
    assert(!vfs_path_lookup(&ns,&root,&root,"/relative",0,&again));
    assert(vfs_path_node(&again)->type==RELIEFOS_FS_TYPE_SYMLINK);vfs_path_put(&again);
    again=find(&ns,&top,&top,"../../new",2);assert(vfs_path_context(&again)==&b);vfs_path_put(&again);
    again=find(&ns,&top,&top,"absolute",2);vfs_path_put(&again);
    again=find(&ns,&root,&top,"..",1);assert(vfs_path_equal(&again,&root));vfs_path_put(&again);
    formatted(&ns,&root,&old,"/mnt/old");formatted(&ns,&top,&top,"/");
    search_and_parent(&ns,&root,&before);
    struct vfs_path child=find(&ns,&root,&top,"child",3);
    backend_formatted(&child,"/child");
    struct vfs_mount *nested=mount_new(&c,NULL);assert(!vfs_mount_attach(&ns,nested,&child));vfs_mount_put(nested);
    struct vfs_path nested_root=find(&ns,&root,&root,"/mnt/child",1);
    struct vfs_path other=find(&ns,&root,&root,"other",3);
    assert(vfs_mount_move(&ns,&top,&child)==-ELOOP);
    assert(vfs_mount_move(&ns,&root,&other)==-EBUSY);
    assert(vfs_mount_move(&ns,&old,&other)==-EINVAL);
    assert(!vfs_mount_move(&ns,&top,&other));
    backend_formatted(&top,"/");backend_formatted(&child,"/child");
    formatted(&ns,&root,&top,"/other");formatted(&ns,&root,&nested_root,"/other/child");
    formatted(&ns,&root,&old,"/mnt/old");
    again=find(&ns,&root,&root,"/other/child/nested",2);vfs_path_put(&again);
    again=find(&ns,&root,&root,"/mnt/old",4);vfs_path_put(&again);
    assert(vfs_mount_unmount(&ns,&top)==-EBUSY);
    assert(!vfs_mount_unmount(&ns,&nested_root));vfs_path_put(&nested_root);vfs_path_put(&child);
    assert(!vfs_mount_unmount(&ns,&top));vfs_path_put(&top);
    assert(!runtime->users);
    assert(!module_unload("test_fs",0) && !owner.runtime);
    vfs_path_put(&other);vfs_path_put(&before);vfs_path_put(&old);
    assert(vfs_namespace_destroy(&ns)==-EBUSY);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));no_pins(&a);no_pins(&b);no_pins(&c);assert(!allocations);

    /* Moving a new root over / must not redirect previously held roots/cwds. */
    base=mount_new(&a,NULL);assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(!vfs_namespace_root(&ns,&root));other=find(&ns,&root,&root,"other",3);
    mounted=mount_new(&b,NULL);assert(!vfs_mount_attach(&ns,mounted,&other));vfs_mount_put(mounted);
    top=find(&ns,&root,&root,"/other",1);assert(!vfs_mount_move(&ns,&top,&root));
    struct vfs_path visible={0};assert(!vfs_namespace_root(&ns,&visible));assert(vfs_path_equal(&visible,&top));
    again=find(&ns,&root,&root,"/file",5);vfs_path_put(&again);
    again=find(&ns,&visible,&visible,"/new",2);vfs_path_put(&again);
    char text[32];assert(vfs_path_format(&ns,&visible,&other,text,sizeof(text))==-ENOENT);
    vfs_path_put(&visible);assert(!vfs_mount_unmount(&ns,&top));vfs_path_put(&top);
    vfs_path_put(&other);vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    no_pins(&a);no_pins(&b);assert(!allocations);

    /* Every allocation/retain failure leaves neither a mount pin nor an owner. */
    for(unsigned nth=1;nth<=2;++nth) {
        fail_after=nth;allocation_attempts=0;unpublished=NULL;
        assert(vfs_mount_new(&ops,&b,NULL,&root_node,&unpublished)==-ENOMEM && !unpublished);
        assert(!allocations);no_pins(&b);fail_after=0;
    }
    b.retain_error=-EIO;assert(vfs_mount_new(&ops,&b,NULL,&root_node,&unpublished)==-EIO);
    b.retain_error=0;assert(!allocations);no_pins(&b);
    base=mount_new(&a,NULL);assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(!vfs_namespace_root(&ns,&root));size_t saved=allocations;
    fail_after=1;allocation_attempts=0;
    assert(vfs_path_lookup(&ns,&root,&root,"mnt",VFS_LOOKUP_FOLLOW,&again)==-ENOMEM);
    fail_after=0;assert(allocations==saved);assert(a_entries[1].pins==0);
    a.retain_error=-EIO;assert(vfs_path_lookup(&ns,&root,&root,"mnt",VFS_LOOKUP_FOLLOW,&again)==-EIO);
    a.retain_error=0;assert(allocations==saved && a_entries[1].pins==0);
    a.lookup_error=-EAGAIN;assert(vfs_path_lookup(&ns,&root,&root,"mnt",VFS_LOOKUP_FOLLOW,&again)==-EAGAIN);
    a.lookup_error=0;vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));assert(!allocations);
    no_pins(&a);no_pins(&b);no_pins(&c);
    /* Stacks and independent namespaces are real graph edges, without a small
     * fixed mount array or accidental cross-namespace path acceptance. */
    base=mount_new(&a,NULL);assert(!vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(!vfs_namespace_root(&ns,&root));before=find(&ns,&root,&root,"mnt",2);
    struct vfs_namespace foreign={0};base=mount_new(&a,NULL);
    assert(!vfs_namespace_init(&foreign,base));vfs_mount_put(base);
    struct vfs_path foreign_root={0};assert(!vfs_namespace_root(&foreign,&foreign_root));
    mounted=mount_new(&b,NULL);
    assert(vfs_mount_attach(&ns,mounted,&foreign_root)==-EXDEV);
    assert(!vfs_mount_attach(&ns,mounted,&before));vfs_mount_put(mounted);
    struct vfs_mount *second=mount_new(&c,NULL);
    assert(vfs_mount_attach(&ns,second,&before)==-EBUSY);vfs_mount_put(second);
    struct vfs_path stack[80];stack[0]=find(&ns,&root,&root,"/mnt",1);
    assert(vfs_mount_move(&ns,&stack[0],&foreign_root)==-EXDEV);
    assert(vfs_path_lookup(&ns,&root,&foreign_root,".",0,&again)==-EXDEV);
    for(unsigned i=1;i<80;++i) {
        mounted=mount_new(&b,NULL);assert(!vfs_mount_attach(&ns,mounted,&stack[i-1]));vfs_mount_put(mounted);
        stack[i]=find(&ns,&root,&root,"/mnt",1);
        assert(!vfs_path_equal(&stack[i-1],&stack[i]));
    }
    for(unsigned i=80;i>0;--i){assert(!vfs_mount_unmount(&ns,&stack[i-1]));vfs_path_put(&stack[i-1]);}
    vfs_path_put(&before);vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    vfs_path_put(&foreign_root);assert(!vfs_namespace_destroy(&foreign));
    no_pins(&a);no_pins(&b);no_pins(&c);assert(!allocations);
    puts("PASS referenced mount tree: covered cwd/dirfd/root, chroot boundary, symlinks, MS_MOVE subtree/rollback, busy unmount, actual FS module pins and failure cleanup");
    return 0;
}
