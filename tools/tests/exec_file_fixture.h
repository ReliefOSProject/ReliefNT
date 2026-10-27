/* Test-only inode/provider boundary. ELF sources and mount/dentry lifetimes
 * use the real VFS; storage inode exclusion is checked by this small fixture. */
#ifndef RELIEFNT_TEST_EXEC_FILE_FIXTURE_H
#define RELIEFNT_TEST_EXEC_FILE_FIXTURE_H
#include <assert.h>
#include <reliefnt/exec_file.h>
#include <reliefnt/module.h>
#include <linux/errno.h>

static unsigned exec_fixture_pins,exec_fixture_inode_refs,exec_fixture_denials;
static uint64_t exec_fixture_size;
static int exec_fixture_read(void *,const struct storage_node *,uint64_t,void *,uint32_t,uint32_t *,uint64_t);
static int exec_fixture_lookup(void *context,const struct storage_node *parent,
                                const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -ENOENT;}
static int exec_fixture_retain(void *context,const struct storage_node *node,void **pin)
{assert(context==&exec_fixture_pins && node && pin);++exec_fixture_pins;*pin=NULL;return 0;}
static void exec_fixture_release(void *context,const struct storage_node *node,void *pin)
{assert(context==&exec_fixture_pins && node && !pin && exec_fixture_pins);--exec_fixture_pins;}
static bool exec_fixture_same(void *context,const struct storage_node *a,const struct storage_node *b)
{assert(context==&exec_fixture_pins);return a->first_cluster==b->first_cluster;}
static int exec_fixture_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{assert(context==&exec_fixture_pins && vfs_path_node(path));*out=(struct linux_stat_abi){.st_size=(int64_t)exec_fixture_size};return 0;}
static const struct vfs_backend_ops exec_fixture_ops={
    .lookup=exec_fixture_lookup,.retain=exec_fixture_retain,.release=exec_fixture_release,
    .same_node=exec_fixture_same,.getattr=exec_fixture_stat,.read=exec_fixture_read,
};
int storage_inode_get(const struct storage_node *node,struct storage_inode_ref **out)
{assert(node && out);++exec_fixture_inode_refs;*out=(void *)&exec_fixture_inode_refs;return 0;}
int storage_inode_put(struct storage_inode_ref *inode)
{if(inode){assert(inode==(void *)&exec_fixture_inode_refs && exec_fixture_inode_refs);--exec_fixture_inode_refs;}return 0;}
int storage_inode_deny_write(struct storage_inode_ref *inode)
{assert(inode==(void *)&exec_fixture_inode_refs && exec_fixture_inode_refs);++exec_fixture_denials;return 0;}
void storage_inode_allow_write(struct storage_inode_ref *inode)
{assert(inode==(void *)&exec_fixture_inode_refs && exec_fixture_denials);--exec_fixture_denials;}
static struct vfs_exec_source *exec_fixture_open(const struct storage_node *node)
{
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    struct vfs_path path={0};struct vfs_exec_source *source=NULL;
    exec_fixture_size=node->size;
    assert(!vfs_mount_new(&exec_fixture_ops,&exec_fixture_pins,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,node,&path));
    assert(!vfs_exec_source_open(&path,&source) && source);
    vfs_path_put(&path);vfs_mount_put(mount);
    assert(exec_fixture_pins==2 && exec_fixture_inode_refs==1 && exec_fixture_denials==1);
    return source;
}
#endif
