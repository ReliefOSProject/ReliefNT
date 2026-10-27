/* SPDX-License-Identifier: Apache-2.0 */
/* Real procfs provider plus the real shared resolver and DAC. Only the small
 * root-directory inode provider and hardware/user memory boundaries are fake. */
#define main proc_fixture_main
#include "procfs_directories_test.c"
#undef main
#include <reliefnt/permissions.h>

static int rootfs_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    (void)context;
    static const struct {unsigned parent,id;const char *name;uint32_t type;} nodes[]={
        {1,2,"etc",RELIEFOS_FS_TYPE_DIR}, {2,3,"mtab",RELIEFOS_FS_TYPE_SYMLINK},
        {1,4,"proc",RELIEFOS_FS_TYPE_DIR}, {1,5,"home",RELIEFOS_FS_TYPE_DIR},
        {5,6,"test",RELIEFOS_FS_TYPE_DIR}
    };
    for(unsigned i=0;i<sizeof(nodes)/sizeof(nodes[0]);++i)
        if(nodes[i].parent==parent->first_cluster && !strcmp(nodes[i].name,name))
            {*out=(struct storage_node){.type=nodes[i].type,.first_cluster=nodes[i].id};return 0;}
    return -ENOENT;
}
static int rootfs_link(void *context,const struct storage_node *node,char *out,size_t capacity,size_t *length)
{
    (void)context;assert(node->first_cluster==3);const char target[]="../proc/mounts";
    *length=sizeof(target)-1;if(capacity<*length)return -ENAMETOOLONG;memcpy(out,target,*length);return 0;
}
static int rootfs_permissions(void *context,const struct vfs_path *path,struct reliefos_permissions *value,bool write)
{
    (void)context;if(write)return -EROFS;
    *value=(struct reliefos_permissions){vfs_path_node(path)->type==RELIEFOS_FS_TYPE_SYMLINK ? 0777 : 0755,0,0};return 0;
}
static int rootfs_search(void *context,const struct vfs_path *path)
{return fs_permissions_check_path(context,path,FS_ACCESS_EXEC,false);}
int main(void)
{
    unsigned before=proc_allocations;
    static const struct vfs_backend_ops ops={.lookup=rootfs_lookup,.retain=proc_path_retain,
        .release=proc_path_release,.same_node=proc_path_same,.readlink=rootfs_link,.permissions=rootfs_permissions};
    struct storage_node node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};
    struct vfs_namespace ns={0};struct vfs_mount *base=NULL,*proc=NULL;
    struct vfs_path root={0},point={0},cwd={0};
    assert(!vfs_mount_new(&ops,NULL,NULL,&node,&base) && !vfs_namespace_init(&ns,base));vfs_mount_put(base);
    assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"/proc",0,&point));
    assert(!proc_create_vfs_mount(&proc) && !vfs_mount_attach(&ns,proc,&point));vfs_mount_put(proc);
    assert(!vfs_path_lookup(&ns,&root,&root,"/home/test",0,&cwd));
    vfs_path_get(&root,&current.fs_paths.root);vfs_path_get(&cwd,&current.fs_paths.cwd);
    strcpy(sched_task_cwd(&current),"/deliberately-stale-name");
    const char *inputs[]={"/etc/mtab","/proc/self/mountinfo","/proc/self/cwd","/proc/self"};
    const char *expected[]={"/proc/42/mounts","/proc/42/mountinfo","/home/test","/proc/self"};
    for(unsigned i=0;i<sizeof(inputs)/sizeof(inputs[0]);++i) {
        struct vfs_lookup_policy policy={.flags=i==3 ? 0 : VFS_LOOKUP_FOLLOW,
            .search=rootfs_search,.context=&current};
        struct vfs_lookup_result found={0};char name[128];
        assert(!vfs_path_resolve(&ns,&root,&cwd,inputs[i],&policy,&found));
        assert(!vfs_path_format(&ns,&root,&found.path,name,sizeof(name)) && !strcmp(name,expected[i]));
        vfs_path_put(&found.path);
    }
    vfs_path_put(&current.fs_paths.root);vfs_path_put(&current.fs_paths.cwd);
    vfs_path_put(&cwd);vfs_path_put(&point);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));sched_task_identity_detach(&current);
    assert(!proc_path_pins && proc_allocations==before);
    puts("PASS actual permission walker: mtab/proc mounts, self/mountinfo, retained cwd despite stale text and nofollow self");
    return 0;
}
