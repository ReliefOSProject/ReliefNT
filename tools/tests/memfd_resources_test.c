#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/time.h>
#include "../../kernel/ipc/shm.c"

static struct task tasks[80];
static uint32_t task_count;
static struct tmpfs_super *backing;
struct storage_inode_ref { uint32_t ino; };
static struct { uint64_t phys; uint32_t refs; } backing_pages[64];
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
uint64_t mm_alloc_pages(uint32_t count)
{
    void *memory = aligned_alloc(4096, (size_t)count * 4096);
    if (memory) memset(memory, 0, (size_t)count * 4096);
    return (uintptr_t)memory;
}
void mm_free_pages(uint64_t address, uint32_t count) { (void)count; free((void *)(uintptr_t)address); }
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
uint64_t mm_alloc_page(void)
{
    for (unsigned i = 0; i < 64; ++i) if (!backing_pages[i].refs) {
        uint64_t phys = mm_alloc_pages(1);
        backing_pages[i].phys = phys;
        backing_pages[i].refs = phys ? 1 : 0;
        return phys;
    }
    return 0;
}
void mm_retain_page(uint64_t phys)
{
    for (unsigned i = 0; i < 64; ++i) if (backing_pages[i].phys == phys && backing_pages[i].refs) {
        ++backing_pages[i].refs; return;
    }
    abort();
}
void mm_free_page(uint64_t phys)
{
    for (unsigned i = 0; i < 64; ++i) if (backing_pages[i].phys == phys && backing_pages[i].refs) {
        if (!--backing_pages[i].refs) mm_free_pages(phys, 1);
        return;
    }
    abort();
}
uint64_t mm_total_memory_kib(void) { return 65536; }
int time_wall_clock(struct reliefos_time_info *time)
{ *time = (struct reliefos_time_info){.unix_seconds = 1234}; return 0; }
void page_cache_invalidate_node(const struct storage_node *node) { (void)node; }
void sched_truncate_file_mappings(const struct storage_node *node, uint64_t size)
{ (void)node; (void)size; }
int storage_shmem_create(uint32_t uid, uint32_t gid, struct storage_node *node, struct storage_inode_ref **out)
{
    if (!backing) assert(!tmpfs_new(STORAGE_SHMEM_VOLUME, 0, "size=0,nr_inodes=0", 0, 0, &backing));
    int ret = tmpfs_create_anonymous(backing, uid, gid, node);
    if (ret) return ret;
    *out = malloc(sizeof(**out)); assert(*out); (*out)->ino = node->first_cluster;
    return 0;
}
int storage_inode_put(struct storage_inode_ref *ref)
{ if (ref) { tmpfs_drop(backing, ref->ino); free(ref); } return 0; }
int storage_truncate_held_node(struct storage_node *node, uint64_t size)
{ int ret = tmpfs_truncate(backing, node->first_cluster, size); if (!ret) node->size = size; return ret; }
int storage_read_node(const struct storage_node *node, uint64_t off, void *data, uint32_t length, uint32_t *done)
{ return tmpfs_read(backing, node->first_cluster, off, data, length, done); }
int storage_write_held_node(struct storage_node *node, uint64_t off, const void *data, uint32_t length, uint32_t *done)
{ return tmpfs_write(backing, node->first_cluster, off, data, length, done); }
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags) { (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags) { (void)lock; (void)flags; }
struct task *sched_current_task(void) { return &tasks[0]; }
struct task *sched_task_at(uint32_t index) { return index < task_count ? &tasks[index] : NULL; }
struct task *sched_find(uint32_t pid) { return pid && pid <= task_count ? &tasks[pid - 1] : NULL; }
uint32_t sched_snapshot(struct task_snapshot_info *out, uint32_t capacity, uint64_t *tick)
{
    if (tick) *tick = 0;
    if (!out || !capacity) return task_count;
    uint32_t count = task_count < capacity ? task_count : capacity;
    for (uint32_t i = 0; i < count; ++i) out[i] = (struct task_snapshot_info){.pid = i + 1};
    return count;
}
struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{ return index < SCHED_TASK_VMA_MAX ? &sched_task_mm(task)->vmas[index] : NULL; }
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task; return SCHED_TASK_VMA_MAX; }
int file_can_write(const struct task_file *file) { return file && (file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY; }

int main(int argc, char **argv)
{
    (void)argv;
    kernel_objects_init();
    struct task_file files[80] = {0};
    unsigned count = argc > 1 ? 1 : 80;
    for (unsigned i = 0; i < count; ++i) {
        files[i].flags = RELIEFOS_O_RDWR;
        int ret = task_shm_attach(&files[i], LINUX_MFD_ALLOW_SEALING);
        if (ret) fprintf(stderr, "memfd %u attach: %d\n", i, ret);
        assert(!ret);
        assert(!task_shm_truncate(&files[i], 0));
    }
    if (argc > 1) {
        int ret = task_shm_truncate(&files[0], 16 * 1024 * 1024);
        if (ret) fprintf(stderr, "16 MiB memfd truncate: %d\n", ret);
        assert(!ret);
        assert(files[0].node.size == 16 * 1024 * 1024);
        files[0].offset = 16 * 1024 * 1024 - 4;
        assert(task_shm_write(&files[0], "tail", 4) == 4);
        char result[8] = {1,1,1,1,1,1,1,1};
        files[0].offset -= 8;
        assert(task_shm_read(&files[0], result, 8) == 8);
        assert(!memcmp(result, "\0\0\0\0tail", 8));
        assert(!task_shm_truncate(&files[0], 1));
        assert(!task_shm_truncate(&files[0], 16 * 1024 * 1024));
        files[0].offset = 16 * 1024 * 1024 - 8;
        assert(task_shm_read(&files[0], result, 8) == 8);
        assert(!memcmp(result, "\0\0\0\0\0\0\0\0", 8));
        /* A potentially writable shared VMA after task 64 prevents sealing,
         * including after mprotect has removed current write permission. */
        task_count = 80;
        for (unsigned i = 0; i < task_count; ++i) tasks[i].pid = i + 1;
        tasks[70].vmas[0] = (struct task_vma){.used = 1,
            .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_SHARED,
            .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE, .prot = LINUX_PROT_READ,
            .file_node = files[0].node};
        assert(task_shm_add_seals(&files[0], LINUX_F_SEAL_WRITE) == -RELIEFOS_EBUSY);
        tasks[70].vmas[0].flags &= ~TASK_VMA_FLAG_SHARED;
        assert(!task_shm_add_seals(&files[0], LINUX_F_SEAL_WRITE));
        uint32_t maximum = LINUX_PROT_READ | LINUX_PROT_WRITE;
        assert(!task_shm_mmap_check(&files[0], LINUX_MAP_PRIVATE, LINUX_PROT_WRITE, &maximum));
        assert(maximum == (LINUX_PROT_READ | LINUX_PROT_WRITE));
        assert(task_shm_mmap_check(&files[0], LINUX_MAP_SHARED, LINUX_PROT_WRITE, &maximum) == -RELIEFOS_EPERM);
        assert(!task_shm_mmap_check(&files[0], LINUX_MAP_SHARED, LINUX_PROT_READ, &maximum));
        assert(maximum == LINUX_PROT_READ);
        assert(task_shm_write(&files[0], "x", 1) == -RELIEFOS_EPERM);
        /* Simulate the real VMA inode hold surviving the last descriptor. */
        struct storage_node mapped = files[0].node;
        assert(!tmpfs_hold(backing, mapped.first_cluster));
        task_shm_release(&files[0]);
        storage_inode_put(files[0].inode); files[0].inode = NULL;
        files[0].flags = 0;
        uint64_t page = 0;
        assert(!tmpfs_get_page(backing, mapped.first_cluster, 0, &page));
        assert(page);
        mm_free_page(page);
        tmpfs_drop(backing, mapped.first_cluster);
    }
    for (unsigned i = 0; i < count; ++i) {
        task_shm_release(&files[i]);
        storage_inode_put(files[i].inode);
    }
    tmpfs_destroy(backing);
    puts("PASS memfd resources");
}
