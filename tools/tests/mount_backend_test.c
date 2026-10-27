/* SPDX-License-Identifier: Apache-2.0 */
/* Existing whole VFS composition supplies the real tmpfs/inode adapters and
 * physical-I/O boundary stubs. Production code is independently compiled. */
#define RELIEFNT_FIXTURE_REGISTER_TYPES
#define RELIEFNT_FIXTURE_OWN_IRQ_LOCKS
#define RELIEFNT_FIXTURE_ISO_READER
#define RELIEFNT_FIXTURE_SECTOR_STATS
#define RELIEFNT_FIXTURE_REAL_DAC
#define RELIEFNT_FIXTURE_PTY_METADATA
#define RELIEFNT_FIXTURE_UNMAPPED_TRUNCATE
#define RELIEFNT_FIXTURE_SEPARATE_JOURNAL
#define RELIEFNT_FIXTURE_DEVICE_FLUSH
#define main storage_mkdir_reference_main
#include "storage_mkdir_mount_test.c"
#undef main
#include "../../fs/vfs/storage_backend.h"
#include <reliefnt/mount.h>
#include <reliefnt/exec_file.h>
#include <reliefnt/module.h>
#include <reliefnt/devfs.h>
#include <reliefnt/permissions.h>
#include <reliefnt/procfs.h>
#include <reliefnt/inventory.h>
#include "../../kernel/syscall/internal.h"
#include "../../kernel/syscall/ipc/unix_path.h"
#include "../../kernel/exec/runtime.h"
#include "../../kernel/exec/pending.h"
#include <reliefnt/kernel_file.h>
#include <linux/socket.h>
#include "../../fs/vfs/mount_internal.h"
#include "../../fs/operations.h"
#include "cluster_provider_trace.h"

/** @brief Model the absent audio registry in this filesystem-only fixture.
 * @param token Unmodelled card identity.
 * @return ENODEV; no card discovery or audio I/O is part of this fixture.
 * Native ALSA numbers are exercised by the dedicated devfs/descriptor tests.
 */
int sound_service_card_index(uint32_t token)
{(void)token;return -19;}

static const struct storage_filesystem_ops *counted_provider;
static unsigned provider_roots,provider_lookups,provider_reads,provider_directories;
static unsigned provider_stats,provider_permissions,provider_times,provider_statfs;
static unsigned provider_holds,provider_puts,provider_writes,provider_truncates;
static unsigned provider_creates,provider_removes,provider_renames,provider_links;
static int counted_root(struct storage_volume *volume,struct storage_node *out)
{ ++provider_roots;return counted_provider->root(volume,out); }
static int counted_lookup(struct storage_volume *volume,const struct storage_node *parent,
                          const char *name,struct storage_node *out)
{ ++provider_lookups;return counted_provider->lookup(volume,parent,name,out); }
static int counted_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                        void *bytes,uint32_t capacity,uint32_t *length,
                        struct storage_read_cursor *cursor,uint64_t flags)
{
    ++provider_reads;assert(flags&MS_NOATIME);
    return counted_provider->read(volume,node,offset,bytes,capacity,length,cursor,flags);
}
static int counted_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                           struct reliefos_dir_entry *entry,uint64_t flags)
{
    ++provider_directories;assert(flags&MS_NOATIME);
    return counted_provider->readdir(volume,node,cursor,entry,flags);
}
static int counted_stat(struct storage_volume *volume,const struct storage_node *node,struct linux_stat_abi *out)
{ ++provider_stats;return counted_provider->stat(volume,node,out); }
static int counted_permissions(struct storage_volume *volume,const struct storage_node *node,
                               struct reliefos_permissions *value,bool write)
{ ++provider_permissions;return counted_provider->permissions(volume,node,value,write); }
static int counted_times(struct storage_volume *volume,const struct storage_node *node,int64_t atime,int64_t mtime,
                         bool set_atime,bool set_mtime)
{ ++provider_times;return counted_provider->utimens(volume,node,atime,mtime,set_atime,set_mtime); }
static int counted_statfs(struct storage_volume *volume,struct linux_statfs_abi *out)
{ ++provider_statfs;return counted_provider->statfs(volume,out); }
static int counted_hold(struct storage_volume *volume,const struct storage_node *node)
{ ++provider_holds;return counted_provider->hold_inode(volume,node); }
static int counted_put(struct storage_volume *volume,const struct storage_node *node,bool unlinked)
{ ++provider_puts;return counted_provider->put_inode(volume,node,unlinked); }
static int counted_write(struct storage_volume *volume,struct storage_node *node,uint64_t offset,
                         const void *bytes,uint32_t length,uint32_t *written)
{ ++provider_writes;return counted_provider->write(volume,node,offset,bytes,length,written); }
static int counted_truncate(struct storage_volume *volume,struct storage_node *node,uint64_t length)
{ ++provider_truncates;return counted_provider->truncate(volume,node,length); }
static int counted_create(struct storage_volume *volume,const struct storage_node *parent,const char *name,uint32_t mode,
                          const struct reliefos_permissions *value,const char *target,struct storage_node *out)
{ ++provider_creates;return counted_provider->create(volume,parent,name,mode,value,target,out); }
static int counted_remove(struct storage_volume *volume,const struct storage_node *parent,const char *name,
                          const struct storage_node *node,bool directory)
{ ++provider_removes;return counted_provider->remove(volume,parent,name,node,directory); }
static int counted_rename(struct storage_volume *volume,const struct storage_rename_args *args)
{ ++provider_renames;return counted_provider->rename(volume,args); }
static int counted_link(struct storage_volume *volume,const struct storage_node *source,
                        const struct storage_node *parent,const char *name)
{ ++provider_links;return counted_provider->link(volume,source,parent,name); }
static void real_instance_dispatch(struct storage_volume *volume)
{
    counted_provider=volume->fs_ops;assert(counted_provider);
    struct storage_filesystem_ops traced=*counted_provider;
    traced.root=counted_root;traced.lookup=counted_lookup;
    traced.read=counted_read;traced.readdir=counted_readdir;volume->fs_ops=&traced;
    traced.stat=counted_stat;traced.permissions=counted_permissions;
    traced.utimens=counted_times;traced.statfs=counted_statfs;
    traced.hold_inode=counted_hold;traced.put_inode=counted_put;
    traced.write=counted_write;traced.truncate=counted_truncate;
    traced.create=counted_create;traced.remove=counted_remove;traced.rename=counted_rename;traced.link=counted_link;
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};struct vfs_path root={0},found={0};
    assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_mount_set_flags(mount,MS_NOATIME));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns,&root));
    assert(vfs_path_lookup_child(&root,"provider-missing",&found)==-ENOENT);
    assert(!vfs_path_create_node(&root,"provider-file",LINUX_S_IFREG,
        &(struct reliefos_permissions){0600,0,0},NULL,&found));
    uint32_t done=0;assert(!vfs_path_write(&found,0,"native",6,&done) && done==6);
    char content[16]={0};
    assert(!vfs_path_read(&found,0,content,sizeof(content),&done) && done==6 && !memcmp(content,"native",6));
    struct storage_read_cursor read_cursor={0};
    assert(!vfs_path_read_cursor(&found,1,content,2,&done,&read_cursor) && done==2 && !memcmp(content,"at",2));
    uint64_t cursor=0;struct reliefos_dir_entry entry;
    assert(vfs_path_readdir(&root,&cursor,&entry)==1);
    assert(provider_reads==2 && provider_directories==1);
    struct reliefos_permissions mode={0640,0,0};
    assert(!vfs_path_permissions(&found,&mode,true));
    assert(!vfs_path_utimens(&found,123,456,true,true));
    struct linux_stat_abi status;
    assert(!vfs_path_stat(&found,&status) && status.st_size==6 && (status.st_mode&07777)==0640);
    assert(status.atime_sec==123 && status.mtime_sec==456);
    struct linux_statfs_abi filesystem;
    assert(!vfs_path_statfs(&root,&filesystem) && filesystem.f_type==0x01021994);
    assert(provider_stats && provider_permissions && provider_times==1 && provider_statfs==1);
    assert(!vfs_path_truncate(&found,3));
    assert(!vfs_path_stat(&found,&status) && status.st_size==3);
    assert(provider_writes==1 && provider_truncates==1);
    assert(!vfs_path_link(&ns,&found,&root,"provider-hard"));
    assert(!vfs_path_rename(&ns,&found,&root,"provider-moved"));
    assert(!vfs_path_remove(&ns,&found,false));vfs_path_put(&found);
    assert(!vfs_path_lookup_child(&root,"provider-hard",&found));
    assert(!vfs_path_stat(&found,&status) && status.st_size==3 && status.st_nlink==1);
    assert(!vfs_path_remove(&ns,&found,false));vfs_path_put(&found);
    assert(provider_creates==1 && provider_removes==2 && provider_renames==1 && provider_links==1);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    assert(provider_roots==1 && provider_lookups>=1);
    assert(provider_holds && provider_holds==provider_puts);
    volume->fs_ops=counted_provider;
}

uint64_t mm_total_memory_kib(void){return 65536;}
/** @brief Check mountinfo against fixture-provided native or anonymous dev_t.
 * @param ns Actual namespace. @param root Retained root path.
 * @param type Expected filesystem type. @param flags Expected attachment policy.
 * Memory disks use their real registered major/minor; tmpfs remains anonymous.
 */
static void real_mount_description(struct vfs_namespace *ns,const struct vfs_path *root,
                                   const char *type,const char *flags)
{
    char text[1024],wanted[80];uint32_t count=0;
    assert(!vfs_read_mount_table(ns,root,true,0,text,sizeof(text)-1,&count));text[count]=0;
    uint32_t major=0,minor=1;
    if(strcmp(type,"tmpfs")) {
        const struct test_memory_disk *disk=test_memory_disk(vfs_path_context(root));
        major=1;minor=disk->registration.info.controller;
    }
    snprintf(wanted,sizeof(wanted),"1 1 %u:%u / / %s - %s ",major,minor,flags,type);
    assert(strstr(text,wanted));
}
/* This inode-only fixture creates no VMAs. Observe the real tmpfs resize
 * notification at the MM boundary; mapped truncation has separate VM tests. */
static unsigned unmapped_truncate_notifications;
void sched_truncate_file_mappings(const struct storage_node *node,uint64_t size)
{
    assert(node && (node->flags&STORAGE_NODE_FLAG_TMPFS) && size<=INT64_MAX);
    ++unmapped_truncate_notifications;
}
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{(void)node;(void)value;(void)write;return -EOPNOTSUPP;}
static uint64_t filesystem_sector_reads;
static uintptr_t lookup_user_base;
static size_t lookup_user_bytes;
bool user_range_ok(uint64_t address,uint64_t length)
{
    return address>=lookup_user_base && length<=lookup_user_bytes &&
        address-lookup_user_base<=lookup_user_bytes-length;
}
size_t user_strlen(const char *text,size_t maximum)
{
    for(size_t i=0;i<maximum;++i) {
        if(!user_range_ok((uintptr_t)text+i,1))return maximum;
        if(!text[i])return i;
    }
    return maximum;
}
static bool lookup_should_wait;
/* Physical completion boundary: the controller has already cleared PxCI,
 * but the task still owns its pending record and an actual wait-queue link. */
static struct task *lookup_queued_task;
static struct kernel_wait_queue lookup_waiters={.lock=KERNEL_SPINLOCK_INIT};
static struct ahci_hba_port lookup_completed_port;
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
void sched_block_current(void){abort();}
bool user_range_writable(uint64_t address,uint64_t length)
{return user_range_ok(address,length);}
static unsigned readlink_waits;
static struct storage_node sync_expected;
static unsigned sync_errors;
static int failed_inode_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only)
{
    assert(context && node->volume_id==sync_expected.volume_id && node->first_cluster==sync_expected.first_cluster);
    (void)filesystem;(void)data_only;++sync_errors;return -EIO;
}
static struct storage_volume *no_post_create_volume;
static uint64_t no_post_create_parent;
static const char *no_post_create_name;
static unsigned post_create_reads;
static bool fail_rename_metadata;
static unsigned fail_rename_reservation;
/* Only the deferred-final-flush test reaches a hardware cache-flush boundary.
 * No controller registers or DMA are emulated by the VFS fixture. */
static unsigned retirement_flush_calls;
static bool retirement_flush_failure;
static int retirement_read(void *context,uint64_t first,uint32_t count,void *buffer)
{(void)context;(void)first;(void)count;(void)buffer;abort();}
static int retirement_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{(void)context;(void)first;(void)count;(void)buffer;abort();}
static int retirement_flush(void *context)
{
    assert(context==&retirement_flush_calls && storage_transport_lock.state);++retirement_flush_calls;
    return retirement_flush_failure ? -EIO : 0;
}
static const struct block_device_operations retirement_operations={
    .read=retirement_read,.write=retirement_write,.flush=retirement_flush};
static struct block_device_registration retirement_registration={
    .info={.blocks=8,.block_size=512,.max_blocks=8},.operations=&retirement_operations,.context=&retirement_flush_calls};
/* A journal commit reads its recovery flag before its first physical write.
 * Emulate the real single-CPU transport's resumable read only during this test;
 * all disk-format, journal, cache and metadata code remains production code. */
static bool metadata_async_read_probe;
static unsigned metadata_commit_reads,metadata_resumable_reads;
static bool access_stat_failure;
static unsigned access_stat_failures;
int __real_storage_inode_stat(const struct storage_node *node,struct linux_stat_abi *value);
int __wrap_storage_inode_stat(const struct storage_node *node,struct linux_stat_abi *value)
{
    if(access_stat_failure){++access_stat_failures;return -EIO;}
    return __real_storage_inode_stat(node,value);
}
int __real_storage_read_device(const struct storage_volume *volume,uint64_t lba,uint32_t count,void *buffer);
int __wrap_storage_read_device(const struct storage_volume *volume,uint64_t lba,uint32_t count,void *buffer)
{
    if(metadata_async_read_probe) {
        ++metadata_commit_reads;
        if(storage_async_can_yield()){++metadata_resumable_reads;return -EAGAIN;}
    }
    return __real_storage_read_device(volume,lba,count,buffer);
}
int __real_storage_cluster_write(struct storage_node *node,uint64_t offset,const void *data,uint32_t length,uint32_t *written);
int __wrap_storage_cluster_write(struct storage_node *node,uint64_t offset,const void *data,uint32_t length,uint32_t *written)
{
    if(fail_rename_reservation && !--fail_rename_reservation){*written=0;return -ENOSPC;}
    return __real_storage_cluster_write(node,offset,data,length,written);
}
int __real_storage_backend_commit_rename_metadata(struct storage_rename_metadata *change);
int __wrap_storage_backend_commit_rename_metadata(struct storage_rename_metadata *change)
{
    if(fail_rename_metadata)return -EIO; /* Inject physical metadata-write failure. */
    int heap_budget=test_fail_heap_after,old_page_failure=test_fail_cache_alloc;
    test_fail_heap_after=0;test_fail_cache_alloc=1;
    int rc=__real_storage_backend_commit_rename_metadata(change);
    test_fail_heap_after=heap_budget;test_fail_cache_alloc=old_page_failure;
    return rc;
}
int __real_storage_ext4_read_inode(struct storage_volume *volume,uint64_t ino,struct ext4_inode_view *out);
int __wrap_storage_ext4_read_inode(struct storage_volume *volume,uint64_t ino,struct ext4_inode_view *out)
{
    uint64_t published=0;
    if(volume==no_post_create_volume && no_post_create_name &&
       !storage_ext4_dir_lookup(volume,no_post_create_parent,no_post_create_name,&published,NULL) && published==ino) {
        ++post_create_reads;return -ENOMEM;
    }
    return __real_storage_ext4_read_inode(volume,ino,out);
}
static int (*native_readlink)(void *,const struct storage_node *,char *,size_t,size_t *);
static int readlink_with_wait(void *context,const struct storage_node *node,char *buffer,size_t capacity,size_t *length)
{
    if(readlink_waits){--readlink_waits;*length=0;return -EAGAIN;}
    return native_readlink(context,node,buffer,capacity,length);
}
static void lookup_cancel(void *context,uint32_t pid)
{assert(context==&lookup_completed_port);ahci_cancel_task(pid);}
static const struct block_device_operations lookup_block_ops={.read=retirement_read,.cancel=lookup_cancel};
static const struct block_device_registration lookup_block_registration={
    .info={.device=0x123400,.blocks=8,.block_size=512,.max_blocks=1,.flags=BLOCK_DEVICE_READ_ONLY},
    .operations=&lookup_block_ops,.context=&lookup_completed_port};
static int lookup_with_wait(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    if(lookup_should_wait) {
        lookup_should_wait=false;
        if(lookup_queued_task) {
            assert(!kernel_wait_queue_add(&lookup_waiters,lookup_queued_task));
            storage_task_io_owner=lookup_queued_task->pid;
            ahci_pending_command=(struct ahci_pending_command){.active=1,
                .owner_pid=lookup_queued_task->pid,.port=&lookup_completed_port};
        }
        return -EAGAIN;
    }
    return storage_backend_lookup(context,parent,name,out);
}
int storage_read_sectors(uint64_t lba,uint32_t sectors,void *buffer)
{
    ++filesystem_sector_reads;
    return storage_reference_hardware_sectors(lba,sectors,buffer);
}

/* Replace only the optical block transport, preserving the ISO parser,
 * inode table, namespace walker and file read implementation. */
static const uint8_t *iso_image;
static size_t iso_image_size;
int storage_read_iso_blocks(uint64_t lba,uint32_t blocks,void *buffer)
{
    if(lba>SIZE_MAX/2048 || (size_t)blocks>SIZE_MAX/2048)return -5;
    size_t offset=(size_t)lba*2048,length=(size_t)blocks*2048;
    if(offset>iso_image_size || length>iso_image_size-offset)return -5;
    memcpy(buffer,iso_image+offset,length);return 0;
}
/* Exercise the production initial attachment with the real optical parser.
 * The image owns /media but deliberately has no numbered optical directory. */
static void initial_optical_namespace(void)
{
    assert(!storage_inode_purge_volume(0));
    g_volumes[3]=g_volumes[0];g_volumes[3].volume_id=3;

    g_volumes[0]=(struct storage_volume){0};
    const unsigned slots[]={0,4,5};
    for(size_t i=0;i<sizeof(slots)/sizeof(slots[0]);++i) {
        unsigned slot=slots[i];struct tmpfs_super *fs=NULL;
        assert(!tmpfs_new(slot,0,"size=0,nr_inodes=0",0,0,&fs));
        g_volumes[slot]=(struct storage_volume){.volume_id=slot,.kind=STORAGE_VOLUME_ANONYMOUS,
            .ready=true,.filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.tmpfs=fs};
    }
    const char *directories[]={"/dev","/proc","/sys","/media"};
    for(size_t i=0;i<sizeof(directories)/sizeof(directories[0]);++i)
        assert(!tmpfs_create(g_volumes[0].tmpfs,directories[i],LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!storage_namespace_bootstrap());
    struct vfs_namespace *ns=vfs_system_namespace();assert(ns);
    struct vfs_path root={0},optical={0};assert(!vfs_namespace_root(ns,&root));
    assert(!vfs_path_lookup(ns,&root,&root,"/media/cdrom0",0,&optical));
    assert(vfs_path_context(&optical)==&g_volumes[3]);
    assert(vfs_path_mount_flags(&optical)&MS_RDONLY);
    const void *data=NULL;size_t length=0;
    assert(!vfs_kernel_read_file(ns,"/media/cdrom0/demo.ko",&data,&length));
    assert(length==28 && !memcmp(data,"native module snapshot data!",28));
    vfs_kernel_file_free(data,length);vfs_path_put(&optical);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(ns));
    for(unsigned slot=0;slot<STORAGE_MAX_VOLUMES;++slot) {
        assert(!storage_inode_volume_busy(slot));assert(!storage_inode_purge_volume(slot));
        if(g_volumes[slot].tmpfs)tmpfs_destroy(g_volumes[slot].tmpfs);
        g_volumes[slot]=(struct storage_volume){0};
    }
    puts("PASS initial optical attachment: real ISO, absent numbered target, explicit role and readonly exact payload");
}
static void iso_filesystem(const char *input)
{
    FILE *file=fopen(input,"rb");assert(file && !fseek(file,0,SEEK_END));
    long bytes=ftell(file);assert(bytes>0 && !fseek(file,0,SEEK_SET));
    uint8_t *image=malloc((size_t)bytes);assert(image);
    assert(fread(image,1,(size_t)bytes,file)==(size_t)bytes && !fclose(file));
    iso_image=image;iso_image_size=(size_t)bytes;
    struct storage_volume *volume=&g_volumes[0];
    *volume=(struct storage_volume){.volume_id=0};test_memory_bind(volume,image,(uint64_t)bytes);g_active_volume=volume;
    assert(!iso9660_mount());volume->fs_ops=&storage_iso_ops;volume->ready=true;
    struct storage_node value;memset(&value,0xa5,sizeof(value));
    assert(!iso9660_find_in_dir(volume->iso_root_extent,volume->iso_root_size,"demo.ko",&value));
    assert(!value.mount_generation && !value.inode_generation);
    struct storage_inode_ref *pin=NULL;
    assert(!storage_inode_get(&value,&pin) && pin);
    assert(storage_inode_volume_busy(0));
    assert(storage_inode_write_get(pin)==-EROFS);
    assert(!storage_inode_deny_write(pin));storage_inode_allow_write(pin);
    assert(!storage_inode_put(pin) && !storage_inode_volume_busy(0));
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};
    assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},held={0};assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"demo.ko",VFS_LOOKUP_FOLLOW,&held));
    real_mount_description(&ns,&root,"iso9660","ro");
    char content[64]={0};uint32_t got=0;
    assert(!vfs_path_read(&held,0,content,sizeof(content),&got));
    assert(got==28 && !memcmp(content,"native module snapshot data!",28));
    const void *copy=NULL;size_t length=0;
    assert(!vfs_kernel_read_file(&ns,"/demo.ko",&copy,&length) && length==28 && !memcmp(copy,content,28));
    vfs_kernel_file_free(copy,length);
    vfs_path_put(&held);vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    assert(!storage_inode_volume_busy(0));
    assert(test_memory_detach(volume)==image);initial_optical_namespace();free(image);iso_image=NULL;iso_image_size=0;
    puts("PASS real ISO9660 namespace/inode pins: initialized node identity, readonly writer rejection and exact file payload");
}

static void writer_leases(const struct storage_node *node)
{
    struct storage_inode_ref *first=NULL,*alias=NULL;
    assert(!storage_inode_get(node,&first) && first);
    assert(!storage_inode_get(node,&alias) && alias==first);
    assert(!storage_inode_write_get(first) && !storage_inode_write_get(alias));
    assert(storage_inode_deny_write(alias)==-ETXTBSY);
    storage_inode_write_put(first);
    assert(storage_inode_deny_write(first)==-ETXTBSY);
    storage_inode_write_put(alias);
    assert(!storage_inode_deny_write(first) && !storage_inode_deny_write(alias));
    assert(storage_inode_write_get(first)==-ETXTBSY);
    storage_inode_allow_write(first);
    assert(storage_inode_write_get(alias)==-ETXTBSY);
    storage_inode_allow_write(alias);
    assert(!storage_inode_write_get(first));storage_inode_write_put(first);
    assert(!storage_inode_put(first) && !storage_inode_put(alias));
}

/* Characterize a generated/captured provider at its documented callback
 * boundary while all actual path/inode/FS implementations remain linked. */
static const struct vfs_backend_ops *kernel_read_native;
static unsigned kernel_read_opens,kernel_read_closes,kernel_read_calls;
static unsigned kernel_read_error_after;
static bool kernel_read_short_eof;
static unsigned kernel_read_size_override;
static char kernel_read_capture_token;
static int kernel_capture_open(void *context,const struct storage_node *node,uint32_t flags,void **out)
{
    assert(context && node && flags==LINUX_O_RDONLY && !storage_io_async_context);
    ++kernel_read_opens;*out=&kernel_read_capture_token;return 0;
}
static int kernel_capture_read(void *context,const struct storage_node *node,void *state,
                                uint64_t offset,void *buffer,uint32_t capacity,uint32_t *got,uint64_t flags)
{
    assert(state==&kernel_read_capture_token && !storage_io_async_context);++kernel_read_calls;
    *got=0;if(kernel_read_error_after && kernel_read_calls>=kernel_read_error_after)return -EIO;
    if(kernel_read_short_eof && kernel_read_calls>1)return 0;
    return kernel_read_native->read(context,node,offset,buffer,capacity>2 ? 2 : capacity,got,flags);
}
static void kernel_capture_close(void *context,const struct storage_node *node,void *state)
{
    assert(context && node && state==&kernel_read_capture_token && !storage_io_async_context);++kernel_read_closes;
}
static int kernel_capture_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *value)
{
    int rc=kernel_read_native->getattr(context,path,value);
    if(!rc && kernel_read_size_override)value->st_size=kernel_read_size_override==1 ? UINT64_C(0x100000004) : INT64_MAX;
    return rc;
}
static void kernel_mutation_objects(const struct vfs_path *directory)
{
    struct vfs_namespace *ns=directory->mount->ns;struct vfs_path root={0},child={0},file={0};
    char parent[512],name[560],data_name[576];assert(!vfs_namespace_root(ns,&root));
    assert(!vfs_path_format(ns,&root,directory,parent,sizeof(parent)));
    snprintf(name,sizeof(name),"%s/kernel-controls",parent);snprintf(data_name,sizeof(data_name),"%s/data",name);
    assert(!vfs_kernel_mkdir(ns,name,0750) && vfs_kernel_mkdir(ns,name,0750)==-EEXIST);
    storage_io_async_context=true;storage_io_write_started=false;
    assert(!vfs_kernel_write_file(ns,data_name,"control",7));
    assert(storage_io_async_context && storage_io_write_started);
    storage_io_async_context=false;storage_io_write_started=false;
    assert(!vfs_path_lookup(ns,&root,&root,name,0,&child));
    assert(!vfs_path_lookup(ns,&root,&root,data_name,0,&file));
    struct reliefos_permissions mode={0700,2345,3456},got;
    assert(!vfs_path_permissions(&file,&mode,true));
    assert(!vfs_kernel_write_file(ns,data_name,"kept",4));
    assert(!vfs_path_permissions(&file,&got,false) && !memcmp(&mode,&got,sizeof(mode)));
    const void *data=NULL;size_t length=0;
    assert(!vfs_kernel_read_file(ns,data_name,&data,&length) && length==4 && !memcmp(data,"kept",4));vfs_kernel_file_free(data,length);
    struct storage_inode_ref *pin=NULL;assert(!storage_inode_get(vfs_path_node(&file),&pin) && pin);
    assert(!storage_inode_deny_write(pin));
    assert(vfs_kernel_write_file(ns,data_name,"denied",6)==-ETXTBSY);
    storage_inode_allow_write(pin);assert(!storage_inode_put(pin));
    assert(!vfs_kernel_read_file(ns,data_name,&data,&length) && length==4 && !memcmp(data,"kept",4));vfs_kernel_file_free(data,length);
    uint64_t flags=vfs_path_mount_flags(directory);
    assert(!vfs_path_remount(directory,flags|MS_RDONLY));
    assert(vfs_kernel_write_file(ns,data_name,"denied",6)==-EROFS);
    assert(vfs_kernel_unlink(ns,data_name)==-EROFS);
    assert(!vfs_path_remount(directory,flags));
    assert(!vfs_kernel_write_file(ns,data_name,NULL,0));
    assert(!vfs_kernel_read_file(ns,data_name,&data,&length) && data && !length);vfs_kernel_file_free(data,length);
    assert(!vfs_kernel_unlink(ns,data_name) && vfs_kernel_unlink(ns,data_name)==-ENOENT);
    vfs_path_put(&file);assert(!vfs_path_remove(ns,&child,true));vfs_path_put(&child);vfs_path_put(&root);
}
static void kernel_read_objects(const struct vfs_path *file,bool visible)
{
    struct vfs_namespace *ns=file->mount->ns;struct vfs_path root={0};char name[512],bytes[8]={0};
    assert(!vfs_namespace_root(ns,&root) && !vfs_path_format(ns,&root,file,name,sizeof(name)));
    const void *data=(void *)1;size_t length=99;uint32_t got=99;
    if(!visible) {
        assert(vfs_kernel_read_file(ns,name,&data,&length)==-ENOENT && !data && !length);
        vfs_path_put(&root);return; /* Held covered inode cannot replace namespace lookup. */
    }
    assert(!vfs_kernel_read_file(ns,name,&data,&length) && data && length==4 && !memcmp(data,"data",4));
    vfs_kernel_file_free(data,length);
    assert(!vfs_kernel_read(ns,name,1,bytes,2,&got) && got==2 && !memcmp(bytes,"at",2));
    assert(!vfs_kernel_read(ns,name,4,bytes,sizeof(bytes),&got) && !got);
    assert(vfs_kernel_read_file(NULL,name,&data,&length)==-ENODEV && !data && !length);
    assert(vfs_kernel_read_file(ns,"relative",&data,&length)==-EINVAL && !data && !length);
    assert(vfs_kernel_read_file(ns,"/",&data,&length)==-EISDIR && !data && !length);
    assert(vfs_kernel_read(ns,name,0,NULL,1,&got)==-EINVAL && !got);
    kernel_read_native=file->mount->ops;struct vfs_backend_ops capture=*kernel_read_native;
    capture.file_open=kernel_capture_open;capture.file_read=kernel_capture_read;capture.file_close=kernel_capture_close;
    capture.getattr=kernel_capture_stat;file->mount->ops=&capture;
    unsigned passed=0;
    for(int budget=0;budget<18;++budget) {
        uint64_t refs=file->mount->references;unsigned opens=kernel_read_opens,closes=kernel_read_closes;
        storage_io_async_context=true;storage_io_write_started=budget&1;kernel_read_calls=0;test_fail_heap_after=budget;
        int rc=vfs_kernel_read_file(ns,name,&data,&length);test_fail_heap_after=-1;
        assert(storage_io_async_context && storage_io_write_started==(bool)(budget&1));
        storage_io_async_context=false;storage_io_write_started=false;
        assert(kernel_read_opens-opens==kernel_read_closes-closes && file->mount->references==refs);
        if(rc)assert(rc==-ENOMEM && !data && !length);
        else {assert(data && length==4 && !memcmp(data,"data",4) && kernel_read_calls==2);++passed;vfs_kernel_file_free(data,length);}
    }
    assert(passed);
    for(unsigned mode=0;mode<2;++mode) {
        kernel_read_calls=0;kernel_read_error_after=mode ? 0 : 2;kernel_read_short_eof=mode;
        assert(vfs_kernel_read_file(ns,name,&data,&length)==-EIO && !data && !length);
        assert(kernel_read_calls==2 && kernel_read_opens==kernel_read_closes);
    }
    kernel_read_error_after=0;kernel_read_short_eof=false;
    kernel_read_size_override=1;test_fail_cache_alloc=true;
    assert(vfs_kernel_read_file(ns,name,&data,&length)==-ENOMEM && !data && !length);
    kernel_read_size_override=2;
    assert(vfs_kernel_read_file(ns,name,&data,&length)==-EFBIG && !data && !length);
    kernel_read_size_override=0;test_fail_cache_alloc=false;
    assert(kernel_read_opens==kernel_read_closes);
    file->mount->ops=kernel_read_native;vfs_path_put(&root);
}
static int search_held_directory(void *context,const struct vfs_path *path)
{return fs_permissions_check_path(context,path,FS_ACCESS_EXEC,false);}
static void create_held_file(const struct vfs_path *directory,bool visible)
{
    if(vfs_path_node(directory)->flags&STORAGE_NODE_FLAG_TMPFS) {
        struct reliefos_permissions atomic_mode={0600,1901,2901};
        for(int budget=0;budget<12;++budget) {
            struct vfs_path published={0},lookup={0};
            test_fail_heap_after=budget;
            int rc=vfs_path_create_node(directory,"atomic-endpoint",LINUX_S_IFSOCK,&atomic_mode,NULL,&published);
            test_fail_heap_after=-1;
            if(rc) {
                assert(rc==-ENOMEM && !published.mount);
                assert(vfs_path_lookup_child(directory,"atomic-endpoint",&lookup)==-ENOENT);
            } else {
                assert(!vfs_path_remove(directory->mount->ns,&published,false));vfs_path_put(&published);
            }
        }
    }
    if(vfs_path_node(directory)->flags&STORAGE_NODE_FLAG_EXT2) {
        no_post_create_volume=&g_volumes[vfs_path_node(directory)->volume_id];
        no_post_create_parent=vfs_path_node(directory)->first_cluster;no_post_create_name="commit-result";
        struct reliefos_permissions committed={0750,1901,2901};struct vfs_path result={0};
        assert(!vfs_path_create_node(directory,no_post_create_name,LINUX_S_IFDIR,&committed,NULL,&result));
        assert(!post_create_reads);
        no_post_create_volume=NULL;no_post_create_name=NULL;
        assert(!vfs_path_remove(result.mount->ns,&result,true));vfs_path_put(&result);
    }
    struct reliefos_permissions mode={0670,1901,2901},got;
    struct vfs_path file={0},collision={0};
    const struct vfs_backend_ops *native=directory->mount->ops;
    struct vfs_backend_ops waiting=*native;waiting.lookup=lookup_with_wait;
    directory->mount->ops=&waiting;lookup_should_wait=true;
    assert(!vfs_path_new_file(directory,"direct-new",&mode,&file));
    assert(lookup_should_wait);lookup_should_wait=false;directory->mount->ops=native;
    assert(vfs_path_node(&file)->type==RELIEFOS_FS_TYPE_FILE && !vfs_path_node(&file)->size);
    assert(!vfs_path_permissions(&file,&got,false) && !memcmp(&mode,&got,sizeof(got)));
    if(visible) {
        kernel_mutation_objects(directory);
        struct vfs_path root={0};char name[512];const void *empty=NULL;size_t length=99;
        assert(!vfs_namespace_root(directory->mount->ns,&root));
        assert(!vfs_path_format(directory->mount->ns,&root,&file,name,sizeof(name)));
        assert(!vfs_kernel_read_file(directory->mount->ns,name,&empty,&length) && empty && !length);
        vfs_kernel_file_free(empty,length);vfs_path_put(&root);
    }
    struct linux_stat_abi before,after;struct linux_statfs_abi filesystem;
    assert(!vfs_path_stat(&file,&before) && !before.st_size && before.st_ino &&
        (before.st_mode&07777)==0670 && before.st_uid==1901 && before.st_gid==2901);
    uint32_t count=0;
    assert(!vfs_path_write(&file,0,"data",4,&count) && count==4);
    kernel_read_objects(&file,visible);
    assert(!vfs_path_stat(&file,&after) && after.st_size==4 && after.st_ino==before.st_ino);
    unsigned notifications=unmapped_truncate_notifications;
    assert(!vfs_path_truncate(&file,2));
    if(vfs_path_node(&file)->flags&STORAGE_NODE_FLAG_TMPFS)
        assert(unmapped_truncate_notifications==notifications+1);
    assert(!vfs_path_stat(&file,&after) && after.st_size==2 && after.st_ino==before.st_ino);
    /* A different inode handle extends the file without refreshing this
     * path's cached node. The executable source needs the real size/data. */
    struct storage_node alias=*vfs_path_node(&file);uint32_t appended=0;
    assert(!storage_write_held_node(&alias,2,"tail",4,&appended) && appended==4);
    struct vfs_exec_source *image=NULL;
    assert(!vfs_exec_source_open(&file,&image) && image);
    assert(vfs_exec_source_node(image)->size==6);
    assert(storage_inode_write_get(file.dentry->backend_pin)==-ETXTBSY);
    struct vfs_exec_source *retained_image=vfs_exec_source_get(image);vfs_exec_source_put(image);
    char image_bytes[16]={0};uint32_t image_read=0;
    assert(!vfs_exec_source_read(retained_image,0,image_bytes,sizeof(image_bytes),&image_read) &&
        image_read==6 && !memcmp(image_bytes,"datail",6));
    vfs_exec_source_put(retained_image);
    struct storage_read_cursor cursor={0};char sequential[8]={0};uint32_t sequential_read=0;
    assert(!vfs_path_read_cursor(&file,0,sequential,2,&sequential_read,&cursor) && sequential_read==2);
    assert(!vfs_path_read_cursor(&file,2,sequential+2,4,&sequential_read,&cursor) && sequential_read==4 &&
        !memcmp(sequential,"datail",6));
    struct vfs_backend_ops fallback=*native;fallback.read_cursor=NULL;
    directory->mount->ops=&fallback;cursor.valid=1;
    assert(!vfs_path_read_cursor(&file,0,sequential,sizeof(sequential),&sequential_read,&cursor) &&
        sequential_read==6 && !cursor.valid && !memcmp(sequential,"datail",6));
    directory->mount->ops=native;
    assert(!storage_inode_write_get(file.dentry->backend_pin));
    image=(void *)1;
    assert(vfs_exec_source_open(&file,&image)==-ETXTBSY && !image);
    storage_inode_write_put(file.dentry->backend_pin);
    assert(!vfs_path_truncate(&file,2));
    assert(!vfs_path_fallocate(&file,0,3,8190));
    assert(!vfs_path_stat(&file,&after) && after.st_size==8193);
    assert(!vfs_path_sync(&file,false,false));
    assert(!vfs_path_sync(&file,true,false));
    assert(!vfs_path_statfs(&file,&filesystem) && filesystem.f_type && filesystem.f_bsize>0);
    if(vfs_path_node(&file)->flags&(STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_EXT2)) {
        assert(!vfs_path_utimens(&file,1700000101,1700000202,true,true));
        assert(!vfs_path_stat(&file,&after) && after.atime_sec==1700000101 && after.mtime_sec==1700000202);
        assert(!vfs_path_utimens(&file,0,1700000303,false,true));
        assert(!vfs_path_stat(&file,&after) && after.atime_sec==1700000101 && after.mtime_sec==1700000303);
    }
    mode.mode=0000;
    assert(vfs_path_new_file(directory,"direct-new",&mode,&collision)==-EEXIST);
    assert(!collision.mount && !collision.dentry);
    assert(!vfs_path_permissions(&file,&got,false) && got.mode==0670);
    assert(vfs_path_new_file(directory,"../escape",&mode,&collision)==-EINVAL);
    assert(vfs_path_new_file(directory,"..",&mode,&collision)==-EINVAL);
    assert(vfs_path_new_file(directory,"",&mode,&collision)==-EINVAL);
    struct vfs_path replaced={0};struct reliefos_permissions replaced_mode={0644,1801,2801};
    assert(!vfs_path_new_file(directory,"rename-target",&replaced_mode,&replaced));
    assert(!vfs_path_write(&replaced,0,"target",6,&count) && count==6);
    for(int budget=0;budget<2;++budget) {
        test_fail_heap_after=budget;
        int failure=vfs_path_rename(directory->mount->ns,&file,directory,"rename-target");
        test_fail_heap_after=-1;assert(failure==-ENOMEM);
        assert(!strcmp(vfs_path_name(&file),"direct-new"));
        assert(!vfs_path_stat(&replaced,&after) && after.st_nlink && after.st_size==6);
    }
    assert(!vfs_path_rename(directory->mount->ns,&file,directory,"rename-target"));
    assert(!strcmp(vfs_path_name(&file),"rename-target"));
    assert(vfs_path_lookup_child(directory,"direct-new",&collision)==-ENOENT);
    assert(!vfs_path_stat(&replaced,&after) && !after.st_nlink && after.st_size==6);
    char replaced_bytes[8]={0};uint32_t replaced_count=0;
    assert(!vfs_path_read(&replaced,0,replaced_bytes,sizeof(replaced_bytes),&replaced_count) &&
        replaced_count==6 && !memcmp(replaced_bytes,"target",6));
    vfs_path_put(&replaced);
    assert(!vfs_path_rename(directory->mount->ns,&file,directory,"direct-new"));
    if(vfs_path_node(directory)->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
        test_fail_cache_alloc=1;
        int allocation_failure=vfs_path_rename(directory->mount->ns,&file,directory,"allocation-error");
        test_fail_cache_alloc=0;
        assert(allocation_failure==-ENOMEM && !strcmp(vfs_path_name(&file),"direct-new"));
        assert(vfs_path_lookup_child(directory,"allocation-error",&collision)==-ENOENT);
        fail_rename_metadata=true;
        int fault=vfs_path_rename(directory->mount->ns,&file,directory,"metadata-error");
        fail_rename_metadata=false;
        assert(fault==-EIO && !strcmp(vfs_path_name(&file),"metadata-error"));
        assert(vfs_path_lookup_child(directory,"direct-new",&collision)==-ENOENT);
        assert(!vfs_path_rename(directory->mount->ns,&file,directory,"direct-new"));
        ((struct storage_inode_ref *)file.dentry->backend_pin)->permissions_valid=false;
    }
    struct vfs_path reopened={0};assert(!vfs_path_lookup_child(directory,"direct-new",&reopened));
    assert(!vfs_path_permissions(&reopened,&got,false) && got.mode==0670 && got.uid==1901 && got.gid==2901);
    vfs_path_put(&reopened);
    if(vfs_path_node(directory)->flags&(STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_EXT2)) {
        assert(!vfs_path_link(directory->mount->ns,&file,directory,"hard-alias"));
        struct vfs_path alias={0};assert(!vfs_path_lookup_child(directory,"hard-alias",&alias));
        assert(vfs_path_same_inode(&alias,&file) && !vfs_path_stat(&alias,&after) && after.st_nlink==2);
        assert(!vfs_path_rename(directory->mount->ns,&alias,directory,"direct-new"));
        assert(!strcmp(vfs_path_name(&alias),"hard-alias") && !vfs_path_stat(&file,&after) && after.st_nlink==2);
        assert(!vfs_path_remove(directory->mount->ns,&alias,false));vfs_path_put(&alias);
        assert(!vfs_path_stat(&file,&after) && after.st_nlink==1);
    } else assert(vfs_path_link(directory->mount->ns,&file,directory,"hard-alias")==-EOPNOTSUPP);
    mode=(struct reliefos_permissions){0600,1901,2901};
    assert(!vfs_path_create_node(directory,"create-only",LINUX_S_IFREG,&mode,NULL,NULL));
    struct vfs_path only={0};
    assert(!vfs_path_lookup_child(directory,"create-only",&only));
    assert(!vfs_path_stat(&only,&after) && after.st_uid==1901 && after.st_gid==2901 && (after.st_mode&07777)==0600);
    assert(!vfs_path_remove(only.mount->ns,&only,false));vfs_path_put(&only);
    struct vfs_path child={0},nested={0},link={0};
    mode=(struct reliefos_permissions){02750,1901,2901};
    assert(!vfs_path_create_node(directory,"direct-directory",LINUX_S_IFDIR,&mode,NULL,&child));
    assert(vfs_path_link(directory->mount->ns,&child,directory,"directory-alias")==-EPERM);
    assert(!vfs_path_stat(&child,&after) && (after.st_mode&LINUX_S_IFMT)==LINUX_S_IFDIR &&
        (after.st_mode&07777)==02750 && after.st_uid==1901 && after.st_gid==2901);
    mode.mode=0600;
    assert(!vfs_path_create_node(&child,"nested",LINUX_S_IFREG,&mode,NULL,&nested));
    assert(!vfs_path_rename(directory->mount->ns,&nested,directory,"cross-file"));
    assert(!vfs_path_lookup_child(directory,"cross-file",&reopened));
    if(vfs_path_node(directory)->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT))
        ((struct storage_inode_ref *)reopened.dentry->backend_pin)->permissions_valid=false;
    assert(!vfs_path_permissions(&reopened,&got,false) && got.mode==0600 && got.uid==1901 && got.gid==2901);
    vfs_path_put(&reopened);
    assert(vfs_path_lookup_child(&child,"nested",&collision)==-ENOENT);
    assert(!vfs_path_rename(directory->mount->ns,&nested,&child,"nested"));
    for(unsigned i=0;i<50;++i) {
        char member[32];snprintf(member,sizeof(member),"fanout-%02u",i);
        int creation=vfs_path_create_node(&child,member,LINUX_S_IFREG,&mode,NULL,NULL);
        if(creation)fprintf(stderr,"grown directory create index=%u flags=%#x ino=%u generation=%u size=%llu entry=%#llx rc=%d\n",
            i,vfs_path_node(&child)->flags,vfs_path_node(&child)->first_cluster,vfs_path_node(&child)->inode_generation,
            (unsigned long long)vfs_path_node(&child)->size,(unsigned long long)vfs_path_node(&child)->entry_id,creation);
        assert(!creation);
    }
    struct vfs_path sibling={0};struct reliefos_permissions sibling_mode={02770,1902,2902};
    assert(!vfs_path_create_node(directory,"cross-parent",LINUX_S_IFDIR,&sibling_mode,NULL,&sibling));
    if(vfs_path_node(directory)->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
        fail_rename_reservation=2;
        int space=vfs_path_rename(directory->mount->ns,&child,&sibling,"moved-child");
        fail_rename_reservation=0;
        assert(space==-ENOSPC && !strcmp(vfs_path_name(&child),"direct-directory"));
        assert(vfs_path_lookup_child(&sibling,"LEONACL.SYS",&collision)==-ENOENT);
        assert(vfs_path_lookup_child(&sibling,"moved-child",&collision)==-ENOENT);
    }
    assert(!vfs_path_rename(directory->mount->ns,&child,&sibling,"moved-child"));
    assert(!vfs_path_permissions(&child,&got,false) && got.mode==02750 && got.uid==1901 && got.gid==2901);
    struct vfs_path after_move={0};
    assert(!vfs_path_new_file(&child,"after-move",&mode,&after_move));
    assert(!vfs_path_remove(directory->mount->ns,&after_move,false));vfs_path_put(&after_move);
    assert(!vfs_path_rename(directory->mount->ns,&child,directory,"direct-directory"));
    assert(!vfs_path_remove(directory->mount->ns,&sibling,true));vfs_path_put(&sibling);
    for(unsigned i=0;i<50;++i) {
        char member[32];snprintf(member,sizeof(member),"fanout-%02u",i);
        struct vfs_path old_member={0};assert(!vfs_path_lookup_child(&child,member,&old_member));
        assert(!vfs_path_remove(directory->mount->ns,&old_member,false));vfs_path_put(&old_member);
    }
    if(vfs_path_node(directory)->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
        uint64_t reserved_size=0;
        struct storage_volume *volume=&g_volumes[vfs_path_node(directory)->volume_id];
        for(unsigned i=0;i<8;++i) {
            char member[32];snprintf(member,sizeof(member),"foreign-%u",i);
            struct storage_node raw;
            /* A foreign FAT file has no ReliefNT permission record yet. */
            assert(!storage_cluster_new_file(volume,vfs_path_node(&child),member,&raw));
            struct vfs_path foreign={0},sidecar={0};
            assert(!vfs_path_lookup_child(&child,member,&foreign));
            assert(!vfs_path_rename(directory->mount->ns,&foreign,directory,member));
            assert(!vfs_path_lookup_child(&child,"LEONACL.SYS",&sidecar));
            assert(!vfs_path_stat(&sidecar,&after));
            if(!i)reserved_size=after.st_size;
            else assert((uint64_t)after.st_size==reserved_size);
            vfs_path_put(&sidecar);
            assert(!vfs_path_remove(directory->mount->ns,&foreign,false));vfs_path_put(&foreign);
        }
    }
    assert(vfs_path_rename(directory->mount->ns,&child,&child,"cycle")==-EINVAL);
    assert(!vfs_path_rename(directory->mount->ns,&child,directory,"renamed-directory"));
    assert(!strcmp(vfs_path_name(&child),"renamed-directory"));
    char intrinsic_name[VFS_PATH_MAX];
    assert(!vfs_path_backend_name(&nested,intrinsic_name,sizeof(intrinsic_name)) && strstr(intrinsic_name,"/renamed-directory/nested"));
    assert(!vfs_path_rename(directory->mount->ns,&child,directory,"direct-directory"));
    if(vfs_path_node(&child)->flags&(STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_EXT2)) {
        mode.mode=0777;
        assert(!vfs_path_create_node(&child,"relative",LINUX_S_IFLNK,&mode,"nested",&link));
        char target[16]={0};size_t bytes=0;
        assert(!vfs_path_readlink(&link,target,sizeof(target),&bytes) && bytes==6 && !memcmp(target,"nested",6));
        assert(!vfs_path_remove(child.mount->ns,&link,false));
        vfs_path_put(&link);
    }
    assert(vfs_path_remove(child.mount->ns,&child,true)==-ENOTEMPTY);
    assert(vfs_path_remove(child.mount->ns,&child,false)==-EISDIR);
    assert(vfs_path_remove(child.mount->ns,&nested,true)==-ENOTDIR);
    assert(!vfs_path_remove(child.mount->ns,&nested,false));
    assert(!vfs_path_stat(&nested,&after) && !after.st_nlink);
    if(vfs_path_node(&child)->flags&STORAGE_NODE_FLAG_FAT32) {
        struct storage_volume *previous=NULL;
        assert(!storage_select_node_volume(vfs_path_node(&child),&previous));
        struct fat32_dir_ref ref;
        assert(!fat32_find_dirent_ref_in_dir(vfs_path_node(&child)->first_cluster,"LEONACL.SYS",&ref));
        assert(ref.entry_offset>=32 && !fat32_read_cluster(ref.entry_cluster,storage_cluster_buf));
        struct fat32_lfn *lfn=(void *)(storage_cluster_buf+ref.entry_offset-32);
        assert(lfn->attr==FAT32_ATTR_LFN && lfn->order==0x41);
        uint8_t checksum=lfn->checksum;lfn->checksum^=1;
        assert(!fat32_write_cluster(ref.entry_cluster,storage_cluster_buf));storage_cache_invalidate();
        assert(vfs_path_remove(child.mount->ns,&child,true)==-ENOTEMPTY);
        assert(!fat32_read_cluster(ref.entry_cluster,storage_cluster_buf));
        lfn=(void *)(storage_cluster_buf+ref.entry_offset-32);lfn->checksum=checksum;
        assert(!fat32_write_cluster(ref.entry_cluster,storage_cluster_buf));storage_cache_invalidate();
        storage_restore_volume(previous);
    }
    struct vfs_path metadata={0};char metadata_before[128],metadata_after[128];uint32_t metadata_bytes=0;
    if(vfs_path_node(&child)->flags&(STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT)) {
        assert(!vfs_path_lookup_child(&child,"LEONACL.SYS",&metadata));
        assert(!vfs_path_read(&metadata,0,metadata_before,sizeof(metadata_before),&metadata_bytes) && metadata_bytes);
    }
    int removed_directory=vfs_path_remove(child.mount->ns,&child,true);
    if(removed_directory)fprintf(stderr,"held rmdir failed volume=%u flags=%#x inode=%u generation=%llu rc=%d\n",
        vfs_path_node(&child)->volume_id,vfs_path_node(&child)->flags,vfs_path_node(&child)->first_cluster,
        (unsigned long long)vfs_path_node(&child)->inode_generation,removed_directory);
    assert(!removed_directory);
    if(metadata.mount) {
        uint32_t bytes=0;
        assert(!vfs_path_read(&metadata,0,metadata_after,sizeof(metadata_after),&bytes) && bytes==metadata_bytes &&
            !memcmp(metadata_before,metadata_after,bytes));
        assert(!vfs_path_stat(&metadata,&after) && !after.st_nlink);
        vfs_path_put(&metadata);
    }
    assert(!vfs_path_permissions(&child,&got,false) && got.uid==1901 && got.gid==2901 && got.mode==02750);
    assert(vfs_path_remove(child.mount->ns,&child,true)==-ENOENT);
    struct vfs_path orphan={0};
    assert(vfs_path_new_file(&child,"after-rmdir",&mode,&orphan)==-ENOENT && !orphan.mount);
    char gone[VFS_PATH_MAX];
    assert(vfs_path_getcwd(child.mount->ns,directory,&child,gone,sizeof(gone))==-ENOENT);
    vfs_path_put(&nested);vfs_path_put(&child);
    vfs_path_put(&file);
}
static void held_permissions(struct vfs_namespace *ns,const struct vfs_path *root,
                                struct tmpfs_super *fs,const struct vfs_path *file,
                                const struct vfs_path *directory)
{
    static struct task actor;
    actor=(struct task){.uid=42,.gid=42,.fsuid=1000,.fsgid=2000};
    struct reliefos_permissions mode={0700,1000,2000};
    assert(!vfs_path_permissions(file,&mode,true));
    assert(!fs_permissions_check_path(&actor,file,FS_ACCESS_READ|FS_ACCESS_WRITE,false));
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_READ,true)==-EACCES);
    mode.mode=0640;assert(!vfs_path_permissions(file,&mode,true));actor.fsuid=3000;
    assert(!fs_permissions_check_path(&actor,file,FS_ACCESS_READ,false));
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_WRITE,false)==-EACCES);
    assert(fs_permissions_chmod_path(&actor,file,0600)==-EPERM);
    actor.fsuid=1000;assert(!fs_permissions_chmod_path(&actor,file,0644));
    assert(!vfs_path_permissions(file,&mode,false) && mode.mode==0644 && mode.uid==1000);
    assert(fs_permissions_chown_path(&actor,file,4000,UINT32_MAX)==-EPERM);
    actor.cap_effective=1ULL<<CAP_CHOWN;
    assert(!fs_permissions_chown_path(&actor,file,4000,5000));
    assert(!vfs_path_permissions(file,&mode,false) && mode.uid==4000 && mode.gid==5000);
    mode=(struct reliefos_permissions){0640,1000,2000};assert(!vfs_path_permissions(file,&mode,true));
    actor=(struct task){.fsuid=0,.fsgid=0};
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_READ,false)==-EACCES);
    actor.cap_effective=1ULL<<CAP_DAC_OVERRIDE;
    assert(!fs_permissions_check_path(&actor,file,FS_ACCESS_READ,false));
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_EXEC,false)==-EACCES);
    mode.mode=0700;assert(!vfs_path_permissions(file,&mode,true));
    assert(!vfs_path_remount(root,MS_NOEXEC));
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_EXEC,false)==-EACCES);
    assert(!fs_permissions_check_path(&actor,directory,FS_ACCESS_EXEC,false));
    assert(!vfs_path_remount(root,MS_RDONLY));
    assert(fs_permissions_check_path(&actor,file,FS_ACCESS_WRITE,false)==-EROFS);
    assert(fs_permissions_chmod_path(&actor,file,0600)==-EROFS);
    assert(!vfs_path_remount(root,0));
    struct reliefos_permissions private={0700,1000,1000};
    assert(!vfs_path_permissions(directory,&private,true));
    actor=(struct task){.fsuid=3000,.fsgid=3000};
    struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_FOLLOW,.search=search_held_directory,.context=&actor};
    struct vfs_lookup_result denied={0};
    assert(vfs_path_resolve(ns,root,root,"mnt/../",&policy,&denied)==-EACCES);
    assert(!denied.path.mount && !denied.path.dentry);
    private.mode=0755;assert(!vfs_path_permissions(directory,&private,true));
    (void)fs;
    puts("PASS held-inode access: fsuid/real IDs, groups, explicit capabilities, readonly/noexec and search before dot-dot");
}

static void held_task_paths(struct vfs_namespace *ns,const struct vfs_path *root,
                               const struct vfs_path *old_dir,const struct vfs_path *old_file)
{
    static struct task directory_actor;
    directory_actor=(struct task){.cap_effective=UINT64_MAX};
    vfs_path_get(root,&directory_actor.fs_paths.root);
    vfs_path_get(old_dir,&directory_actor.fs_paths.cwd);
    directory_actor.files[0]=(struct task_file){.used=1,.node=*vfs_path_node(old_dir)};
    task_file_bind_path(&directory_actor.files[0],old_dir);
    strcpy(directory_actor.files[0].path,"/stale/name");
    char name[]="/mnt",buffer[VFS_PATH_MAX];
    lookup_user_base=(uintptr_t)buffer;lookup_user_bytes=sizeof(buffer);
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_GETCWD,(uintptr_t)buffer,sizeof(buffer))==5);
    assert(!strcmp(buffer,"/mnt"));
    lookup_user_base=(uintptr_t)name;lookup_user_bytes=sizeof(name);
    assert(!syscall_directory(ns,&directory_actor,LINUX_SYS_CHDIR,(uintptr_t)name,0));
    assert(vfs_path_node(&directory_actor.fs_paths.cwd)->volume_id==2);
    lookup_user_base=(uintptr_t)buffer;lookup_user_bytes=sizeof(buffer);
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_GETCWD,(uintptr_t)buffer,sizeof(buffer))==5);
    assert(!strcmp(buffer,"/mnt"));
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_GETCWD,(uintptr_t)buffer,2)==-ERANGE);
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_GETCWD,0,sizeof(buffer))==-EFAULT);
    assert(!syscall_directory(ns,&directory_actor,LINUX_SYS_FCHDIR,3,0));
    assert(vfs_path_equal(&directory_actor.fs_paths.cwd,old_dir));
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_FCHDIR,99,0)==-EBADF);
    directory_actor.cap_effective=0;
    lookup_user_base=(uintptr_t)name;lookup_user_bytes=sizeof(name);
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_CHROOT,(uintptr_t)name,0)==-EPERM);
    assert(vfs_path_equal(&directory_actor.fs_paths.root,root));
    directory_actor.cap_effective=UINT64_MAX;
    assert(!syscall_directory(ns,&directory_actor,LINUX_SYS_CHROOT,(uintptr_t)name,0));
    assert(vfs_path_node(&directory_actor.fs_paths.root)->volume_id==2);
    assert(vfs_path_equal(&directory_actor.fs_paths.cwd,old_dir));
    lookup_user_base=(uintptr_t)buffer;lookup_user_bytes=sizeof(buffer);
    assert(syscall_directory(ns,&directory_actor,LINUX_SYS_GETCWD,(uintptr_t)buffer,sizeof(buffer))==18);
    assert(!strcmp(buffer,"(unreachable)/mnt"));
    vfs_path_put(&directory_actor.files[0].path_ref);sched_task_fs_release(&directory_actor);
    puts("PASS directory syscalls: actual overmount/cwd/root, stale dirfd names, search/capability gates, Linux raw getcwd size and user writes");
    static struct task actor;
    actor=(struct task){.pid=902,.cap_effective=UINT64_MAX};
    vfs_path_get(root,&actor.fs_paths.root);vfs_path_get(old_dir,&actor.fs_paths.cwd);
    actor.files[0]=(struct task_file){.used=1,.node=*vfs_path_node(old_dir)};
    strcpy(actor.files[0].path,"/deliberately/stale");
    task_file_bind_path(&actor.files[0],old_dir);
    actor.files[1]=(struct task_file){.used=1,.node=*vfs_path_node(old_file)};
    task_file_bind_path(&actor.files[1],old_file);
    char size_name[]="old";
    lookup_user_base=(uintptr_t)size_name;lookup_user_bytes=sizeof(size_name);
    assert(!syscall_truncate_object(ns,&actor,LINUX_SYS_TRUNCATE,(uintptr_t)size_name,5));
    struct linux_stat_abi resized;assert(!vfs_path_stat(old_file,&resized) && resized.st_size==5);
    assert(!storage_inode_deny_write(old_file->dentry->backend_pin));
    assert(syscall_truncate_object(ns,&actor,LINUX_SYS_TRUNCATE,(uintptr_t)size_name,1)==-ETXTBSY);
    storage_inode_allow_write(old_file->dentry->backend_pin);
    assert(!vfs_path_stat(old_file,&resized) && resized.st_size==5);
    /* Actual entry admission is covered by the descriptor suite. Supply its
     * stable OFD boundary here while the same numeric fd now names a directory;
     * the real truncation caller must still select the admitted file. */
    struct task_file admitted=actor.files[1];admitted.flags=LINUX_O_RDWR;
    actor.files[1]=(struct task_file){.used=1,.node=*vfs_path_node(old_dir)};
    task_file_bind_path(&actor.files[1],old_dir);
    actor.syscall_fd=4;actor.syscall_file=&admitted;
    assert(!syscall_truncate_object(ns,&actor,LINUX_SYS_FTRUNCATE,4,7));
    assert(!vfs_path_stat(old_file,&resized) && resized.st_size==7);
    assert(!syscall_fallocate_object(&actor,4,0,7,17));
    assert(!vfs_path_stat(old_file,&resized) && resized.st_size==24);
    assert(!syscall_sync_object(ns,&actor,LINUX_SYS_FSYNC,4));
    assert(!syscall_sync_object(ns,&actor,LINUX_SYS_FDATASYNC,4));
    assert(!syscall_sync_object(ns,&actor,LINUX_SYS_SYNCFS,4));
    assert(syscall_fallocate_object(&actor,4,0,INT64_MAX,1)==-EFBIG);
    admitted.flags=TASK_FILE_FLAG_PATH;
    assert(syscall_fallocate_object(&actor,4,0,0,1)==-EBADF);
    assert(syscall_sync_object(ns,&actor,LINUX_SYS_FSYNC,4)==-EBADF);
    /* Linux syncfs accepts O_PATH; it operates on the backing superblock. */
    assert(!syscall_sync_object(ns,&actor,LINUX_SYS_SYNCFS,4));
    admitted.flags=LINUX_O_RDWR;
    const struct vfs_backend_ops *sync_native=old_file->mount->ops;
    struct vfs_backend_ops sync_failure=*sync_native;sync_failure.sync=failed_inode_sync;
    sync_expected=*vfs_path_node(old_file);old_file->mount->ops=&sync_failure;
    assert(syscall_sync_object(ns,&actor,LINUX_SYS_FSYNC,4)==-EIO);
    assert(syscall_sync_object(ns,&actor,LINUX_SYS_FDATASYNC,4)==-EIO);
    assert(syscall_sync_object(ns,&actor,LINUX_SYS_SYNCFS,4)==-EIO && sync_errors==3);
    old_file->mount->ops=sync_native;
    actor.syscall_file=NULL;vfs_path_put(&actor.files[1].path_ref);actor.files[1]=admitted;
    struct trap_frame exec_frame={.rax=LINUX_SYS_EXECVE,.rdi=0x10000};
    struct storage_inode_ref *temporary_guard=NULL;
    task_lookup_scope_begin(&actor,&exec_frame);
    assert(!task_lookup_exec_guard(&actor,old_file,&temporary_guard) && !temporary_guard);
    assert(storage_inode_write_get(old_file->dentry->backend_pin)==-ETXTBSY);
    uint32_t guarded_write=0;
    assert(vfs_path_write(old_file,0,"X",1,&guarded_write)==-ETXTBSY && !guarded_write);
    task_lookup_scope_begin(&actor,&exec_frame);
    assert(!task_lookup_exec_guard(&actor,old_file,&temporary_guard) && !temporary_guard);
    assert(((struct storage_inode_ref *)old_file->dentry->backend_pin)->write_count==-1);
    task_lookup_scope_cancel(&actor);
    assert(!storage_inode_write_get(old_file->dentry->backend_pin));
    task_lookup_scope_begin(&actor,&exec_frame);
    assert(task_lookup_exec_guard(&actor,old_file,&temporary_guard)==-ETXTBSY && !temporary_guard);
    task_lookup_scope_cancel(&actor);storage_inode_write_put(old_file->dentry->backend_pin);
    assert(!task_lookup_exec_guard(&actor,old_file,&temporary_guard) && temporary_guard);
    storage_inode_allow_write(temporary_guard);assert(!storage_inode_put(temporary_guard));
    uint32_t restored=0;
    assert(!vfs_path_truncate(old_file,9));
    assert(!vfs_path_write(old_file,0,"old inode",9,&restored) && restored==9);
    char directory_name[]=".";
    lookup_user_base=(uintptr_t)directory_name;lookup_user_bytes=sizeof(directory_name);
    assert(syscall_truncate_object(ns,&actor,LINUX_SYS_TRUNCATE,(uintptr_t)directory_name,0)==-EISDIR);
    char target[]="original-target";
    lookup_user_base=(uintptr_t)target;lookup_user_bytes=sizeof(target);
    struct trap_frame payload_frame={.rax=LINUX_SYS_SYMLINKAT,.rdi=(uintptr_t)target,.rsi=3,.rdx=0x1234};
    struct task_user_text payload={0};
    task_lookup_scope_begin(&actor,&payload_frame);
    assert(!task_lookup_user_text(&actor,(uintptr_t)target,VFS_PATH_MAX,false,&payload));
    assert(!payload.allocation && !strcmp(payload.text,"original-target"));
    strcpy(target,"changed");lookup_user_base=lookup_user_bytes=0;
    task_lookup_scope_begin(&actor,&payload_frame);
    assert(!task_lookup_user_text(&actor,(uintptr_t)target,VFS_PATH_MAX,false,&payload));
    assert(!payload.allocation && !strcmp(payload.text,"original-target"));
    task_lookup_scope_cancel(&actor);
    struct vfs_path socket_path={0};
    assert(!syscall_unix_path(ns,&actor,"retry-peer",true,&socket_path));
    const uint64_t address_calls[]={LINUX_SYS_SENDTO,LINUX_SYS_SENDMSG,LINUX_SYS_SENDMMSG};
    for(unsigned call=0;call<3;++call) {
        struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,"retry-peer");
        struct trap_frame address_frame={.rax=address_calls[call],.rdi=9,.rsi=(uintptr_t)&address};
        lookup_user_base=(uintptr_t)&address;lookup_user_bytes=sizeof(address);
        task_lookup_scope_begin(&actor,&address_frame);
        struct unix_address initial={0},resumed={0};
        assert(!syscall_unix_address(ns,&actor,(uintptr_t)&address,sizeof(address),false,&initial));
        strcpy(address.sun_path,"now-missing");
        vfs_path_put(&actor.fs_paths.cwd);vfs_path_get(root,&actor.fs_paths.cwd);
        task_lookup_scope_begin(&actor,&address_frame);
        assert(!syscall_unix_address(ns,&actor,(uintptr_t)&address,sizeof(address),false,&resumed));
        assert(vfs_path_same_inode(&initial.object,&resumed.object) &&
            vfs_path_same_inode(&socket_path,&resumed.object));
        vfs_path_put(&initial.object);vfs_path_put(&resumed.object);task_lookup_scope_cancel(&actor);
        vfs_path_put(&actor.fs_paths.cwd);vfs_path_get(old_dir,&actor.fs_paths.cwd);
    }
    assert(!vfs_path_remove(ns,&socket_path,false));vfs_path_put(&socket_path);
    assert(!task_lookup_user_text(&actor,0,32,true,&payload) && payload.allocation && !payload.text[0]);
    kernel_free(payload.allocation);
    assert(task_lookup_user_text(&actor,0,32,false,&payload)==-EFAULT && !payload.text && !payload.allocation);
    struct reliefos_permissions original_parent;
    assert(!vfs_path_permissions(old_dir,&original_parent,false));
    struct reliefos_permissions sgid_parent={02775,0,97};
    assert(!vfs_path_permissions(old_dir,&sgid_parent,true));
    uint32_t original_umask=*sched_task_umask(&actor);*sched_task_umask(&actor)=0027;
    actor.fsgid=37;actor.cap_effective=0;
    char create_name[32]="via-held-dir";
    lookup_user_base=(uintptr_t)create_name;lookup_user_bytes=sizeof(create_name);
    assert(!syscall_create_object(ns,&actor,3,(uintptr_t)create_name,LINUX_S_IFDIR,0777,NULL));
    struct vfs_path made={0};
    assert(!vfs_path_lookup_child(old_dir,"via-held-dir",&made));
    assert(!vfs_path_stat(&made,&resized) && (resized.st_mode&07777)==02750 && resized.st_uid==0 && resized.st_gid==97);
    struct {char old[32],name[32];} rename_names={"via-held-dir","renamed-held-dir"};
    lookup_user_base=(uintptr_t)&rename_names;lookup_user_bytes=sizeof(rename_names);
    assert(!syscall_rename_object(ns,&actor,3,(uintptr_t)rename_names.old,3,(uintptr_t)rename_names.name,0));
    assert(!strcmp(vfs_path_name(&made),"renamed-held-dir"));
    assert(!syscall_rename_object(ns,&actor,3,(uintptr_t)rename_names.name,3,(uintptr_t)rename_names.old,0));
    assert(!strcmp(vfs_path_name(&made),"via-held-dir"));
    lookup_user_base=(uintptr_t)create_name;lookup_user_bytes=sizeof(create_name);
    actor.fsuid=2000;
    assert(syscall_create_object(ns,&actor,3,(uintptr_t)create_name,LINUX_S_IFDIR,0777,NULL)==-EEXIST);
    strcpy(create_name,"denied");
    assert(syscall_create_object(ns,&actor,3,(uintptr_t)create_name,LINUX_S_IFDIR,0777,NULL)==-EACCES);
    actor.fsuid=0;actor.cap_effective=UINT64_MAX;
    struct {char source[16],destination[32];} link_names={"old","caller-hardlink"};
    lookup_user_base=(uintptr_t)&link_names;lookup_user_bytes=sizeof(link_names);
    assert(!syscall_link_object(ns,&actor,3,(uintptr_t)link_names.source,3,(uintptr_t)link_names.destination,0));
    struct vfs_path caller_alias={0};assert(!vfs_path_lookup_child(old_dir,link_names.destination,&caller_alias));
    assert(vfs_path_same_inode(&caller_alias,old_file));
    assert(!syscall_remove_object(ns,&actor,3,(uintptr_t)link_names.destination,false));vfs_path_put(&caller_alias);
    link_names.source[0]=0;actor.cap_effective=0;
    assert(syscall_link_object(ns,&actor,4,(uintptr_t)link_names.source,3,(uintptr_t)link_names.destination,LINUX_AT_EMPTY_PATH)==-ENOENT);
    actor.cap_effective=1ULL<<CAP_DAC_READ_SEARCH;
    assert(!syscall_link_object(ns,&actor,4,(uintptr_t)link_names.source,3,(uintptr_t)link_names.destination,LINUX_AT_EMPTY_PATH));
    actor.cap_effective=UINT64_MAX;
    assert(!syscall_remove_object(ns,&actor,3,(uintptr_t)link_names.destination,false));
    struct {char target[8],name[32];} link_text={"old","via-held-link"};
    lookup_user_base=(uintptr_t)&link_text;lookup_user_bytes=sizeof(link_text);
    assert(!syscall_symlink_object(ns,&actor,(uintptr_t)link_text.target,3,(uintptr_t)link_text.name));
    struct vfs_path made_link={0};
    assert(!vfs_path_lookup_child(old_dir,link_text.name,&made_link));
    char link_bytes[8];size_t link_size=0;
    assert(!vfs_path_readlink(&made_link,link_bytes,sizeof(link_bytes),&link_size) && link_size==3 && !memcmp(link_bytes,"old",3));
    assert(!syscall_remove_object(ns,&actor,3,(uintptr_t)link_text.name,false));
    assert(!vfs_path_stat(&made_link,&resized) && !resized.st_nlink);vfs_path_put(&made_link);
    lookup_user_base=(uintptr_t)create_name;lookup_user_bytes=sizeof(create_name);
    strcpy(create_name,"via-held-dir");
    struct vfs_path child={0};struct reliefos_permissions child_mode={0640,0,0};
    assert(!vfs_path_new_file(&made,"payload",&child_mode,&child));
    assert(syscall_remove_object(ns,&actor,3,(uintptr_t)create_name,true)==-ENOTEMPTY);
    assert(syscall_remove_object(ns,&actor,3,(uintptr_t)create_name,false)==-EISDIR);
    assert(!vfs_path_remove(ns,&child,false));vfs_path_put(&child);
    struct reliefos_permissions sticky_parent=sgid_parent;sticky_parent.mode|=01000;
    assert(!vfs_path_permissions(old_dir,&sticky_parent,true));
    actor.fsuid=2000;actor.fsgid=97;actor.cap_effective=0;
    assert(syscall_remove_object(ns,&actor,3,(uintptr_t)create_name,true)==-EPERM);
    actor.cap_effective=1ULL<<CAP_FOWNER;
    assert(!syscall_remove_object(ns,&actor,3,(uintptr_t)create_name,true));
    assert(!vfs_path_stat(&made,&resized) && !resized.st_nlink);vfs_path_put(&made);
    actor.fsuid=0;actor.cap_effective=UINT64_MAX;
    actor.fsgid=0;*sched_task_umask(&actor)=original_umask;
    assert(!vfs_path_permissions(old_dir,&original_parent,true));
    struct sockaddr_un socket_address={.sun_family=AF_UNIX};
    strcpy(socket_address.sun_path,"held-endpoint");
    lookup_user_base=(uintptr_t)&socket_address;lookup_user_bytes=sizeof(socket_address);
    struct unix_address bound={0},peer={0},rebound={0};
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),true,&bound));
    assert(!strcmp(bound.path,"held-endpoint") && bound.length==14);
    assert(syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),true,&peer)==-EADDRINUSE);
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),false,&peer));
    assert(vfs_path_same_inode(&bound.object,&peer.object));vfs_path_put(&peer.object);
    struct tmpfs_super *memory=g_volumes[vfs_path_node(old_dir)->volume_id].tmpfs;
    assert(!tmpfs_link_child(memory,vfs_path_node(&bound.object)->first_cluster,
        vfs_path_node(old_dir)->first_cluster,"endpoint-alias"));
    strcpy(socket_address.sun_path,"endpoint-alias");
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),false,&peer));
    assert(!vfs_path_equal(&bound.object,&peer.object) && vfs_path_same_inode(&bound.object,&peer.object));
    assert(!vfs_path_remove(ns,&peer.object,false));vfs_path_put(&peer.object);
    assert(!vfs_path_remove(ns,&bound.object,false));
    assert(!vfs_path_stat(&bound.object,&resized) && !resized.st_nlink);
    strcpy(socket_address.sun_path,"held-endpoint");
    assert(syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),false,&peer)==-ENOENT);
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),true,&rebound));
    assert(!vfs_path_same_inode(&bound.object,&rebound.object));
    struct reliefos_permissions denied_socket={0000,0,0};
    assert(!vfs_path_permissions(&rebound.object,&denied_socket,true));actor.cap_effective=0;
    assert(syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,sizeof(socket_address),false,&peer)==-EACCES);
    actor.cap_effective=UINT64_MAX;
    assert(!vfs_path_remove(ns,&rebound.object,false));vfs_path_put(&rebound.object);vfs_path_put(&bound.object);
    socket_address.sun_path[0]=0;socket_address.sun_path[1]='A';socket_address.sun_path[2]=0;socket_address.sun_path[3]='B';
    struct trap_frame address_frame={.rax=__NR_connect,.rdi=3,.rsi=(uintptr_t)&socket_address,.rdx=6};
    task_lookup_scope_begin(&actor,&address_frame);
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,6,false,&peer));
    assert(peer.length==4 && !memcmp(peer.path,"\0A\0B",4) && !peer.object.mount);
    memset(&socket_address,0,sizeof(socket_address));lookup_user_base=lookup_user_bytes=0;
    task_lookup_scope_begin(&actor,&address_frame);
    assert(!syscall_unix_address(ns,&actor,(uintptr_t)&socket_address,6,false,&peer));
    assert(peer.length==4 && !memcmp(peer.path,"\0A\0B",4));task_lookup_scope_cancel(&actor);
    struct vfs_lookup_result found={0};
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,"old",false,false,VFS_LOOKUP_FOLLOW,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    assert(!resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"old",false,false,VFS_LOOKUP_FOLLOW,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    assert(resolve_kernel_object_at_flags(ns,&actor,3,"new",false,false,0,&found)==-ENOENT);
    assert(!found.path.mount);
    assert(!resolve_kernel_object_at_flags(ns,&actor,-999,"/mnt/new",false,false,0,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);
    assert(resolve_kernel_object_at_flags(ns,&actor,999,"old",false,false,0,&found)==-EBADF);
    assert(resolve_kernel_object_at_flags(ns,&actor,4,"old",false,false,0,&found)==-ENOTDIR);
    actor.files[2]=(struct task_file){.used=1,.flags=TASK_FILE_FLAG_PIPE,
        .node={.type=RELIEFOS_FS_TYPE_FIFO}};
    assert(resolve_kernel_object_at_flags(ns,&actor,5,"child",false,false,0,&found)==-ENOTDIR);
    actor.files[2]=(struct task_file){0};
    assert(resolve_kernel_object_at_flags(ns,&actor,3,"",false,false,0,&found)==-ENOENT);
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,"",true,false,0,&found));
    assert(vfs_path_equal(&found.path,old_dir));vfs_path_put(&found.path);
    assert(!resolve_kernel_object_at_flags(ns,&actor,4,"",true,false,0,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,"missing/",false,false,VFS_LOOKUP_PARENT,&found));
    assert(vfs_path_equal(&found.path,old_dir) && !strcmp(found.last,"missing") && found.trailing);
    vfs_path_put(&found.path);
    struct trap_frame retry={.rax=LINUX_SYS_OPENAT,.rdi=3,.rsi=0x1234};
    task_lookup_scope_begin(&actor,&retry);
    const struct vfs_backend_ops *native=old_dir->mount->ops;
    struct vfs_backend_ops waiting=*native;waiting.lookup=lookup_with_wait;old_dir->mount->ops=&waiting;
    lookup_should_wait=true;
    char original_name[]="old";
    assert(resolve_kernel_object_at_flags(ns,&actor,3,original_name,false,false,VFS_LOOKUP_FOLLOW,&found)==-EAGAIN);
    assert(!found.path.mount);
    struct vfs_path replacement={0};
    assert(!vfs_path_lookup(ns,root,root,"/mnt",0,&replacement));
    task_file_bind_path(&actor.files[0],&replacement);
    vfs_path_put(&actor.fs_paths.cwd);vfs_path_get(&replacement,&actor.fs_paths.cwd);
    strcpy(original_name,"new");
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,original_name,false,false,VFS_LOOKUP_FOLLOW,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    task_lookup_scope_cancel(&actor);
    assert(!actor.lookup_scope.armed);
    task_file_bind_path(&actor.files[0],old_dir);
    vfs_path_put(&actor.fs_paths.cwd);vfs_path_get(old_dir,&actor.fs_paths.cwd);
    char user_name[]="old";lookup_user_base=(uintptr_t)user_name;lookup_user_bytes=sizeof(user_name);
    retry.rsi=(uintptr_t)user_name;task_lookup_scope_begin(&actor,&retry);lookup_should_wait=true;
    assert(resolve_user_object_at_flags(ns,&actor,3,retry.rsi,false,false,VFS_LOOKUP_FOLLOW,&found)==-EAGAIN);
    task_file_bind_path(&actor.files[0],&replacement);
    strcpy(user_name,"new");lookup_user_bytes=0; /* Simulated unmap after getname. */
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_user_object_at_flags(ns,&actor,3,retry.rsi,false,false,VFS_LOOKUP_FOLLOW,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);task_lookup_scope_cancel(&actor);
    task_file_bind_path(&actor.files[0],old_dir);
    char absolute_name[]="/mnt/new";lookup_user_base=(uintptr_t)absolute_name;lookup_user_bytes=sizeof(absolute_name);
    retry.rdi=(uint64_t)(int64_t)LINUX_AT_FDCWD;retry.rsi=(uintptr_t)absolute_name;
    task_lookup_scope_begin(&actor,&retry);lookup_should_wait=true;
    assert(resolve_user_object_at_flags(ns,&actor,LINUX_AT_FDCWD,retry.rsi,false,false,0,&found)==-EAGAIN);
    vfs_path_put(&actor.fs_paths.root);vfs_path_get(old_dir,&actor.fs_paths.root);lookup_user_bytes=0;
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_user_object_at_flags(ns,&actor,LINUX_AT_FDCWD,retry.rsi,false,false,0,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);task_lookup_scope_cancel(&actor);
    vfs_path_put(&actor.fs_paths.root);vfs_path_get(root,&actor.fs_paths.root);
    old_dir->mount->ops=native;
    retry.rdi=3;retry.rsi=0x1234;task_lookup_scope_begin(&actor,&retry);
    uint64_t before_scope=old_dir->mount->references;
    for(unsigned i=0;i<7;++i) {
        assert(!resolve_kernel_object_at_flags(ns,&actor,3,"old",false,false,0,&found));
        assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    }
    assert(actor.lookup_scope.extra);task_lookup_scope_cancel(&actor);
    assert(!actor.lookup_scope.extra && old_dir->mount->references==before_scope);
    task_lookup_scope_begin(&actor,&retry);
    for(unsigned i=0;i<2;++i) {
        assert(!resolve_kernel_object_at_flags(ns,&actor,3,"old",false,false,0,&found));vfs_path_put(&found.path);
    }
    test_fail_heap_after=0;
    assert(resolve_kernel_object_at_flags(ns,&actor,3,"old",false,false,0,&found)==-ENOMEM);
    test_fail_heap_after=-1;assert(!found.path.mount);task_lookup_scope_cancel(&actor);
    assert(old_dir->mount->references==before_scope);
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,"old",false,false,0,&found));vfs_path_put(&found.path);
    ++retry.rsi;task_lookup_scope_begin(&actor,&retry);assert(!actor.lookup_scope.paths[0].name);
    retry.rax=LINUX_SYS_READ;task_lookup_scope_begin(&actor,&retry);assert(!actor.lookup_scope.armed);
    const struct vfs_backend_ops *upper_native=replacement.mount->ops;
    struct vfs_backend_ops upper_wait=*upper_native;upper_wait.lookup=lookup_with_wait;
    replacement.mount->ops=&upper_wait;
    retry=(struct trap_frame){.rax=LINUX_SYS_OPENAT,.rdi=(uint64_t)(int64_t)LINUX_AT_FDCWD,.rsi=0x5678};
    task_lookup_scope_begin(&actor,&retry);
    uint64_t upper_before=replacement.mount->references;lookup_should_wait=true;
    assert(resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/new",false,false,0,&found)==-EAGAIN);
    assert(replacement.mount->references==upper_before+1); /* In-progress directory stays pinned. */
    struct vfs_mount *cover=NULL;assert(!storage_create_vfs_mount(&g_volumes[0],NULL,&cover));
    assert(!vfs_mount_attach(ns,cover,&replacement));vfs_mount_put(cover);
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/new",false,false,0,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);
    /* A later permission/open callback may yield after lookup completed.
     * Retrying the syscall must still use that exact completed operand. */
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/new",false,false,0,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);task_lookup_scope_cancel(&actor);
    struct vfs_path cover_root={0};assert(!vfs_path_lookup(ns,root,root,"/mnt",0,&cover_root));
    assert(!vfs_mount_unmount(ns,&cover_root));vfs_path_put(&cover_root);
    assert(replacement.mount->references==upper_before);
    upper_wait.lookup=upper_native->lookup;upper_wait.readlink=readlink_with_wait;native_readlink=upper_native->readlink;
    task_lookup_scope_begin(&actor,&retry);readlink_waits=48;
    assert(resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/link",false,false,VFS_LOOKUP_FOLLOW,&found)==-EAGAIN);
    assert(replacement.mount->references==upper_before+2); /* Directory and exact symlink. */
    assert(!tmpfs_unlink(g_volumes[2].tmpfs,"/link",false));
    while(readlink_waits) {
        task_lookup_scope_begin(&actor,&retry);
        assert(resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/link",false,false,VFS_LOOKUP_FOLLOW,&found)==-EAGAIN);
    }
    task_lookup_scope_begin(&actor,&retry);
    assert(!resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/link",false,false,VFS_LOOKUP_FOLLOW,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);task_lookup_scope_cancel(&actor);
    assert(replacement.mount->references==upper_before);
    assert(!tmpfs_create(g_volumes[2].tmpfs,"/link",LINUX_S_IFLNK|0777,"new",NULL));
    task_lookup_scope_begin(&actor,&retry);readlink_waits=1;
    assert(resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/link",false,false,VFS_LOOKUP_FOLLOW,&found)==-EAGAIN);
    assert(replacement.mount->references==upper_before+2);
    task_lookup_scope_cancel(&actor);assert(replacement.mount->references==upper_before);readlink_waits=0;
    upper_wait.lookup=lookup_with_wait;upper_wait.readlink=upper_native->readlink;
    lookup_queued_task=&actor;
    assert(!register_block_device(&lookup_block_registration));
    for(unsigned changed_call=0;changed_call<2;++changed_call) {
        task_lookup_scope_begin(&actor,&retry);lookup_should_wait=true;
        assert(resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"/mnt/new",false,false,0,&found)==-EAGAIN);
        assert(lookup_waiters.count==1 && ahci_pending_command.active && storage_task_io_owner==actor.pid);
        if(changed_call){++retry.rsi;task_lookup_scope_begin(&actor,&retry);}
        else task_lookup_scope_cancel(&actor);
        assert(!lookup_waiters.count && !actor.waiting_queue && !ahci_pending_command.active && !storage_task_io_owner);
        assert(replacement.mount->references==upper_before);task_lookup_scope_cancel(&actor);
    }
    assert(!unregister_block_device(&lookup_block_registration));
    lookup_queued_task=NULL;
    replacement.mount->ops=upper_native;
    puts("PASS resumable walk: intermediate mount pinned across cover, removed symlink survives48 I/O waits, cancellation releases exact references");
    vfs_path_put(&replacement);
    assert(!resolve_kernel_object_at_flags(ns,&actor,3,"future",false,false,VFS_LOOKUP_FOLLOW|VFS_LOOKUP_MISSING,&found));
    assert(found.missing && !strcmp(found.last,"future") && vfs_path_equal(&found.path,old_dir));
    vfs_path_put(&found.path);
    assert(!resolve_kernel_object_at_flags(ns,&actor,-1,"/mnt/dangling",false,false,VFS_LOOKUP_FOLLOW|VFS_LOOKUP_MISSING,&found));
    assert(found.missing && !strcmp(found.last,"future") && vfs_path_node(&found.path)->volume_id==2);
    vfs_path_put(&found.path);
    assert(resolve_kernel_object_at_flags(ns,&actor,3,"future/child",false,false,VFS_LOOKUP_MISSING,&found)==-ENOENT);
    assert(!found.path.mount && !found.missing);
    assert(!resolve_kernel_object_at_flags(ns,&actor,-1,"/mnt",false,false,0,&found));
    vfs_path_put(&actor.fs_paths.root);actor.fs_paths.root=found.path;found.path=(struct vfs_path){0};
    assert(!resolve_kernel_object_at_flags(ns,&actor,-1,"/new",false,false,0,&found));
    assert(vfs_path_node(&found.path)->volume_id==2);vfs_path_put(&found.path);
    /* chroot alone preserves cwd, matching Linux even when it is outside root. */
    assert(!resolve_kernel_object_at_flags(ns,&actor,LINUX_AT_FDCWD,"old",false,false,0,&found));
    assert(vfs_path_equal(&found.path,old_file));vfs_path_put(&found.path);
    char cwd[VFS_PATH_MAX];
    assert(!vfs_path_getcwd(ns,&actor.fs_paths.root,&actor.fs_paths.cwd,cwd,sizeof(cwd)));
    assert(!strcmp(cwd,"(unreachable)/mnt"));
    assert(vfs_namespace_destroy(ns)==-EBUSY);
    vfs_path_put(&actor.files[0].path_ref);vfs_path_put(&actor.files[1].path_ref);
    vfs_path_put(&actor.fs_paths.root);vfs_path_put(&actor.fs_paths.cwd);
    puts("PASS actual task object resolver: covered cwd/dirfd, stale display name, absolute badfd, empty path, creation parent and independent chroot");
}

static int initial_volume(void *context,struct vfs_mount **out)
{
    assert(!vfs_system_namespace());
    return storage_create_vfs_mount(context,NULL,out);
}
static int initial_devices(void *context,struct vfs_mount **out)
{
    (void)context;assert(!vfs_system_namespace());return devfs_create_vfs_mount(out);
}
static void initial_namespace(void)
{
    struct vfs_boot_mount mounts[]={
        {"/mnt",initial_volume,&g_volumes[2]},
        {"/devices",initial_devices,NULL},
        {"/absent",initial_devices,NULL}
    };
    assert(!vfs_system_namespace());
    assert(vfs_system_bootstrap(initial_volume,&g_volumes[0],mounts,3)==-ENOENT);
    assert(!vfs_system_namespace());
    assert(!storage_inode_volume_busy(0) && !storage_inode_volume_busy(2));
    bool succeeded=false;
    for(int64_t budget=0;budget<45;++budget) {
        test_fail_heap_after=budget;
        int rc=vfs_system_bootstrap(initial_volume,&g_volumes[0],mounts,2);
        test_fail_heap_after=-1;
        if(!rc) {
            struct vfs_namespace *ns=vfs_system_namespace();assert(ns);
            struct vfs_path root={0},path={0};assert(!vfs_namespace_root(ns,&root));
            static struct task initial;
            initial=(struct task){0};assert(!sched_task_fs_start(&initial,ns));
            assert(vfs_path_equal(&initial.fs_paths.root,&root) && vfs_path_equal(&initial.fs_paths.cwd,&root));
            assert(vfs_namespace_destroy(ns)==-EBUSY);
            assert(!vfs_path_lookup(ns,&root,&root,"/mnt/new",0,&path));
            assert(vfs_path_node(&path)->volume_id==2);vfs_path_put(&path);
            assert(!vfs_path_lookup(ns,&root,&root,"/devices/null",0,&path));
            assert(vfs_path_node(&path)->first_cluster==STORAGE_DEV_KIND_NULL);
            vfs_path_put(&path);vfs_path_put(&root);sched_task_fs_release(&initial);
            assert(!vfs_namespace_destroy(ns));succeeded=true;
        } else assert(rc==-ENOMEM);
        assert(!vfs_system_namespace());
        assert(!storage_inode_volume_busy(0) && !storage_inode_volume_busy(2));
    }
    assert(succeeded);
    struct vfs_boot_mount generated={"/initial-generated",initial_devices,NULL,true};
    struct vfs_boot_mount missing_parent={"/initial-missing/child",initial_devices,NULL,true};
    assert(vfs_system_bootstrap(initial_volume,&g_volumes[0],&missing_parent,1)==-ENOENT);
    assert(!tmpfs_create(g_volumes[0].tmpfs,"/initial-not-directory",LINUX_S_IFREG|0600,NULL,NULL));
    struct vfs_boot_mount not_directory={"/initial-not-directory",initial_devices,NULL,true};
    assert(vfs_system_bootstrap(initial_volume,&g_volumes[0],&not_directory,1)==-ENOTDIR);
    assert(!tmpfs_unlink(g_volumes[0].tmpfs,"/initial-not-directory",false));
    bool generated_success=false;
    for(int64_t budget=0;budget<50;++budget) {
        test_fail_heap_after=budget;
        int rc=vfs_system_bootstrap(initial_volume,&g_volumes[0],&generated,1);
        test_fail_heap_after=-1;
        if(!rc){assert(!vfs_namespace_destroy(vfs_system_namespace()));generated_success=true;}
        else assert(rc==-ENOMEM && !vfs_system_namespace());
        assert(!storage_inode_volume_busy(0));
        struct storage_node target;
        rc=tmpfs_lookup(g_volumes[0].tmpfs,generated.target,&target);
        if(!rc) {
            struct linux_stat_abi status;assert(!tmpfs_stat(g_volumes[0].tmpfs,target.first_cluster,&status));
            assert((status.st_mode&07777)==0755 && !status.st_uid && !status.st_gid);
            assert(!tmpfs_unlink(g_volumes[0].tmpfs,generated.target,true));
        } else assert(rc==-ENOENT);
    }
    assert(generated_success);
    assert(!tmpfs_create(g_volumes[0].tmpfs,generated.target,LINUX_S_IFDIR|0701,NULL,NULL));
    struct storage_node target;assert(!tmpfs_lookup(g_volumes[0].tmpfs,generated.target,&target));
    struct reliefos_permissions mode={0701,123,456};
    assert(!tmpfs_permissions(g_volumes[0].tmpfs,target.first_cluster,&mode,true));
    assert(!vfs_system_bootstrap(initial_volume,&g_volumes[0],&generated,1));
    assert(!vfs_namespace_destroy(vfs_system_namespace()));
    struct linux_stat_abi preserved;assert(!tmpfs_stat(g_volumes[0].tmpfs,target.first_cluster,&preserved));
    assert((preserved.st_mode&07777)==0701 && preserved.st_uid==123 && preserved.st_gid==456);
    assert(!tmpfs_unlink(g_volumes[0].tmpfs,generated.target,true));
    puts("PASS initial shared namespace: publish only after every mount, failed attachment rollback, allocation-failure cleanup and actual devfs/storage crossings");
    puts("PASS optional initial mount directory:50 allocation budgets, missing parents/type errors and existing metadata preserved");
}

/* Only these two factory boundaries use independent RAM roots in this storage
 * bootstrap fixture. The procfs/inventory suites exercise the real providers;
 * this test composes the real root/devfs/volume/shm builder and its rollback. */
static bool initial_sysfs_error;
int proc_create_vfs_mount(struct vfs_mount **out)
{assert(!vfs_system_namespace());return storage_create_vfs_mount(&g_volumes[4],NULL,out);}
int sysfs_create_vfs_mount(struct vfs_mount **out)
{
    assert(!vfs_system_namespace());
    if(initial_sysfs_error){*out=NULL;return -EIO;}
    return storage_create_vfs_mount(&g_volumes[5],NULL,out);
}
static void kernel_boot_control(struct vfs_namespace *ns,const struct vfs_path *root)
{
    int rc=vfs_kernel_mkdir(ns,"/boot",0755);assert(!rc || rc==-EEXIST);
    struct vfs_path point={0},boot={0},state={0},link={0};struct vfs_mount *mount=NULL;
    assert(!vfs_path_lookup(ns,root,root,"/boot",0,&point));
    assert(vfs_path_context(&point)==&g_volumes[STORAGE_VOLUME_BOOT]);
    assert(!vfs_mount_unmount(ns,&point));vfs_path_put(&point);
    assert(!vfs_path_lookup(ns,root,root,"/boot",0,&point));
    assert(!storage_create_vfs_mount(&g_volumes[STORAGE_VOLUME_BOOT],NULL,&mount));
    assert(!vfs_mount_attach(ns,mount,&point));vfs_mount_put(mount);vfs_path_put(&point);
    assert(!vfs_path_lookup(ns,root,root,"/boot",0,&boot));
    const char *control="/boot/reliefos/state/kerneldebug.next";
    assert(!storage_write_boot_esp_file(control,"owned",5));
    const void *data=NULL;size_t length=0;
    assert(!vfs_kernel_read_file(ns,control,&data,&length) && length==5 && !memcmp(data,"owned",5));vfs_kernel_file_free(data,length);
    assert(!vfs_kernel_write_file(ns,"/outside-control","outside",7));
    assert(storage_write_boot_esp_file("/boot/../outside-control","denied",6)==-EXDEV);
    assert(!vfs_path_lookup(ns,root,root,"/boot/reliefos/state",0,&state));
    struct reliefos_permissions mode={0777,0,0};
    assert(!vfs_path_create_node(&state,"outside-link",LINUX_S_IFLNK,&mode,"/outside-control",&link));vfs_path_put(&link);
    assert(storage_write_boot_esp_file("/boot/reliefos/state/outside-link","denied",6)==-EXDEV);
    assert(!storage_unlink_boot_esp_file("/boot/reliefos/state/outside-link"));
    assert(!vfs_kernel_read_file(ns,"/outside-control",&data,&length) && length==7 && !memcmp(data,"outside",7));vfs_kernel_file_free(data,length);
    assert(!storage_new_tmpfs_mount("cover-boot",0,NULL,0,0,&mount));
    assert(!vfs_mount_attach(ns,mount,&boot));vfs_mount_put(mount);
    assert(storage_write_boot_esp_file(control,"denied",6)==-ENODEV);
    assert(storage_unlink_boot_esp_file(control)==-ENODEV);
    struct vfs_path cover={0};assert(!vfs_path_lookup(ns,root,root,"/boot",0,&cover));
    assert(!vfs_mount_unmount(ns,&cover));vfs_path_put(&cover);
    assert(!storage_unlink_boot_esp_file(control) && !storage_unlink_boot_esp_file(control));
    vfs_path_put(&state);assert(!vfs_mount_unmount(ns,&boot));vfs_path_put(&boot);
    assert(storage_write_boot_esp_file(control,"denied",6)==-ENODEV);
    assert(!vfs_kernel_unlink(ns,"/outside-control"));
    puts("PASS kernel boot controls: actual ESP attachment, no text labels, escape rejection, literal unlink and overmount rejection");
}
static void production_initial_namespace(struct tmpfs_super *rootfs)
{
    assert(!tmpfs_create(rootfs,"/boot",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(rootfs,"/mount-label-destination",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(rootfs,"/dev",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(rootfs,"/proc",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(rootfs,"/sys",LINUX_S_IFDIR|0755,NULL,NULL));
    for(unsigned slot=4;slot<=5;++slot) {
        struct tmpfs_super *fs=NULL;assert(!tmpfs_new(slot,0,"size=0,nr_inodes=0",0,0,&fs));
        g_volumes[slot]=(struct storage_volume){.volume_id=slot,.kind=STORAGE_VOLUME_ANONYMOUS,.ready=true,.filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.tmpfs=fs};
    }

    initial_sysfs_error=true;
    assert(storage_namespace_bootstrap()==-EIO && !vfs_system_namespace());
    assert(!g_volumes[3].ready && !g_volumes[3].tmpfs);initial_sysfs_error=false;
    bool success=false;
    for(int budget=0;budget<80;++budget) {
        test_fail_heap_after=budget;int rc=storage_namespace_bootstrap();test_fail_heap_after=-1;
        if(rc)assert(rc==-ENOMEM && !vfs_system_namespace());
        else {
            struct vfs_namespace *ns=vfs_system_namespace();assert(ns);
            struct vfs_path root={0},node={0};assert(!vfs_namespace_root(ns,&root));
            assert(!vfs_path_lookup(ns,&root,&root,"/boot",0,&node));
            assert(vfs_path_context(&node)==&g_volumes[STORAGE_VOLUME_BOOT]);vfs_path_put(&node);
            /* A normal alias is attached through the actual VFS API, never a
             * filename field inside its shared filesystem instance. */
            struct vfs_path alias_point={0};struct vfs_mount *alias_mount=NULL;
            assert(!vfs_path_lookup(ns,&root,&root,"/mnt",0,&alias_point));
            assert(!storage_create_vfs_mount(&g_volumes[2],NULL,&alias_mount));
            assert(!vfs_mount_attach(ns,alias_mount,&alias_point));vfs_mount_put(alias_mount);vfs_path_put(&alias_point);
            kernel_boot_control(ns,&root);
            assert(!vfs_path_lookup(ns,&root,&root,"/dev/null",0,&node));
            assert(vfs_path_node(&node)->first_cluster==STORAGE_DEV_KIND_NULL);vfs_path_put(&node);
            assert(!vfs_path_lookup(ns,&root,&root,"/proc",0,&node));assert(vfs_path_node(&node)->volume_id==4);vfs_path_put(&node);
            assert(!vfs_path_lookup(ns,&root,&root,"/sys",0,&node));assert(vfs_path_node(&node)->volume_id==5);vfs_path_put(&node);
            assert(!vfs_path_lookup(ns,&root,&root,"/mnt/new",0,&node));assert(vfs_path_node(&node)->volume_id==2);vfs_path_put(&node);
            assert(!vfs_path_lookup(ns,&root,&root,"/dev/shm",0,&node));
            assert(vfs_path_node(&node)->flags&STORAGE_NODE_FLAG_TMPFS);
            assert(vfs_path_context(&node)==&g_volumes[3]);
            struct reliefos_permissions mode;assert(!vfs_path_permissions(&node,&mode,false));
            assert(mode.mode==01777 && !mode.uid && !mode.gid);
            create_held_file(&node,true);vfs_path_put(&node);
            struct vfs_path mounted={0},destination={0};char diagnostic[64];
            assert(!vfs_path_lookup(ns,&root,&root,"/mnt",0,&mounted));
            assert(!vfs_path_lookup(ns,&root,&root,"/mount-label-destination",0,&destination));
            assert(!g_volumes[2].data_partition_mount && !g_volumes[2].source_disk_id && !g_volumes[2].source_partition_index);
            g_volumes[2].data_partition_mount=true;g_volumes[2].source_disk_id=77;g_volumes[2].source_partition_index=3;
            assert(!storage_disk_partition_mount_path(77,3,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/mnt"));
            assert(!vfs_mount_move(ns,&mounted,&destination));
            assert(!storage_disk_partition_mount_path(77,3,diagnostic,sizeof(diagnostic)) && !strcmp(diagnostic,"/mount-label-destination"));
            assert(storage_disk_partition_mount_path(77,3,diagnostic,3)==-ENAMETOOLONG && !diagnostic[0]);
            g_volumes[2].data_partition_mount=false;g_volumes[2].source_disk_id=0;g_volumes[2].source_partition_index=0;
            vfs_path_put(&mounted);vfs_path_put(&destination);vfs_path_put(&root);
            assert(!vfs_namespace_destroy(ns));
            assert(g_volumes[3].ready && g_volumes[3].tmpfs && !storage_inode_volume_busy(3));
            tmpfs_destroy(g_volumes[3].tmpfs);g_volumes[3]=(struct storage_volume){0};success=true;
        }
        assert(!g_volumes[3].ready && !g_volumes[3].tmpfs);
        for(unsigned slot=0;slot<STORAGE_MAX_VOLUMES;++slot)assert(!storage_inode_volume_busy(slot));
    }
    assert(success);
    for(unsigned slot=4;slot<=5;++slot){tmpfs_destroy(g_volumes[slot].tmpfs);g_volumes[slot]=(struct storage_volume){0};}
    puts("PASS storage initial namespace adapter: explicit root/devfs/providers/volume/shm, sticky volatile storage,80 allocation budgets and failed-provider rollback");
}

#include "mount_fixture_ops.h"

/** @brief Exercise native cluster atime, aliases and orphan-record reuse.
 * @param ns Actual namespace. @param root Retained original root.
 * @param extended True for exFAT, false for FAT32.
 * @return Persisted quantized atime of /time-persist; restores the test clock.
 * All mutation and lifetime operations use actual VFS/native codecs.
 */
static int64_t cluster_access_policy(struct vfs_namespace *ns,const struct vfs_path *root,bool extended)
{
    struct storage_volume *volume=vfs_path_context(root);
    struct vfs_path file={0},point={0},alias={0},other={0};struct vfs_mount *mount=NULL;
    assert(!vfs_kernel_write_file(ns,"/time-file","time",4));
    assert(!vfs_path_lookup(ns,root,root,"/time-file",0,&file));
    assert(!vfs_kernel_mkdir(ns,"/time-alias",0755));
    assert(!vfs_path_lookup(ns,root,root,"/time-alias",0,&point));
    assert(!storage_create_vfs_mount(volume,NULL,&mount) && !vfs_mount_attach(ns,mount,&point));vfs_mount_put(mount);
    assert(!vfs_path_lookup(ns,root,root,"/time-alias",0,&alias));
    assert(!vfs_path_lookup(ns,root,root,"/time-alias/time-file",0,&other));
    uint64_t saved=test_wall_seconds,saved_flags=vfs_path_mount_flags(root);
    assert(!vfs_path_remount(root,MS_STRICTATIME) && !vfs_path_remount(&alias,MS_NOATIME));
    char byte;uint32_t got;struct linux_stat_abi one,two;
    assert(!vfs_path_read(&file,0,&byte,1,&got) && got==1);
    int64_t first=extended ? (int64_t)(test_wall_seconds&~UINT64_C(1)) : (int64_t)(test_wall_seconds/86400*86400);
    assert(!vfs_path_stat(&other,&one) && one.atime_sec==first && !one.atime_nsec);
    test_wall_seconds+=172805;
    assert(!vfs_path_read(&other,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==first);
    assert(!vfs_path_remount(&alias,MS_STRICTATIME));
    assert(!vfs_path_read(&other,0,&byte,1,&got) && got==1);
    int64_t later=extended ? (int64_t)(test_wall_seconds&~UINT64_C(1)) : (int64_t)(test_wall_seconds/86400*86400);
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==later);
    assert(!vfs_path_remount(&alias,MS_RDONLY|MS_STRICTATIME));
    test_wall_seconds+=86400;
    assert(!vfs_path_read(&file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==later);
    assert(!vfs_path_remount(&alias,MS_STRICTATIME));
    vfs_path_put(&other);assert(!vfs_mount_unmount(ns,&alias));vfs_path_put(&alias);
    assert(!vfs_path_remove(ns,&point,true));vfs_path_put(&point);
    assert(!mount_fixture_rename(ns,"/time-file","/time-moved"));
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==later);
    assert(!mount_fixture_remove(ns,"/time-moved",false));
    assert(!vfs_kernel_write_file(ns,"/time-moved","new",3));
    assert(!vfs_path_lookup(ns,root,root,"/time-moved",0,&other));
    assert(!vfs_path_stat(&other,&one));
    assert(!vfs_path_read(&file,0,&byte,1,&got) && got==1 && byte=='t');
    assert(!vfs_path_stat(&other,&two) && two.atime_sec==one.atime_sec && two.atime_nsec==one.atime_nsec);
    int64_t orphan=extended ? (int64_t)(test_wall_seconds&~UINT64_C(1)) : (int64_t)(test_wall_seconds/86400*86400);
    assert(!vfs_path_stat(&file,&two) && two.atime_sec==orphan && !two.st_nlink);
    vfs_path_put(&other);vfs_path_put(&file);
    assert(!mount_fixture_remove(ns,"/time-moved",false));
    assert(!vfs_kernel_write_file(ns,"/time-persist","persist",7));
    assert(!vfs_path_lookup(ns,root,root,"/time-persist",0,&file));
    assert(!vfs_path_read(&file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==orphan);
    if(extended) {
        struct exfat_dir_ref ref;struct exfat_entry_timestamps raw;
        assert(!exfat_node_ref(vfs_path_node(&file),&ref) && !exfat_entry_times_read(&ref,&raw));
        struct storage_dos_time encoded;assert(!storage_dos_encode(orphan+3600,0,&encoded));
        exfat_put_u16(raw.bytes+8,encoded.clock);exfat_put_u16(raw.bytes+10,encoded.date);raw.bytes[16]=0x84;
        assert(!exfat_entry_times_write(&ref,&raw));
        int64_t check;uint32_t fraction;
        exfat_timestamp_decode(0x21,0,155,0x84,&check,&fraction);
        assert(check==315529201 && fraction==550000000);
        exfat_timestamp_decode(0x21,0,155,0xfc,&check,&fraction);
        assert(check==315536401 && fraction==550000000);
    }
    assert(!mount_fixture_rename(ns,"/time-persist","/time-stage"));
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==orphan);
    assert(!vfs_kernel_write_file(ns,"/time-saved","destination",11));
    assert(!mount_fixture_rename(ns,"/time-stage","/time-saved"));
    assert(!vfs_path_stat(&file,&one) && one.atime_sec==orphan);vfs_path_put(&file);
    uint64_t restore=saved_flags;if(!(restore&(MS_NOATIME|MS_RELATIME)))restore|=MS_STRICTATIME;
    assert(!vfs_path_remount(root,restore));test_wall_seconds=saved;
    puts("PASS native FAT/exFAT atime policy, coarse resolution, readonly aliases and unlinked slot reuse isolation");
    return orphan;
}
/** @brief Reopen native cluster metadata after all inode caches have been purged.
 * @param volume Borrowed ready instance. @param expected Persisted access time.
 * @return None; balances the complete new namespace and purges its idle inode cache.
 */
static void cluster_access_persisted(struct storage_volume *volume,int64_t expected)
{
    struct vfs_namespace ns={0};struct vfs_path root={0},file={0};
    assert(!mount_fixture_begin(&ns,volume,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"/time-saved",0,&file));
    struct linux_stat_abi status;assert(!vfs_path_stat(&file,&status) && status.atime_sec==expected && !status.atime_nsec);
    vfs_path_put(&file);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !storage_inode_purge_volume(volume->volume_id));
}

static void cluster_filesystem(const char *input,const char *output,bool extended)
{
    FILE *file=fopen(input,"rb");assert(file && !fseek(file,0,SEEK_END));
    long bytes=ftell(file);assert(bytes>0 && !fseek(file,0,SEEK_SET));
    struct storage_volume *volume=&g_volumes[0];
    *volume=(struct storage_volume){.volume_id=0,
        .esp_start_lba=1,.exfat_start_lba=1,
        .esp_sector_count=(uint64_t)bytes/512,.exfat_sector_count=(uint64_t)bytes/512};
    uint8_t *image=calloc(1,(size_t)bytes+512);assert(image);
    test_memory_bind(volume,image,(uint64_t)bytes+512);
    assert(fread(test_memory_bytes(volume)+512,1,(size_t)bytes,file)==(size_t)bytes && !fclose(file));
    g_active_volume=volume;assert(!(extended ? exfat_mount() : fat32_mount()));
    volume->fs_ops=extended ? &storage_exfat_ops : &storage_fat_ops;
    struct storage_filesystem_ops traced_filesystem=*volume->fs_ops;
    struct storage_cluster_ops traced_clusters;
    cluster_provider_trace(&traced_clusters,traced_filesystem.cluster);
    traced_filesystem.cluster=&traced_clusters;volume->fs_ops=&traced_filesystem;
    volume->ready=true;storage_cache_invalidate();
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};
    assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},held={0};assert(!vfs_namespace_root(&ns,&root));
    int64_t persisted_access=cluster_access_policy(&ns,&root,extended);
    assert(!vfs_kernel_write_file(&ns,"/demo.ko","retained",8));
    struct storage_node value,empty1,empty2;struct storage_inode_ref *pin=NULL,*one=NULL,*two=NULL;
    assert(!mount_fixture_lookup(&ns,"/demo.ko",&value));
    assert(!storage_inode_get(&value,&pin) && pin);
    assert(!vfs_kernel_write_file(&ns,"/empty1","",0) && !vfs_kernel_write_file(&ns,"/empty2","",0));
    assert(!mount_fixture_lookup(&ns,"/empty1",&empty1) && !mount_fixture_lookup(&ns,"/empty2",&empty2));
    assert(!empty1.first_cluster && !empty2.first_cluster);
    assert(!storage_inode_get(&empty1,&one) && one);
    assert(!storage_inode_get(&empty2,&two) && two && two!=one);
    assert(!storage_inode_deny_write(one));
    assert(storage_inode_write_get(one)==-ETXTBSY && !storage_inode_write_get(two));
    storage_inode_write_put(two);storage_inode_allow_write(one);
    assert(!storage_inode_put(one) && !storage_inode_put(two));
    assert(!storage_inode_deny_write(pin));uint32_t got=0;
    assert(mount_fixture_write(&ns,"/demo.ko",0,"X",1,&got)==-ETXTBSY);
    assert(mount_fixture_truncate(&ns,"/demo.ko",0)==-ETXTBSY);
    storage_inode_allow_write(pin);
    assert(!mount_fixture_rename(&ns,"/demo.ko","/moved.ko"));
    assert(!storage_write_held_node(&value,8,"!",1,&got) && got==1);
    char content[32]={0};assert(!storage_read_node(&value,0,content,sizeof(content),&got));
    assert(got==9 && !memcmp(content,"retained!",9));
    assert(!mount_fixture_remove(&ns,"/moved.ko",false));
    assert(!vfs_kernel_write_file(&ns,"/replacement","new",3));
    assert(!storage_truncate_held_node(&value,0));
    assert(!storage_write_held_node(&value,9,"X",1,&got) && got==1);
    memset(content,0xa5,sizeof(content));assert(!storage_read_node(&value,0,content,sizeof(content),&got));
    assert(got==10 && content[9]=='X');for(unsigned i=0;i<9;++i)assert(!content[i]);
    assert(mount_fixture_other_inode_users(&root) && !storage_inode_put(pin));
    assert(!mount_fixture_other_inode_users(&root));
    struct storage_node replacement;assert(!mount_fixture_lookup(&ns,"/replacement",&replacement));
    assert(!storage_read_node(&replacement,0,content,sizeof(content),&got) && got==3 && !memcmp(content,"new",3));
    size_t cluster_bytes=volume->cluster_bytes,capacity=6*cluster_bytes+64;
    uint8_t *expected=calloc(1,capacity),*actual=malloc(capacity);assert(expected && actual);
    for(size_t i=0;i<cluster_bytes-3;++i)expected[i]=(uint8_t)(i*19+7);
    assert(!vfs_kernel_write_file(&ns,"/large",expected,(uint32_t)cluster_bytes-3));
    struct storage_node large,victim;struct storage_inode_ref *large_pin=NULL,*victim_pin=NULL;
    assert(!mount_fixture_lookup(&ns,"/large",&large) && !storage_inode_get(&large,&large_pin) && large_pin);
    /* Occupy the next extent to force exFAT's old NoFatChain file to grow
     * through noncontiguous clusters, rather than accidentally testing only
     * a one-cluster/contiguous fast path. */
    assert(!vfs_kernel_write_file(&ns,"/fragment",expected,(uint32_t)(2*cluster_bytes+7)));
    size_t end=3*cluster_bytes+12;
    memcpy(expected+end-4,"tail",4);
    assert(!storage_write_held_node(&large,end-4,"tail",4,&got) && got==4);
    assert(!storage_read_node(&large,0,actual,(uint32_t)capacity,&got) && got==end && !memcmp(actual,expected,end));
    assert(!storage_truncate_held_node(&large,cluster_bytes+7));
    memset(expected+cluster_bytes+7,0,capacity-cluster_bytes-7);end=5*cluster_bytes+37;
    assert(!storage_truncate_held_node(&large,end));
    memcpy(expected+cluster_bytes-2,"crossing",8);
    assert(!storage_write_held_node(&large,cluster_bytes-2,"crossing",8,&got) && got==8);
    assert(!storage_read_node(&large,0,actual,(uint32_t)capacity,&got) && got==end && !memcmp(actual,expected,end));
    assert(!vfs_kernel_write_file(&ns,"/victim","victim",6));
    assert(!mount_fixture_lookup(&ns,"/victim",&victim) && !storage_inode_get(&victim,&victim_pin) && victim_pin);
    assert(!mount_fixture_rename(&ns,"/large","/victim"));
    assert(!storage_read_node(&victim,0,content,sizeof(content),&got) && got==6 && !memcmp(content,"victim",6));
    assert(!storage_read_node(&large,0,actual,(uint32_t)capacity,&got) && got==end && !memcmp(actual,expected,end));
    assert(!storage_inode_put(victim_pin) && !storage_inode_put(large_pin));
    assert(!mount_fixture_remove(&ns,"/victim",false) && !mount_fixture_remove(&ns,"/fragment",false));
    free(expected);free(actual);
    assert(!vfs_kernel_mkdir(&ns,"/helddir",0755));struct storage_node directory;struct storage_inode_ref *directory_pin=NULL;
    assert(!mount_fixture_lookup(&ns,"/helddir",&directory) && !storage_inode_get(&directory,&directory_pin) && directory_pin);
    assert(!mount_fixture_remove(&ns,"/helddir",true) && mount_fixture_other_inode_users(&root));
    uint64_t cursor=0;struct reliefos_dir_entry entry;
    assert(storage_readdir_node(&directory,&cursor,&entry)>=0);
    assert(!storage_inode_put(directory_pin) && !mount_fixture_other_inode_users(&root));
    /* Compare sequential appends with the same operations after a public
     * cache flush. Count real sector requests at the transport boundary,
     * rather than timing host RAM or assuming a particular cache layout. */
    size_t prefix=2048*cluster_bytes;uint8_t *zeros=calloc(1,prefix);assert(zeros);
    assert(!vfs_kernel_write_file(&ns,"/append",zeros,(uint32_t)prefix));
    assert(!vfs_kernel_write_file(&ns,"/gap",zeros,(uint32_t)cluster_bytes));free(zeros);
    struct storage_node append;struct storage_inode_ref *append_pin=NULL;
    assert(!mount_fixture_lookup(&ns,"/append",&append) && !storage_inode_get(&append,&append_pin) && append_pin);
    assert(!storage_write_held_node(&append,prefix,"s",1,&got) && got==1);
    uint64_t start_reads=filesystem_sector_reads;
    for(unsigned i=0;i<8;++i)assert(!storage_write_held_node(&append,prefix+1+i,"w",1,&got) && got==1);
    uint64_t warm_reads=filesystem_sector_reads-start_reads;
    start_reads=filesystem_sector_reads;
    for(unsigned i=0;i<8;++i) {
        storage_cache_invalidate();
        assert(!storage_write_held_node(&append,prefix+9+i,"c",1,&got) && got==1);
    }
    uint64_t cold_reads=filesystem_sector_reads-start_reads;
    printf("cluster append sector requests warm=%llu cold=%llu\n",
        (unsigned long long)warm_reads,(unsigned long long)cold_reads);fflush(stdout);
    assert(warm_reads<cold_reads);
    assert(!storage_read_node(&append,prefix,content,sizeof(content),&got) && got==17 &&
        !memcmp(content,"swwwwwwwwcccccccc",17));
    assert(!storage_inode_put(append_pin) && !mount_fixture_remove(&ns,"/append",false) && !mount_fixture_remove(&ns,"/gap",false));
    assert(!vfs_path_lookup(&ns,&root,&root,"replacement",0,&held));
    real_mount_description(&ns,&root,extended ? "exfat" : "vfat","rw");
    create_held_file(&root,true);
    struct vfs_path failed_create={0};struct reliefos_permissions failed_mode={0600,1900,2900};
    test_fail_heap_after=0;
    int failed_result=vfs_path_new_file(&root,"unpublished",&failed_mode,&failed_create);
    test_fail_heap_after=-1;
    assert(failed_result==-ENOMEM && !failed_create.mount);
    assert(vfs_path_lookup(&ns,&root,&root,"unpublished",0,&failed_create)==-ENOENT);
    assert(!vfs_path_read(&held,0,content,sizeof(content),&got));
    assert(got==3 && !memcmp(content,"new",3));vfs_path_put(&held);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !storage_inode_volume_busy(0));
    ns=(struct vfs_namespace){0};assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_kernel_mkdir(&ns,"/secure",0755));
    assert(!vfs_kernel_write_file(&ns,"/secure/key","permissions",11));
    assert(!vfs_path_lookup(&ns,&root,&root,"secure/key",0,&held));
    struct reliefos_permissions permission={0604,1701,2701},seen;
    assert(!vfs_path_permissions(&held,&seen,false));assert(seen.mode==0644);
    assert(!vfs_path_permissions(&held,&permission,true));
    assert(!mount_fixture_rename(&ns,"/secure","/moved"));

    permission=(struct reliefos_permissions){0610,1702,2702};
    assert(!vfs_path_permissions(&held,&permission,true));
    assert(!vfs_path_permissions(&held,&seen,false));assert(!memcmp(&permission,&seen,sizeof(seen)));
    struct vfs_path fresh_metadata={0};
    assert(!vfs_path_lookup(&ns,&root,&root,"/moved/key",0,&fresh_metadata));
    assert(!vfs_path_permissions(&fresh_metadata,&seen,false));vfs_path_put(&fresh_metadata);
    assert(!memcmp(&permission,&seen,sizeof(seen)));

    assert(!mount_fixture_remove(&ns,"/moved/key",false));
    assert(!vfs_path_permissions(&held,&seen,false));assert(!memcmp(&permission,&seen,sizeof(seen)));
    permission.mode=0400;assert(!vfs_path_permissions(&held,&permission,true));
    assert(!vfs_path_permissions(&held,&seen,false) && seen.mode==0400);
    vfs_path_put(&held);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !storage_inode_volume_busy(0));
    puts("PASS FAT-family metadata remains on the held directory after directory rename; unlinked inode permissions survive");
    assert(!storage_inode_purge_volume(0));
    assert(!storage_sync_volume(0));
    cluster_access_persisted(volume,persisted_access);
    cluster_provider_checked();
    FILE *result=fopen(output,"wb");assert(result);
    assert(fwrite(test_memory_bytes(volume)+512,1,(size_t)bytes,result)==(size_t)bytes && !fclose(result));
    test_memory_release(volume);*volume=(struct storage_volume){0};storage_cache_invalidate();
    puts("PASS cluster filesystem held inode: empty-file identity, writer exclusion, rename, removed-file growth/truncate and slot reuse isolation");
}

/** @brief Verify actual disk atime through independent aliases and real native metadata.
 * @param ns Borrowed actual namespace. @param root Borrowed original root.
 * @param file Borrowed original regular inode. @param volume Shared native instance.
 * @return None; restores wall-clock fixture and original attachment policy.
 * All paths/providers and native transaction code remain production code.
 */
static void disk_access_policy(struct vfs_namespace *ns,const struct vfs_path *root,
                                   const struct vfs_path *file,struct storage_volume *volume)
{
    struct vfs_path point={0},alias={0},alias_file={0};struct vfs_mount *mount=NULL;
    assert(!vfs_kernel_mkdir(ns,"/atime-alias",0755));
    assert(!vfs_path_lookup(ns,root,root,"/atime-alias",0,&point));
    assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_mount_attach(ns,mount,&point));vfs_mount_put(mount);
    assert(!vfs_path_lookup(ns,root,root,"/atime-alias",0,&alias));
    assert(!vfs_path_lookup(ns,root,root,"/atime-alias/kept/file",0,&alias_file));
    uint64_t saved_clock=test_wall_seconds,saved_policy=vfs_path_mount_flags(root),shared=volume->mount_flags;
    assert(!vfs_path_remount(root,MS_RELATIME));
    assert(!vfs_path_remount(&alias,MS_NOATIME|MS_NODIRATIME));
    assert(!vfs_path_utimens(file,111,222,true,true));
    char byte;uint32_t got;struct linux_stat_abi status;
    assert(!vfs_path_read(&alias_file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==111);
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(&alias_file,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    int64_t future=(int64_t)test_wall_seconds+3600;
    assert(!vfs_path_utimens(file,future,1,true,true));
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==future);
    /* Equality with ctime is itself a relatime trigger. Advance the controlled
     * clock before strict access so the subsequent relatime check is newer. */
    test_wall_seconds+=2;
    assert(!vfs_path_remount(&alias,MS_STRICTATIME));
    assert(!vfs_path_read(&alias_file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    test_wall_seconds+=2;
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==(int64_t)test_wall_seconds-2);
    assert(!vfs_path_remount(root,MS_STRICTATIME));
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    assert(!vfs_path_remount(root,MS_RELATIME));
    assert(!vfs_path_utimens(file,(int64_t)test_wall_seconds-86400,1,true,true));
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    assert(!vfs_path_utimens(file,111,222,true,true));
    access_stat_failure=true;unsigned failures=access_stat_failures;
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);access_stat_failure=false;
    assert(access_stat_failures==failures+1 && !volume->read_only_reason);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==111);
    assert(!vfs_path_read(file,0,NULL,0,&got) && !got);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==111);
    assert(!vfs_path_read(file,INT64_MAX,&byte,1,&got) && !got);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    assert(!vfs_path_utimens(root,111,222,true,true));
    assert(!vfs_path_remount(&alias,MS_RELATIME|MS_NODIRATIME));
    struct reliefos_dir_entry entry;uint64_t cursor=0;
    assert(vfs_path_readdir(&alias,&cursor,&entry)==1);
    assert(!vfs_path_stat(root,&status) && status.atime_sec==111);
    cursor=0;assert(vfs_path_readdir(root,&cursor,&entry)==1);
    assert(!vfs_path_stat(root,&status) && status.atime_sec==(int64_t)test_wall_seconds);
    assert(!vfs_path_utimens(file,111,222,true,true));
    assert(!vfs_path_remount(&alias,MS_RDONLY|MS_STRICTATIME));
    assert(!vfs_path_read(file,0,&byte,1,&got) && got==1);
    assert(!vfs_path_stat(file,&status) && status.atime_sec==111);
    assert(!vfs_path_remount(&alias,MS_STRICTATIME));
    assert(volume->mount_flags==shared);
    uint64_t restore=saved_policy;
    if(!(restore&(MS_NOATIME|MS_RELATIME)))restore|=MS_STRICTATIME;
    assert(!vfs_path_remount(root,restore));test_wall_seconds=saved_clock;
    vfs_path_put(&alias_file);assert(!vfs_mount_unmount(ns,&alias));vfs_path_put(&alias);
    assert(!vfs_path_remove(ns,&point,true));vfs_path_put(&point);
    puts("PASS real disk atime: noatime/relatime/strictatime, nodiratime, shared readonly and alias isolation");
}

static void ext_family(const char *input,const char *output)
{
    load_volume(input,0);
    struct storage_volume *volume=&g_volumes[0];
    assert(!storage_ext4_ops.mkdir(volume,"/kept"));
    struct storage_node created;
    assert(!storage_ext4_ops.create(volume,"/kept/file",0100644,1000,1000,&created));
    uint32_t got=0;
    assert(!storage_ext4_ops.write(volume,&created,0,"retained",8,&got) && got==8);
    assert(!storage_ext4_ops.symlink(volume,"kept/file","/link"));
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};
    assert(!storage_create_vfs_mount(volume,NULL,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},directory={0},file={0},link={0};
    assert(!vfs_namespace_root(&ns,&root));
    real_mount_description(&ns,&root,volume->filesystem==STORAGE_FILESYSTEM_EXT4 ? "ext4" : "ext2","rw");
    create_held_file(&root,true);
    assert(!vfs_path_lookup(&ns,&root,&root,"kept",VFS_LOOKUP_FOLLOW,&directory));
    assert(!vfs_path_lookup(&ns,&root,&directory,"file",VFS_LOOKUP_FOLLOW,&file));
    assert(!vfs_path_lookup(&ns,&root,&root,"link",VFS_LOOKUP_FOLLOW,&link));
    assert(vfs_path_equal(&file,&link));vfs_path_put(&link);
    if(volume->filesystem==STORAGE_FILESYSTEM_EXT4) {
        metadata_commit_reads=metadata_resumable_reads=0;
        storage_set_io_async_context(true);assert(storage_async_can_yield());
        uint64_t epoch=storage_metadata_generation();metadata_async_read_probe=true;
        int rc=vfs_path_utimens(&file,111,222,true,true);metadata_async_read_probe=false;
        assert(!rc && !volume->read_only_reason && !volume->ext4.fs_error);
        assert(metadata_commit_reads && !metadata_resumable_reads);
        assert(storage_io_async_context && storage_io_write_started && storage_metadata_generation()!=epoch);
        storage_set_io_async_context(false);
        struct linux_stat_abi status;assert(!vfs_path_stat(&file,&status));
        assert(status.atime_sec==111 && status.mtime_sec==222);
        puts("PASS real ext4 metadata admission cannot yield during prior-journal commit or latch false readonly");
    }
    disk_access_policy(&ns,&root,&file,volume);
    writer_leases(vfs_path_node(&file));
    assert(!storage_ext4_ops.unlink(volume,"/kept/file"));
    char content[16]={0};
    assert(!vfs_path_read(&file,0,content,8,&got));
    assert(got==8 && !memcmp(content,"retained",8));
    assert(!storage_ext4_ops.rmdir(volume,"/kept"));
    assert(!vfs_path_lookup(&ns,&root,&directory,".",VFS_LOOKUP_FOLLOW,&link));
    assert(vfs_path_equal(&link,&directory));vfs_path_put(&link);
    vfs_path_put(&file);vfs_path_put(&directory);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !storage_inode_volume_busy(0));
    assert(!storage_ext4_ops.unlink(volume,"/link"));
    assert(!storage_sync_volume(0));
    FILE *result=fopen(output,"wb");assert(result);
    assert(fwrite(test_memory_bytes(volume),1,test_memory_length(volume),result)==test_memory_length(volume));
    assert(!fclose(result));
    storage_ext4_journal_close(volume);test_memory_release(volume);
    *volume=(struct storage_volume){0};storage_cache_invalidate();
    puts("PASS real ext-family mount backend: inode-relative namei, native symlinks and held-unlink/removed-directory lifetime");
}

static void device_mount_objects(struct vfs_namespace *ns,const struct vfs_path *root,struct tmpfs_super *fs)
{
    assert(!tmpfs_create(fs,"/devices",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/moved-devices",LINUX_S_IFDIR|0755,NULL,NULL));
    struct vfs_path point={0},destination={0},mounted={0},file={0},input={0},shm={0};
    assert(!vfs_path_lookup(ns,root,root,"devices",0,&point));
    assert(!vfs_path_lookup(ns,root,root,"moved-devices",0,&destination));
    uint8_t saved=g_devfs_enabled;g_devfs_enabled=0;
    struct vfs_mount *device=NULL;assert(!devfs_create_vfs_mount(&device));
    assert(!vfs_mount_attach(ns,device,&point));vfs_mount_put(device);
    assert(!vfs_path_lookup(ns,root,root,"/devices",0,&mounted));
    assert(!vfs_path_lookup(ns,root,root,"/devices/null",0,&file));
    assert(vfs_path_node(&file)->type==RELIEFOS_FS_TYPE_DEVICE &&
        vfs_path_node(&file)->first_cluster==STORAGE_DEV_KIND_NULL);
    struct reliefos_permissions mode;
    assert(!vfs_path_permissions(&file,&mode,false) && mode.mode==0666);
    mode=(struct reliefos_permissions){0600,1200,1300};
    assert(!vfs_path_permissions(&file,&mode,true));
    mode=(struct reliefos_permissions){0};
    assert(!vfs_path_permissions(&file,&mode,false) && mode.mode==0600 && mode.uid==1200 && mode.gid==1300);
    assert(!vfs_path_lookup(ns,root,root,"/devices/input/mouse",0,&input));
    assert(vfs_path_node(&input)->first_cluster==STORAGE_DEV_KIND_MOUSE);
    assert(!vfs_path_lookup(ns,root,root,"/devices/shm",0,&shm));
    assert(vfs_path_node(&shm)->type==RELIEFOS_FS_TYPE_DIR);
    uint64_t cursor=0;struct reliefos_dir_entry entry;bool found_null=false,found_shm=false;int rc;
    while((rc=vfs_path_readdir(&mounted,&cursor,&entry))>0) {
        if(!strcmp(entry.name,"null"))found_null=true;
        if(!strcmp(entry.name,"shm"))found_shm=true;
    }
    assert(!rc && found_null && found_shm);
    assert(!vfs_mount_move(ns,&mounted,&destination));
    struct vfs_path second={0};assert(!vfs_path_lookup(ns,root,root,"/moved-devices/null",0,&second));
    assert(vfs_path_equal(&second,&file));vfs_path_put(&second);
    vfs_path_put(&file);vfs_path_put(&input);vfs_path_put(&shm);
    assert(!vfs_mount_unmount(ns,&mounted));vfs_path_put(&mounted);
    vfs_path_put(&point);vfs_path_put(&destination);g_devfs_enabled=saved;
    puts("PASS real devfs mount: independent attachment/flag, null/input/shm nodes, directory iteration and MS_MOVE");
}

/* Runtime backends belong to graph attachments. No target pathname is stored
 * in the volume, and a bare inode writer/root pin must also prevent teardown. */
static void runtime_mount_objects(struct vfs_namespace *ns,const struct vfs_path *root,struct tmpfs_super *fs)
{
    assert(!tmpfs_create(fs,"/runtime-source",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/runtime-destination",LINUX_S_IFDIR|0755,NULL,NULL));
    struct vfs_path point={0},destination={0},mounted={0},file={0};
    assert(!vfs_path_lookup(ns,root,root,"runtime-source",0,&point));
    assert(!vfs_path_lookup(ns,root,root,"runtime-destination",0,&destination));
    struct vfs_mount *mount=NULL;
    unsigned before=0;for(unsigned i=0;i<STORAGE_MAX_VOLUMES;++i)before+=g_volumes[i].ready;
    assert(storage_new_tmpfs_mount("invalid",MS_BIND,NULL,0,0,&mount)==-EOPNOTSUPP && !mount);
    assert(storage_new_tmpfs_mount("invalid",0,"unknown=1",0,0,&mount)==-EINVAL && !mount);
    char long_source[VFS_PATH_MAX+1];memset(long_source,'s',sizeof(long_source)-1);long_source[sizeof(long_source)-1]=0;
    assert(storage_new_tmpfs_mount(long_source,0,NULL,0,0,&mount)==-ENAMETOOLONG && !mount);
    for(int budget=0;budget<12;++budget) {
        test_fail_heap_after=budget;
        int rc=storage_new_tmpfs_mount("runtime",MS_NOSUID|MS_NODEV,"size=8192,nr_inodes=16,mode=0750",1700,2700,&mount);
        test_fail_heap_after=-1;
        if(rc)assert(rc==-ENOMEM && !mount);
        else {assert(mount);vfs_mount_put(mount);mount=NULL;}
        unsigned after=0;for(unsigned i=0;i<STORAGE_MAX_VOLUMES;++i)after+=g_volumes[i].ready;
        assert(after==before);
    }
    assert(!storage_new_tmpfs_mount("runtime source",MS_NOSUID|MS_NODEV,"size=8192,nr_inodes=16,mode=0750",1700,2700,&mount));
    assert(!vfs_mount_attach(ns,mount,&point));vfs_mount_put(mount);
    assert(!vfs_path_lookup(ns,root,root,"runtime-source",0,&mounted));
    struct storage_volume *volume=vfs_path_context(&mounted);
    assert(volume && volume->ready);
    struct reliefos_permissions mode={0};assert(!vfs_path_permissions(&mounted,&mode,false));
    assert(mode.mode==0750 && mode.uid==1700 && mode.gid==2700);
    mode=(struct reliefos_permissions){0600,1700,2700};
    assert(!vfs_path_new_file(&mounted,"held",&mode,&file));
    struct storage_inode_ref *writer=NULL;assert(!storage_inode_get(vfs_path_node(&file),&writer));
    assert(!storage_inode_write_get(writer));
    assert(vfs_path_remount(&mounted,MS_RDONLY|MS_NOSUID|MS_NODEV)==-EBUSY);
    assert(!(vfs_path_mount_flags(&mounted)&MS_RDONLY));
    storage_inode_write_put(writer);assert(!storage_inode_put(writer));
    assert(!vfs_path_remount(&mounted,MS_RDONLY|MS_NOSUID|MS_NODEV));
    uint32_t written=99;
    assert(vfs_path_write(&file,0,"x",1,&written)==-EROFS && !written);
    assert(!vfs_path_remount(&mounted,MS_NOSUID|MS_NODEV|MS_NOEXEC));
    assert(!vfs_path_write(&file,0,"kept",4,&written) && written==4);
    assert(vfs_path_remount(&file,0)==-EINVAL);
    assert(vfs_path_remount(&mounted,MS_BIND)==-EOPNOTSUPP);
    assert(!vfs_mount_move(ns,&mounted,&destination));
    char table[2048];uint32_t count=0;
    assert(!vfs_read_mount_table(ns,root,false,0,table,sizeof(table)-1,&count));table[count]=0;
    assert(strstr(table,"runtime\\040source /runtime-destination tmpfs rw,nosuid,nodev,noexec 0 0\n"));
    assert(vfs_mount_unmount(ns,&mounted)==-EBUSY);vfs_path_put(&file);
    struct storage_inode_ref *root_pin=NULL;
    assert(!storage_inode_get(vfs_path_node(&mounted),&root_pin));
    assert(vfs_mount_unmount(ns,&mounted)==-EBUSY);assert(!storage_inode_put(root_pin));
    assert(!vfs_path_sync(&mounted,true,false));
    assert(!vfs_mount_unmount(ns,&mounted));
    assert(volume->ready); /* final held path still owns the detached backend */
    vfs_path_put(&mounted);assert(!volume->ready);
    vfs_path_put(&point);vfs_path_put(&destination);
    puts("PASS owned runtime tmpfs mounts: allocation rollback, root credentials, held writer/raw-inode busy checks, remount, MS_MOVE and final backend release");
}

/* Test the real storage finish/discard path, not only a synthetic backend's
 * callback. A detached final mount must not abandon a retained raw inode or
 * lose the backing context after a controller flush failure. */
static void runtime_failed_teardown(void)
{
    for(unsigned mode=0;mode<3;++mode) {
        struct vfs_mount *mount=NULL,*revived=NULL;struct storage_inode_ref *pin=NULL;
        assert(!storage_new_tmpfs_mount("retirement",0,NULL,1700,2700,&mount));
        struct storage_volume *volume=mount->context;uint32_t slot=volume->volume_id;
        if(mode!=1)assert(!storage_inode_get(&mount->root->node,&pin) && pin);
        else {
            /* The RAM inode provider stays real; only its final device-flush
             * boundary is faulted, with no hardware access. */
            retirement_registration.info.device=block_device_allocate_number(false);
            assert(!register_block_device(&retirement_registration));
            assert(!block_device_get(retirement_registration.info.device,&volume->device_ref));
            retirement_flush_failure=true;retirement_flush_calls=0;
        }
        test_fail_heap_after=0;vfs_mount_put(mount);test_fail_heap_after=-1;
        assert(volume->ready && volume->vfs_mounts==1);
        assert(!vfs_reclaim_mounts(8) && volume->ready && volume->vfs_mounts==1);
        if(mode==1)assert(retirement_flush_calls==2);
        if(mode==2) {
            assert(!storage_create_vfs_mount(volume,NULL,&revived) && volume->vfs_mounts==2);
            assert(vfs_reclaim_mounts(8)==1 && volume->ready && volume->vfs_mounts==1);
            assert(!storage_inode_put(pin));pin=NULL;vfs_mount_put(revived);
        } else {
            struct vfs_mount *unrelated=NULL;
            assert(!storage_new_tmpfs_mount("other",0,NULL,1700,2700,&unrelated));
            assert(((struct storage_volume *)unrelated->context)->volume_id!=slot);vfs_mount_put(unrelated);
            if(pin){assert(!storage_inode_put(pin));pin=NULL;}
            retirement_flush_failure=false;
            assert(vfs_reclaim_mounts(8)==1);
            if(mode==1)assert(retirement_flush_calls==3);
        }
        assert(!volume->ready && !volume->vfs_mounts && !vfs_reclaim_mounts(8));
        if(mode==1)assert(!unregister_block_device(&retirement_registration));
    }
    puts("PASS actual backend deferred teardown: raw-inode wait, failed device flush, no allocation, slot retention, revived membership and final retry");
}

/* Linux keeps nosuid/nodev/noexec/atime on each attachment while the backing
 * filesystem and its readonly state are shared. Two real mounts of one tmpfs
 * context must not change each other's policy when one is remounted. */
static void runtime_attachment_policies(struct vfs_namespace *ns,const struct vfs_path *root)
{
    struct vfs_path a={0},b={0},one={0},two={0},file={0},alias={0};
    assert(!vfs_path_lookup(ns,root,root,"runtime-source",0,&a));
    assert(!vfs_path_lookup(ns,root,root,"runtime-destination",0,&b));
    struct vfs_mount *first=NULL,*second=NULL;
    assert(!storage_new_tmpfs_mount("policy",MS_NOSUID|MS_NODEV|MS_RELATIME,NULL,1700,2700,&first));
    struct storage_volume *volume=first->context;
    assert(!storage_create_vfs_mount(volume,NULL,&second));
    assert(!vfs_mount_set_flags(second,MS_NOEXEC|MS_NOATIME));
    assert(!vfs_mount_attach(ns,first,&a) && !vfs_mount_attach(ns,second,&b));
    vfs_mount_put(first);vfs_mount_put(second);
    assert(!vfs_path_lookup(ns,root,root,"runtime-source",0,&one));
    assert(!vfs_path_lookup(ns,root,root,"runtime-destination",0,&two));
    assert(vfs_mount_set_flags(two.mount,0)==-EBUSY);
    struct linux_statfs_abi initial;
    assert(!vfs_path_statfs(&two,&initial) &&
        (initial.f_flags&(MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME|LINUX_ST_RELATIME))==(MS_NOEXEC|MS_NOATIME));
    struct reliefos_permissions mode={0600,1700,2700};uint32_t done=0;
    assert(!vfs_path_new_file(&one,"data",&mode,&file) && !vfs_path_write(&file,0,"kept",4,&done) && done==4);
    assert(!vfs_path_lookup(ns,root,&two,"data",0,&alias) && vfs_path_same_inode(&file,&alias));
    assert(!vfs_path_remount(&two,MS_NOEXEC|MS_NOATIME));
    assert(vfs_path_mount_flags(&one)==(MS_NOSUID|MS_NODEV|MS_RELATIME));
    assert(vfs_path_mount_flags(&two)==(MS_NOEXEC|MS_NOATIME));
    struct linux_statfs_abi left,right;
    assert(!vfs_path_statfs(&one,&left) && !vfs_path_statfs(&two,&right));
    assert((left.f_flags&(MS_NOSUID|MS_NODEV|MS_NOEXEC|LINUX_ST_RELATIME))==
        (MS_NOSUID|MS_NODEV|LINUX_ST_RELATIME));
    assert((right.f_flags&(MS_NOSUID|MS_NODEV|MS_NOEXEC|MS_NOATIME|LINUX_ST_RELATIME))==(MS_NOEXEC|MS_NOATIME));
    assert(!vfs_path_utimens(&file,111,222,true,true));char bytes[8]={0};struct linux_stat_abi status;
    assert(!vfs_path_read(&alias,0,bytes,sizeof(bytes),&done) && done==4 && !memcmp(bytes,"kept",4));
    assert(!vfs_path_stat(&file,&status) && status.atime_sec==111);
    assert(!vfs_path_read(&file,0,bytes,sizeof(bytes),&done) && done==4);
    assert(!vfs_path_stat(&alias,&status) && status.atime_sec==1800000000);
    assert(!vfs_path_utimens(&one,111,222,true,true));
    uint64_t cookie=0;struct reliefos_dir_entry entry;
    assert(vfs_path_readdir(&two,&cookie,&entry)==1 && !vfs_path_stat(&one,&status) && status.atime_sec==111);
    cookie=0;assert(vfs_path_readdir(&one,&cookie,&entry)==1 && !vfs_path_stat(&two,&status) && status.atime_sec==1800000000);
    assert(!vfs_path_remount(&two,MS_NOEXEC|MS_NODIRATIME|MS_RELATIME));
    assert(!vfs_path_utimens(&one,111,222,true,true));cookie=0;
    assert(vfs_path_readdir(&two,&cookie,&entry)==1 && !vfs_path_stat(&one,&status) && status.atime_sec==111);
    assert(!vfs_path_utimens(&file,111,222,true,true) && !vfs_path_read(&alias,0,bytes,sizeof(bytes),&done));
    assert(!vfs_path_stat(&file,&status) && status.atime_sec==1800000000);
    assert(!vfs_path_remount(&two,MS_NOEXEC|MS_STRICTATIME|MS_NOATIME|MS_RELATIME|MS_NODIRATIME));
    assert(vfs_path_mount_flags(&two)==(MS_NOEXEC|MS_NODIRATIME));
    assert(!vfs_path_remount(&two,MS_RDONLY|MS_NOEXEC|MS_NOATIME));
    assert(vfs_path_mount_flags(&one)==(MS_RDONLY|MS_NOSUID|MS_NODEV|MS_RELATIME));
    char table[4096];uint32_t count=0;
    assert(!vfs_read_mount_table(ns,root,true,0,table,sizeof(table)-1,&count));table[count]=0;
    assert(strstr(table," /runtime-source rw,nosuid,nodev,relatime - tmpfs policy ro\n") &&
        strstr(table," /runtime-destination ro,noexec,noatime - tmpfs tmpfs ro\n"));
    assert(vfs_path_write(&file,0,"bad",3,&done)==-EROFS && !done);
    assert(!vfs_path_remount(&two,MS_NOEXEC|MS_NOATIME));
    vfs_path_put(&file);assert(!vfs_mount_unmount(ns,&one));vfs_path_put(&one);
    assert(volume->ready && volume->vfs_mounts==1);
    assert(!vfs_path_read(&alias,0,bytes,sizeof(bytes),&done) && done==4 && !memcmp(bytes,"kept",4));
    assert(vfs_path_mount_flags(&two)==(MS_NOEXEC|MS_NOATIME));
    vfs_path_put(&alias);assert(!vfs_mount_unmount(ns,&two));vfs_path_put(&two);
    assert(!volume->ready);vfs_path_put(&a);vfs_path_put(&b);
    puts("PASS independent attachment policy/statfs/atime with shared readonly backing and last-mount lifetime");
}

/* Boot cleanup must use the same inode/mount graph as ordinary processes.
 * In particular a RAM initial root works, paths deeper than PATH_MAX are
 * cleaned one component at a time, and symlinks/mounts never escape the tree. */
static void runtime_boot_objects(struct vfs_namespace *ns,const struct vfs_path *root,struct tmpfs_super *fs)
{
    assert(!tmpfs_create(fs,"/dev",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/run",LINUX_S_IFDIR|0777,NULL,NULL));
    struct vfs_path run={0},current={0},next={0},outside={0};
    assert(!vfs_path_lookup(ns,root,root,"run",0,&run));vfs_path_get(&run,&current);
    struct reliefos_permissions mode={0700,0,0};
    for(unsigned i=0;i<220;++i) {
        assert(!vfs_path_create_node(&current,"level-0123456789abcdef",LINUX_S_IFDIR,&mode,NULL,&next));
        vfs_path_put(&current);current=next;next=(struct vfs_path){0};
    }
    assert(!vfs_path_new_file(&current,"stale",&mode,&next));vfs_path_put(&next);vfs_path_put(&current);
    assert(!vfs_path_new_file(root,"boot-outside",&mode,&outside));uint32_t done=0;
    assert(!vfs_path_write(&outside,0,"untouched",9,&done) && done==9);
    assert(!vfs_path_create_node(&run,"escape",LINUX_S_IFLNK,&mode,"/boot-outside",&next));vfs_path_put(&next);vfs_path_put(&run);
    assert(!userland_prepare_runtime(ns));
    assert(vfs_path_lookup(ns,root,root,"run/level-0123456789abcdef",0,&next)==-ENOENT);
    assert(vfs_path_lookup(ns,root,root,"run/escape",0,&next)==-ENOENT);
    char bytes[10]={0};assert(!vfs_path_read(&outside,0,bytes,sizeof(bytes),&done) && done==9 && !memcmp(bytes,"untouched",9));
    const struct {const char *path;uint32_t mode;} wanted[]={{"run",0755},{"run/lock",0755},{"dev/shm",01777}};
    for(unsigned i=0;i<sizeof(wanted)/sizeof(wanted[0]);++i) {
        struct reliefos_permissions observed;
        assert(!vfs_path_lookup(ns,root,root,wanted[i].path,0,&next));
        assert(!vfs_path_permissions(&next,&observed,false) && observed.mode==wanted[i].mode && !observed.uid && !observed.gid);
        vfs_path_put(&next);
    }
    assert(!vfs_path_lookup(ns,root,root,"run",0,&run));
    assert(!vfs_path_create_node(&run,"covered",LINUX_S_IFDIR,&mode,NULL,&next));
    struct vfs_mount *cover=NULL;assert(!storage_create_vfs_mount(&g_volumes[0],NULL,&cover));
    assert(!vfs_mount_attach(ns,cover,&next));vfs_mount_put(cover);vfs_path_put(&next);
    assert(userland_prepare_runtime(ns)==-EXDEV); /* Same backend, different attachment. */
    assert(!vfs_path_read(&outside,0,bytes,sizeof(bytes),&done) && done==9 && !memcmp(bytes,"untouched",9));
    assert(!vfs_path_lookup(ns,root,root,"run/covered",0,&next));
    assert(!vfs_mount_unmount(ns,&next));vfs_path_put(&next);vfs_path_put(&run);
    uint64_t references=ns->root->references;bool completed=false;
    for(int budget=0;budget<70;++budget) {
        test_fail_heap_after=budget;int rc=userland_prepare_runtime(ns);test_fail_heap_after=-1;
        assert(!rc || rc==-ENOMEM);completed|=!rc;assert(ns->root->references==references);
    }
    assert(completed && !userland_prepare_runtime(ns));vfs_path_put(&outside);
    const char *cleanup[]={"run/lock","run","dev/shm","dev","boot-outside"};
    for(unsigned i=0;i<sizeof(cleanup)/sizeof(cleanup[0]);++i) {
        assert(!vfs_path_lookup(ns,root,root,cleanup[i],0,&next));
        assert(!vfs_path_remove(ns,&next,i<4));vfs_path_put(&next);
    }
    puts("PASS boot runtime actual objects: RAM root, >5KiB depth, literal symlink removal, same-FS mount boundary, modes and OOM cleanup");
}

static int64_t scoped_mount_call(struct vfs_namespace *ns,struct task *task,uint64_t number,
                                   uint64_t a0,uint64_t a1,uint64_t a2,uint64_t a3,uint64_t a4)
{
    struct trap_frame frame={.rax=number,.rdi=a0,.rsi=a1,.rdx=a2,.r10=a3,.r8=a4};
    task_lookup_scope_begin(task,&frame);
    int64_t rc=syscall_mount_control(ns,task,number,a0,a1,a2,a3,a4);
    if(rc!=-EAGAIN)task_lookup_scope_cancel(task);
    return rc;
}
static void runtime_mount_syscalls(struct vfs_namespace *ns,const struct vfs_path *root,struct tmpfs_super *fs)
{
    assert(!tmpfs_create(fs,"/syscall-source",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/syscall-destination",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/syscall-link",LINUX_S_IFLNK|0777,"syscall-destination",NULL));
    struct task actor={.pid=901,.fsuid=1700,.fsgid=2700};
    vfs_path_get(root,&actor.fs_paths.root);vfs_path_get(root,&actor.fs_paths.cwd);
    struct {char source[64],target[64],type[16],options[80];} user={
        "private source","/syscall-source","tmpfs","size=8192,nr_inodes=16,mode=0700"};
    lookup_user_base=(uintptr_t)&user;lookup_user_bytes=sizeof(user);
    uint64_t source=(uintptr_t)user.source,target=(uintptr_t)user.target,type=(uintptr_t)user.type,options=(uintptr_t)user.options;
    assert(scoped_mount_call(ns,&actor,LINUX_SYS_MOUNT,source,target,type,0,options)==-EPERM);
    actor.cap_effective=1ULL<<CAP_SYS_ADMIN;
    assert(!scoped_mount_call(ns,&actor,LINUX_SYS_MOUNT,source,target,type,MS_NOSUID|MS_NODEV|MS_SILENT|MS_MGC_VAL,options));
    struct vfs_path mounted={0};assert(!vfs_path_lookup(ns,root,root,user.target,0,&mounted));
    struct storage_volume *volume=vfs_path_context(&mounted);
    assert(volume->tmpfs);
    assert((vfs_path_mount_flags(&mounted)&(MS_RELATIME|MS_NOSUID|MS_NODEV))==(MS_RELATIME|MS_NOSUID|MS_NODEV));
    assert(scoped_mount_call(ns,&actor,LINUX_SYS_UMOUNT2,target,0,0,0,0)==-EBUSY);
    vfs_path_put(&mounted);
    assert(!scoped_mount_call(ns,&actor,LINUX_SYS_MOUNT,UINT64_MAX,target,UINT64_MAX,MS_REMOUNT|MS_RDONLY|MS_NOSUID|MS_NODEV|MS_SILENT,0));
    assert(!vfs_path_lookup(ns,root,root,user.target,0,&mounted));
    assert(vfs_path_mount_flags(&mounted)&MS_RDONLY);
    assert(vfs_path_mount_flags(&mounted)&MS_RELATIME);vfs_path_put(&mounted);
    strcpy(user.source,"/syscall-source");strcpy(user.target,"/syscall-destination");
    assert(!scoped_mount_call(ns,&actor,LINUX_SYS_MOUNT,source,target,0,MS_MOVE|MS_SILENT,0));
    assert(!vfs_path_lookup(ns,root,root,user.target,0,&mounted));
    assert(vfs_path_context(&mounted)==volume && (vfs_path_mount_flags(&mounted)&MS_RDONLY));vfs_path_put(&mounted);
    assert(scoped_mount_call(ns,&actor,LINUX_SYS_UMOUNT2,target,0x100,0,0,0)==-EINVAL);
    assert(scoped_mount_call(ns,&actor,LINUX_SYS_UMOUNT2,target,MNT_DETACH,0,0,0)==-EOPNOTSUPP);
    strcpy(user.source,"/syscall-link");
    assert(scoped_mount_call(ns,&actor,LINUX_SYS_UMOUNT2,source,UMOUNT_NOFOLLOW,0,0,0)==-EINVAL);
    assert(!scoped_mount_call(ns,&actor,LINUX_SYS_UMOUNT2,source,0,0,0,0));
    assert(!volume->ready && !actor.lookup_scope.armed);
    vfs_path_put(&actor.fs_paths.root);vfs_path_put(&actor.fs_paths.cwd);
    puts("PASS actual mount/remount/MS_MOVE/umount syscalls: scoped objects, capability, busy refs, atime preservation, ignored remount source/type and nofollow");
}

/** @brief Check real deferred-script rewriting, final DAC and OOM ownership.
 * @param fs Borrowed initial memory filesystem with no published namespace.
 * @return None. Uses actual namei/inodes/script parser, retires every task
 * argument and path on success/error, and leaves no global namespace behind.
 */
static void pending_script_launch(struct tmpfs_super *fs)
{
    assert(!vfs_system_bootstrap(initial_volume,&g_volumes[0],NULL,0));
    struct vfs_namespace *ns=vfs_system_namespace();struct vfs_path root={0},script={0},interpreter={0};
    assert(!vfs_namespace_root(ns,&root));
    struct reliefos_permissions mode={0755,1000,1000};
    assert(!vfs_path_create_node(&root,"init-script",LINUX_S_IFREG,&mode,NULL,&script));
    assert(!vfs_path_create_node(&root,"script-interpreter",LINUX_S_IFREG,&mode,NULL,&interpreter));
    const char text[]="#!/script-interpreter --flag words\n";uint32_t written;
    assert(!vfs_path_write(&script,0,text,sizeof(text)-1,&written) && written==sizeof(text)-1);
    assert(!vfs_path_write(&interpreter,0,"\177ELF",4,&written) && written==4);
    bool oom_complete=false;
    for(int scenario=-2;scenario<100;++scenario) {
        static struct task actor;actor=(struct task){.pid=1901,.fsuid=1000,.fsgid=1000,.flags=TASK_FLAG_PENDING_LOAD};
        assert(!sched_task_fs_start(&actor,ns));
        vfs_path_get(&script,&sched_task_mm(&actor)->executable_path);
        const char data[]="/init-script\0tail\0A=one two";
        actor.exec_data=kernel_malloc(sizeof(data));assert(actor.exec_data);memcpy(actor.exec_data,data,sizeof(data));
        actor.exec_data_len=sizeof(data);actor.exec_argc=2;actor.exec_envc=1;
        actor.exec_argv=kernel_malloc(3*sizeof(char *));actor.exec_envp=kernel_malloc(2*sizeof(char *));
        assert(actor.exec_argv && actor.exec_envp);
        actor.exec_argv[0]=actor.exec_data;actor.exec_argv[1]=actor.exec_data+13;actor.exec_argv[2]=NULL;
        actor.exec_envp[0]=actor.exec_data+18;actor.exec_envp[1]=NULL;
        /* A prebound kernel path does not override the task's final execution
         * authority, for the script or its eventual interpreter. */
        mode.mode=scenario==-2 ? 0600 : 0755;assert(!vfs_path_permissions(&script,&mode,true));
        struct reliefos_permissions executable={0700,scenario==-1 ? 2000 : 1000,1000};
        assert(!vfs_path_permissions(&interpreter,&executable,true));
        if(scenario>=0)test_fail_heap_after=scenario;
        int rc=userland_resolve_pending_scripts(&actor,"/init-script");test_fail_heap_after=-1;
        assert(actor.exec_argv && actor.exec_envp && actor.exec_data);
        if(scenario<0)assert(rc==-EACCES);
        else if(rc)assert(rc==-ENOMEM);
        else {
            assert(actor.exec_argc==4 && actor.exec_envc==1);
            assert(!strcmp(actor.exec_argv[0],"/script-interpreter") && !strcmp(actor.exec_argv[1],"--flag words"));
            assert(!strcmp(actor.exec_argv[2],"/init-script") && !strcmp(actor.exec_argv[3],"tail"));
            assert(!strcmp(actor.exec_envp[0],"A=one two"));
            assert(vfs_path_same_inode(&sched_task_mm(&actor)->executable_path,&interpreter));
            oom_complete=true;
        }
        kernel_free(actor.exec_argv);kernel_free(actor.exec_envp);kernel_free(actor.exec_data);
        vfs_path_put(&sched_task_mm(&actor)->executable_path);sched_task_fs_release(&actor);
        if(oom_complete)break;
    }
    assert(oom_complete);
    assert(!vfs_path_remove(ns,&script,false) && !vfs_path_remove(ns,&interpreter,false));
    vfs_path_put(&script);vfs_path_put(&interpreter);vfs_path_put(&root);assert(!vfs_namespace_destroy(ns));
    struct storage_node missing;assert(tmpfs_lookup(fs,"/init-script",&missing)==-ENOENT);
    puts("PASS pending /init scripts share execve parsing, one optional argument, final script/interpreter DAC and complete OOM ownership");
}

int main(int argc,char **argv)
{
    storage_fixture_register_filesystems();
    struct module unavailable={0};
    struct storage_volume closed={.ready=true,.filesystem=STORAGE_FILESYSTEM_TMPFS};
    struct vfs_mount *closed_mount=NULL;
    /* An unavailable provider must be rejected before its root callback is
     * entered. The absent backing returns EINVAL if consulted prematurely. */
    assert(storage_create_vfs_mount(&closed,&unavailable,&closed_mount)==-ENODEV);
    assert(!closed_mount);
    struct tmpfs_super *fs=NULL,*second=NULL;
    assert(!tmpfs_new(0,0,"size=0,nr_inodes=0",1000,1000,&fs));
    assert(!tmpfs_new(2,0,"size=0,nr_inodes=0",1000,1000,&second));
    g_volumes[0]=(struct storage_volume){.volume_id=0,.kind=STORAGE_VOLUME_ANONYMOUS,.ready=true,
        .filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.tmpfs=fs};
    g_volumes[2]=(struct storage_volume){.volume_id=2,.kind=STORAGE_VOLUME_ANONYMOUS,.ready=true,
        .filesystem=STORAGE_FILESYSTEM_TMPFS,.fs_ops=&storage_tmpfs_ops,.tmpfs=second};
    pending_script_launch(fs);
    real_instance_dispatch(&g_volumes[0]);
    g_active_volume=&g_volumes[0];
    assert(!tmpfs_create(fs,"/mnt",LINUX_S_IFDIR|0755,NULL,NULL));
    assert(!tmpfs_create(fs,"/mnt/old",LINUX_S_IFREG|0644,NULL,NULL));
    assert(!tmpfs_create(second,"/new",LINUX_S_IFREG|0644,NULL,NULL));
    assert(!tmpfs_create(second,"/link",LINUX_S_IFLNK|0777,"new",NULL));
    assert(!tmpfs_create(second,"/dangling",LINUX_S_IFLNK|0777,"future",NULL));
    struct vfs_namespace ns={0};struct vfs_mount *mount=NULL;
    assert(!storage_create_vfs_mount(&g_volumes[0],NULL,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},old_dir={0},old_file={0},top={0},file={0};
    assert(!vfs_namespace_root(&ns,&root));
    real_mount_description(&ns,&root,"tmpfs","rw");
    runtime_boot_objects(&ns,&root,fs);
    runtime_mount_objects(&ns,&root,fs);
    runtime_failed_teardown();
    runtime_attachment_policies(&ns,&root);
    runtime_mount_syscalls(&ns,&root,fs);
    device_mount_objects(&ns,&root,fs);
    assert(!vfs_path_lookup(&ns,&root,&root,"mnt",VFS_LOOKUP_FOLLOW,&old_dir));
    assert(!vfs_path_lookup(&ns,&root,&old_dir,"old",VFS_LOOKUP_FOLLOW,&old_file));
    char long_target[512],long_readback[512];
    for(unsigned i=0;i<200;++i){long_target[2*i]='.';long_target[2*i+1]='/';}
    strcpy(long_target+400,"mnt/old");
    struct reliefos_permissions link_mode={0777,1000,1000};struct vfs_path long_link={0},followed={0};
    assert(!vfs_path_create_node(&root,"long-link",LINUX_S_IFLNK,&link_mode,long_target,&long_link));
    size_t link_length=0;
    assert(!vfs_path_readlink(&long_link,long_readback,sizeof(long_readback),&link_length) &&
        link_length==strlen(long_target) && !memcmp(long_target,long_readback,link_length));
    assert(!vfs_path_lookup(&ns,&root,&root,"long-link",VFS_LOOKUP_FOLLOW,&followed));
    assert(vfs_path_same_inode(&followed,&old_file));vfs_path_put(&followed);
    assert(!vfs_path_remove(&ns,&long_link,false));vfs_path_put(&long_link);
    held_permissions(&ns,&root,fs,&old_file,&old_dir);
    struct storage_node held=*vfs_path_node(&old_file);
    uint32_t written=0;
    assert(!tmpfs_write(fs,held.first_cluster,0,"old inode",9,&written) && written==9);
    writer_leases(&held);
    assert(storage_inode_volume_busy(0));
    /* Lookups use the directory inode pinned in each dentry and cross the
     * real graph edge; the native volume contains no namespace pathname. */
    assert(!storage_create_vfs_mount(&g_volumes[2],NULL,&mount));
    assert(!vfs_mount_attach(&ns,mount,&old_dir));vfs_mount_put(mount);
    assert(!vfs_path_lookup(&ns,&root,&root,"/mnt/link",VFS_LOOKUP_FOLLOW,&file));
    assert(vfs_path_node(&file)->volume_id==2 && vfs_path_node(&file)->type==RELIEFOS_FS_TYPE_FILE);
    vfs_path_put(&file);
    assert(!vfs_path_lookup(&ns,&root,&root,"/mnt",VFS_LOOKUP_FOLLOW,&top));
    held_task_paths(&ns,&root,&old_dir,&old_file);
    create_held_file(&old_dir,false);
    create_held_file(&top,true);
    char retained[16]={0};uint32_t read=0;
    assert(!vfs_path_read(&old_file,0,retained,sizeof(retained),&read) && read==9 && !memcmp(retained,"old inode",9));
    uint64_t cursor=0;struct reliefos_dir_entry entry;bool found_new=false;int step;
    while((step=vfs_path_readdir(&top,&cursor,&entry))>0) {
        if(!strcmp(entry.name,"new"))found_new=true;
        assert(strcmp(entry.name,"old"));
    }
    assert(!step && found_new);
    assert(!vfs_path_lookup(&ns,&root,&old_dir,"old",VFS_LOOKUP_FOLLOW,&file));
    assert(vfs_path_node(&file)->volume_id==0 && vfs_path_equal(&file,&old_file));vfs_path_put(&file);
    assert(!tmpfs_unlink(fs,"/mnt/old",false));
    assert(!tmpfs_unlink(fs,"/mnt/direct-new",false));
    memset(retained,0,sizeof(retained));
    assert(!vfs_path_read(&old_file,0,retained,sizeof(retained),&read) && read==9 && !memcmp(retained,"old inode",9));
    assert(!tmpfs_stat(fs,held.first_cluster,&(struct linux_stat_abi){0}));
    assert(!tmpfs_unlink(fs,"/mnt",true));
    vfs_path_unlinked(&old_dir);
    char removed_cwd[VFS_PATH_MAX];
    assert(vfs_path_getcwd(&ns,&root,&old_dir,removed_cwd,sizeof(removed_cwd))==-ENOENT);
    assert(vfs_path_getcwd(&ns,&old_dir,&old_dir,removed_cwd,sizeof(removed_cwd))==-ENOENT);
    /* A held removed directory is still a valid object for . and .. . */
    assert(!vfs_path_lookup(&ns,&root,&old_dir,".",VFS_LOOKUP_FOLLOW,&file));
    assert(vfs_path_equal(&file,&old_dir));vfs_path_put(&file);
    assert(!vfs_mount_unmount(&ns,&top));vfs_path_put(&top);
    vfs_path_put(&old_file);
    assert(tmpfs_stat(fs,held.first_cluster,&(struct linux_stat_abi){0})==-2);
    vfs_path_put(&old_dir);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));
    assert(!storage_inode_volume_busy(0) && !storage_inode_volume_busy(2));
    assert(!tmpfs_create(fs,"/mnt",LINUX_S_IFDIR|0755,NULL,NULL));
    initial_namespace();
    production_initial_namespace(fs);
    tmpfs_destroy(fs);tmpfs_destroy(second);
    g_volumes[0]=(struct storage_volume){0};g_volumes[2]=(struct storage_volume){0};
    if(argc==3)ext_family(argv[1],argv[2]);
    if(argc==2)iso_filesystem(argv[1]);
    if(argc==4)cluster_filesystem(argv[1],argv[2],!strcmp(argv[3],"exfat"));
    puts("PASS real tmpfs mount backend: inode-relative lookup, covered cwd, symlink traversal, removed objects retained through final path, no text-prefix routing");
    return 0;
}
