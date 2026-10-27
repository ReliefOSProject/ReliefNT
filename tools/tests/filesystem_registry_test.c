/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/fs_type.h>
#include <reliefnt/mount.h>
#include "../../kernel/module/internal.h"
#include "../../block/core/storage_internal.h"

static struct module owner;
static size_t allocations;
static long fail_after=-1;
static unsigned factory_calls,holds,drops,destroyed;
static bool finish_ready=true;
struct storage_volume *g_active_volume;
static unsigned native_starts,native_destroys;
static bool native_fail;
static int native_mount(struct storage_volume *volume)
{
    assert(volume==g_active_volume && volume->fs_type.type && owner.runtime->users);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK);++native_starts;
    volume->filesystem=STORAGE_FILESYSTEM_FAT32;
    return native_fail ? -EIO : 0;
}
static void native_destroy(struct storage_volume *volume)
{ assert(volume->fs_type.type && owner.runtime->users);++native_destroys; }
static int native_root(struct storage_volume *volume,struct storage_node *out)
{ (void)volume;*out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR};return 0; }
static int native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                         const char *name,struct storage_node *out)
{ (void)volume;(void)parent;(void)name;(void)out;return -ENOENT; }
static const struct storage_filesystem_ops native_ops={.name="ownedfs",.owner=&owner,
    .mount=native_mount,.destroy=native_destroy,.root=native_root,.lookup=native_lookup};
void *kernel_malloc(size_t bytes)
{
    if(fail_after==0)return NULL;
    if(fail_after>0)--fail_after;
    void *p=calloc(1,bytes);if(p)++allocations;return p;
}
void kernel_free(void *p) { if(p){assert(allocations);--allocations;free(p);} }
void module_memory_free(struct module_memory *memory) { assert(!memory); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ *flags=0;while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE)){} }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ (void)flags;__atomic_store_n(&lock->state,0,__ATOMIC_RELEASE); }
static int lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{ (void)context;(void)parent;(void)name;(void)out;return -ENOENT; }
static int retain(void *context,const struct storage_node *node,void **pin)
{ (void)node;assert(context==&owner);++holds;*pin=context;return 0; }
static void release(void *context,const struct storage_node *node,void *pin)
{ (void)node;assert(context==pin);++drops; }
static bool same(void *context,const struct storage_node *left,const struct storage_node *right)
{ (void)context;return left->first_cluster==right->first_cluster; }
static int finish(void *context) { assert(context==&owner);return finish_ready ? 0 : -EIO; }
static void destroy(void *context) { assert(context==&owner);++destroyed; }
static const struct vfs_backend_ops backend={.lookup=lookup,.retain=retain,.release=release,
    .same_node=same,.finish=finish,.destroy=destroy};
static int get_tree(const struct fs_mount_request *request,struct vfs_mount **out);
static struct file_system_type other={.name="other",.get_tree=get_tree};
static int get_tree(const struct fs_mount_request *request,struct vfs_mount **out)
{
    ++factory_calls;assert(request && request->flags==123);
    /* Registration from the callback must not deadlock on the registry lock. */
    assert(!register_filesystem(&other));assert(!unregister_filesystem(&other));
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=2};
    return vfs_mount_new(&backend,&owner,&owner,&root,out);
}
static struct file_system_type type={.name="ownedfs",.owner=&owner,.get_tree=get_tree,.storage=&native_ops};
static struct module_runtime *start_owner(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(runtime->name,"test_fs");strcpy(owner.name,"test_fs");
    runtime->owner=&owner;owner.runtime=runtime;module_list=runtime;
    runtime->state=MODULE_COMING;return runtime;
}
int main(void)
{
    struct fs_type_ref ref={0},second={0};
    assert(fs_type_get("missing",&ref)==-ENODEV && !ref.type);
    assert(register_filesystem(NULL)==-EINVAL);
    struct file_system_type invalid={.name="bad/name",.get_tree=get_tree};
    assert(register_filesystem(&invalid)==-EINVAL);
    invalid.name="empty";invalid.get_tree=NULL;assert(register_filesystem(&invalid)==-EINVAL);
    struct module_runtime *runtime=start_owner();
    assert(!register_filesystem(&type));assert(!runtime->users && !runtime->executing);
    assert(register_filesystem(&type)==-EBUSY);
    struct file_system_type duplicate=type;
    assert(register_filesystem(&duplicate)==-EBUSY);
    assert(fs_type_get("ownedfs",&ref)==-ENODEV && !runtime->users);
    runtime->state=MODULE_LIVE;
    assert(!fs_type_get("ownedfs",&ref) && ref.type==&type && runtime->users==1);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK);
    /* Closing name admission does not invalidate already admitted factories. */
    assert(!unregister_filesystem(&type));
    assert(fs_type_get("ownedfs",&second)==-ENODEV);
    struct fs_mount_request request={.flags=123};struct vfs_mount *mount=NULL;
    assert(!fs_type_get_tree(&ref,&request,&mount) && mount && factory_calls==1);
    fs_type_put(&ref);assert(!ref.type && runtime->users==1);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK);
    finish_ready=false;vfs_mount_put(mount);
    assert(holds==drops && !destroyed && module_unload("test_fs",0)==-EWOULDBLOCK);
    finish_ready=true;assert(vfs_reclaim_mounts(1)==1 && destroyed==1);
    assert(!module_unload("test_fs",0) && !allocations);
    assert(unregister_filesystem(&type)==-EINVAL);
    /* Construction owns a separate pin before its first parser callback. */
    runtime=start_owner();assert(!register_filesystem(&type));runtime->state=MODULE_LIVE;
    struct storage_volume volume={0};
    assert(!storage_mount_filesystem(&volume,"ownedfs") && !g_active_volume);
    assert(volume.vfs_managed);
    assert(native_starts==1 && native_destroys==0 && runtime->users==1);
    assert(module_unload("test_fs",0)==-EWOULDBLOCK);
    storage_destroy_filesystem(&volume);
    assert(!volume.fs_type.type && !volume.fs_ops && native_destroys==1 && !runtime->users);
    native_fail=true;
    assert(storage_mount_filesystem(&volume,"ownedfs")==-EIO && !g_active_volume);
    assert(!volume.fs_type.type && !volume.fs_ops && native_destroys==2 && !runtime->users);
    assert(!module_unload("test_fs",0) && !allocations);
    /* Failed COMING init still automatically removes its published resource. */
    runtime=start_owner();assert(!register_filesystem(&type));runtime->state=MODULE_GOING;
    assert(!module_unload("test_fs",0));assert(!allocations);
    assert(fs_type_get("ownedfs",&ref)==-ENODEV);
    /* Every allocation failure is reversible, including the resource ledger. */
    for(long fail=0;;++fail) {
        runtime=start_owner();fail_after=fail;int rc=register_filesystem(&type);fail_after=-1;
        runtime->state=MODULE_GOING;
        if(!rc){assert(!module_unload("test_fs",0));assert(!allocations);break;}
        assert(rc==-ENOMEM && !runtime->users && !runtime->resources);
        assert(fs_type_get("ownedfs",&ref)==-ENODEV);
        assert(!module_unload("test_fs",0) && !allocations);assert(fail<10);
    }
    /* Builtin unregistration keeps an admitted descriptor until its last put. */
    assert(!register_filesystem(&other));assert(!fs_type_get("other",&ref));
    assert(!unregister_filesystem(&other) && allocations==1);
    fs_type_put(&ref);fs_type_put(&ref);assert(!allocations);
    puts("PASS filesystem registry: COMING admission, duplicate names, no self-pin, actual mount/retirement pins, unregister with users, failed init and OOM cleanup");
}
