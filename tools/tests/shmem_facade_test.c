/* Reuse the RAM-backed full VFS facade and its hardware boundary stubs. */
#define main storage_mkdir_reference_main
#define mm_alloc_page storage_unused_alloc_page
#define mm_free_page storage_unused_free_page
#include "storage_mkdir_mount_test.c"
#undef main
#undef mm_alloc_page
#undef mm_free_page

static struct { uint64_t address; unsigned refs; } shmem_frames[8];
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
    tmpfs_destroy(g_shmem_volume.tmpfs);
    g_shmem_volume = (struct storage_volume){0};
    puts("PASS shmem VFS facade: independent superblock, sparse 32 GiB file, coherent pages and inode pins");
}
