/* Reuse the RAM-backed full VFS facade and its hardware boundary stubs. */
#define RELIEFNT_FIXTURE_OWN_IRQ_LOCKS
#define main storage_mkdir_reference_main
#define mm_alloc_page storage_unused_alloc_page
#define mm_free_page storage_unused_free_page
#include "storage_mkdir_mount_test.c"
#undef main
#undef mm_alloc_page
#undef mm_free_page
#include "../../kernel/ipc/shm.c"
#include <linux/errno.h>

static struct { uint64_t address; unsigned refs; } shmem_frames[8];
int file_can_write(const struct task_file *file)
{ return file && (file->flags & RELIEFOS_O_ACCMODE)!=RELIEFOS_O_RDONLY; }
struct task *sched_task_at(uint32_t index) { (void)index;return NULL; }
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task;return 0; }
struct task_vma *sched_task_vma_at(struct task *task,uint32_t index)
{ (void)task;(void)index;abort(); }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state=0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ (void)lock;*flags=0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ (void)lock;(void)flags; }
static void shmem_file_close(struct task_file *file)
{
    if(file->inode_write_access)storage_inode_write_put(file->inode);
    assert(!storage_inode_put(file->inode));task_shm_release(file);
    vfs_path_put(&file->path_ref);*file=(struct task_file){0};
}
static void shmem_readonly_reopen(void)
{
    struct task_file writer={.used=1,.flags=RELIEFOS_O_RDWR};
    assert(!task_shm_attach(&writer,LINUX_MFD_ALLOW_SEALING));
    char label[250];memset(label,'a',249);label[249]=0;
    assert(!task_shm_set_name(&writer,label,249));
    assert(strlen(writer.path)==266 && !memcmp(writer.path,"/memfd:",7) &&
        !memcmp(writer.path+7,label,249) && !strcmp(writer.path+256," (deleted)"));
    char displayed[300];
    assert(!vfs_path_format_link(NULL,NULL,&writer.path_ref,displayed,sizeof(displayed)) && !strcmp(displayed,writer.path));
    assert(task_shm_set_name(&writer,label,250)==-EINVAL && strlen(writer.path)==266);
    assert(writer.path_ref.mount && vfs_path_node(&writer.path_ref)->first_cluster==writer.node.first_cluster);
    assert(task_shm_write(&writer,"module data",11)==11);
    struct linux_stat_abi attributes;
    assert(!vfs_path_stat(&writer.path_ref,&attributes) && attributes.st_size==11 && !attributes.st_nlink);
    struct linux_statfs_abi filesystem;
    assert(!vfs_path_statfs(&writer.path_ref,&filesystem) && filesystem.f_type==0x01021994);
    assert(!task_shm_add_seals(&writer,LINUX_F_SEAL_SHRINK));
    assert(vfs_path_truncate(&writer.path_ref,0)==-EPERM);
    assert(!vfs_path_fallocate(&writer.path_ref,1,4096,4096));
    assert(!vfs_path_stat(&writer.path_ref,&attributes) && attributes.st_size==11 && attributes.st_blocks==16);
    assert(!vfs_path_sync(&writer.path_ref,false,false));
    assert(!vfs_path_sync(&writer.path_ref,true,false));
    assert(!task_shm_add_seals(&writer,LINUX_F_SEAL_GROW));
    assert(vfs_path_fallocate(&writer.path_ref,1,4096,4096)==-EPERM);
    assert(vfs_path_fallocate(&writer.path_ref,0,11,1)==-EPERM);
    assert(!vfs_path_fallocate(&writer.path_ref,0,0,11));
    struct task_file reader={.used=1,.flags=RELIEFOS_O_RDONLY};
    assert(!task_shm_reopen(&reader,&writer.path_ref));
    assert(!reader.offset && writer.offset==11 && reader.aux==writer.aux);
    assert(!strcmp(reader.path,writer.path));
    assert(storage_inode_deny_write(reader.inode)==-ETXTBSY);
    struct vfs_path retained={0};vfs_path_get(&reader.path_ref,&retained);
    shmem_file_close(&writer);
    assert(!storage_inode_deny_write(reader.inode));
    struct task_file blocked={.used=1,.flags=RELIEFOS_O_RDWR};
    assert(task_shm_reopen(&blocked,&reader.path_ref)==-ETXTBSY);
    assert(!blocked.inode && !blocked.path_ref.mount && !blocked.aux && blocked.flags==RELIEFOS_O_RDWR);
    storage_inode_allow_write(reader.inode);
    char bytes[16]={0};assert(task_shm_read(&reader,bytes,sizeof(bytes))==11 && !memcmp(bytes,"module data",11));
    assert(task_shm_get_seals(&reader)==(LINUX_F_SEAL_SHRINK|LINUX_F_SEAL_GROW));
    shmem_file_close(&reader);
    /* A held magic-link target keeps backing data and inode-wide seals even
     * after every original descriptor has closed. Reopen creates a new OFD. */
    reader=(struct task_file){.used=1,.flags=RELIEFOS_O_RDONLY};
    assert(!task_shm_reopen(&reader,&retained));vfs_path_put(&retained);
    assert(strlen(reader.path)==266 && !memcmp(reader.path+7,label,249));
    memset(bytes,0,sizeof(bytes));
    assert(task_shm_read(&reader,bytes,sizeof(bytes))==11 && !memcmp(bytes,"module data",11));
    assert(task_shm_get_seals(&reader)==(LINUX_F_SEAL_SHRINK|LINUX_F_SEAL_GROW));
    struct storage_node old=reader.node;shmem_file_close(&reader);
    assert(storage_inode_stat(&old,&(struct linux_stat_abi){0})==-RELIEFOS_ENOENT);
    puts("PASS real shmem readonly reopen: independent offset, writer leases, retained anonymous target and shared seals");
}
static void shmem_path_failures(void)
{
    unsigned failures=0,successes=0;
    for(unsigned phase=0;phase<2;++phase)for(int budget=0;budget<9;++budget) {
        struct task_file file={.used=1,.flags=RELIEFOS_O_RDWR};
        test_fail_heap_after=budget;
        int rc=task_shm_attach(&file,LINUX_MFD_ALLOW_SEALING);
        test_fail_heap_after=-1;
        if(rc) {
            assert(rc==-ENOMEM && !file.inode && !file.path_ref.mount && !file.aux);
            assert(file.flags==RELIEFOS_O_RDWR);++failures;
        } else {++successes;shmem_file_close(&file);}
        assert(!storage_inode_volume_busy(STORAGE_SHMEM_VOLUME));
        for(unsigned i=0;i<kernel_objects()->capacity;++i)assert(!kernel_objects()->objects[i]);
    }
    assert(failures>=8 && successes);
    puts("PASS real shmem attach allocation failures release inode, write lease, seal registry and unpublished path");
}
uint64_t mm_total_memory_kib(void) { return 65536; }
uint64_t mm_alloc_page(void)
{
    for (unsigned i = 0; i < 8; ++i) if (!shmem_frames[i].refs) {
        shmem_frames[i].address = (uintptr_t)aligned_alloc(4096, 4096);
        shmem_frames[i].refs = shmem_frames[i].address ? 1 : 0;
        return shmem_frames[i].address;
    }
    return 0;
}
void mm_retain_page(uint64_t address)
{
    for (unsigned i = 0; i < 8; ++i) if (shmem_frames[i].address == address && shmem_frames[i].refs) {
        ++shmem_frames[i].refs; return;
    }
    abort();
}
void mm_free_page(uint64_t address)
{
    for (unsigned i = 0; i < 8; ++i) if (shmem_frames[i].address == address && shmem_frames[i].refs) {
        if (!--shmem_frames[i].refs) free((void *)(uintptr_t)address);
        return;
    }
    abort();
}

int main(void)
{
    kernel_objects_init();
    /* Exhausting mounted-volume slots cannot prevent anonymous-file creation. */
    for (unsigned i = 0; i < STORAGE_MAX_VOLUMES; ++i) g_volumes[i].ready = true;
    /* Anonymous memory operations must also progress while another process
     * owns a resumable disk command. They do not use that command's buffers. */
    struct storage_volume *original_volume = g_active_volume;
    storage_task_io_owner = 2;
    storage_set_io_async_context(true);
    struct storage_node nodes[80];
    struct storage_inode_ref *refs[80];
    for (unsigned i = 0; i < 80; ++i) {
        assert(!storage_shmem_create(1000, 1001, &nodes[i], &refs[i]));
        assert(refs[i] && nodes[i].volume_id == STORAGE_SHMEM_VOLUME && !nodes[i].size);
        struct linux_stat_abi info;
        assert(!storage_inode_stat(&nodes[i], &info));
        assert(info.st_uid == 1000 && info.st_gid == 1001 && !info.st_nlink);
        assert(info.st_mode == (LINUX_S_IFREG | 0777));
    }
    const uint64_t offset = 16ULL * 1024 * 1024;
    assert(!storage_truncate_held_node(&nodes[0], 32ULL * 1024 * 1024 * 1024));
    uint64_t flags = UINT64_MAX;
    assert(!storage_node_mount_flags(&nodes[0], &flags) && !flags);
    uint32_t done;
    char data[8];
    assert(!storage_write_held_node(&nodes[0], offset, "data", 4, &done) && done == 4);
    assert(!storage_read_node(&nodes[0], offset - 4, data, sizeof(data), &done) && done == 8);
    assert(!memcmp(data, "\0\0\0\0data", 8));
    uint64_t page;
    assert(!storage_tmpfs_get_page(&nodes[0], offset, &page));
    ((char *)mm_phys_to_virt(page))[0] = 'D';
    storage_inode_retain(refs[0]);
    assert(!storage_inode_put(refs[0]));
    assert(!storage_read_node(&nodes[0], offset, data, 4, &done) && done == 4 && !memcmp(data, "Data", 4));
    mm_free_page(page);
    for (unsigned i = 0; i < 80; ++i) assert(!storage_inode_put(refs[i]));
    assert(storage_inode_stat(&nodes[0], &(struct linux_stat_abi){0}) == -2);
    for (unsigned i = 0; i < 8; ++i) assert(!shmem_frames[i].refs);
    assert(storage_task_io_owner == 2 && g_active_volume == original_volume);
    storage_release_task_io(2);
    storage_set_io_async_context(false);
    shmem_path_failures();shmem_readonly_reopen();
    tmpfs_destroy(g_shmem_volume.tmpfs);
    g_shmem_volume = (struct storage_volume){0};
    puts("PASS shmem VFS facade: independent superblock, sparse 32 GiB file, coherent pages and inode pins");
}
