#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/lock.h>
#define RELIEFNT_TEST_REAL_EXEC_SOURCE
#include "modules/user_memory.h"
#include "sysv_shm_fixture_stubs.h"
#include "exec_file_fixture.h"

static unsigned char file_bytes[8192];
static uint64_t mapped;
static uint64_t mapped_flags;
static unsigned allocations;
static bool io_async;
static int read_error;
static bool short_read;
static unsigned reads;
static bool fail_metadata;
static bool verify_shared_grant;

void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
void *kernel_malloc(size_t size) { return fail_metadata ? NULL : calloc(1, size); }
void kernel_free(void *memory) { free(memory); }

uint64_t mm_alloc_page(void)
{
    void *p = aligned_alloc(4096, 4096);
    assert(p);
    memset(p, 0, 4096);
    ++allocations;
    return (uintptr_t)p;
}
void mm_free_page(uint64_t p) { assert(allocations); --allocations; free((void *)(uintptr_t)p); }
void *mm_phys_to_virt(uint64_t phys) { return (void *)(uintptr_t)phys; }
int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buffer, uint32_t size, uint32_t *got)
{(void)node;(void)offset;(void)buffer;(void)size;(void)got;abort();}
static int exec_fixture_read(void *context,const struct storage_node *node,uint64_t offset,
                               void *buffer,uint32_t size,uint32_t *got,uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context==&exec_fixture_pins && exec_fixture_pins && exec_fixture_denials);
    assert(node->size == sizeof(file_bytes) && offset + size <= node->size);
    ++reads;
    *got = 0;
    if (io_async) return -RELIEFOS_EAGAIN;
    if (read_error) return read_error;
    if (short_read) --size;
    memcpy(buffer, file_bytes + offset, size);
    *got = size;
    return 0;
}
void storage_set_io_async_context(bool enabled) { io_async = enabled; }
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; abort(); }
int storage_errno(int status) { return status; }
bool address_space_map_user_page(struct address_space *as, uint64_t va,
                                 uint64_t phys, uint64_t flags)
{
    (void)as; (void)va;
    if (verify_shared_grant) {
        assert((flags & (RELIEFNT_PAGE_WRITE_NOTIFY | RELIEFNT_PAGE_SHARED | RELIEFNT_PAGE_WRITABLE)) ==
               (RELIEFNT_PAGE_WRITE_NOTIFY | RELIEFNT_PAGE_SHARED));
        page_cache_release(phys);
        assert(page_cache_reclaim(1) == 0);
        assert(!page_cache_retain(phys));
        verify_shared_grant = false;
    }
    mapped_flags = flags; mapped = phys; return true;
}
void kernel_spin_init(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }

/** @brief A shared writable fault retains changed data after unmap and invalidation.
 * @return None; assertions exercise the actual file-fault and cache paths.
 */
static void test_shared_writable_invalidation(void)
{
    struct task task = {0};
    struct task_vma vma = {
        .used = 1, .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_MMAP |
                 TASK_VMA_FLAG_SHARED | TASK_VMA_FLAG_SHARED_FILE,
        .start = 0x5000000, .end = 0x5001000, .file_limit = sizeof(file_bytes),
        .file_node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
                      .size = sizeof(file_bytes)}
    };
    struct vfs_exec_source *source = exec_fixture_open(&vma.file_node);
    vma.exec_source = source;
    memset(file_bytes, 0x53, sizeof(file_bytes));
    page_cache_init();
    verify_shared_grant = true;
    assert(task_map_file_vma_page(&task, &vma, vma.start) == 0);
    assert(!verify_shared_grant);
    assert((mapped_flags & (RELIEFNT_PAGE_WRITE_NOTIFY | RELIEFNT_PAGE_WRITABLE)) ==
           RELIEFNT_PAGE_WRITE_NOTIFY);
    struct page_cache_report report; page_cache_report(&report); assert(!report.dirty);
    uint64_t original = mapped, alias = 0;
    ((unsigned char *)(uintptr_t)original)[42] = 0xa7;
    page_cache_note_write(original); /* This fixture writes via the physical alias. */
    page_cache_report(&report); assert(report.dirty==1);
    page_cache_release(original);
    page_cache_invalidate_node(&vma.file_node);
    assert(page_cache_lookup(&vma.file_node, 0, &alias) == 0);
    assert(alias == original && ((unsigned char *)(uintptr_t)alias)[42] == 0xa7);
    page_cache_release(alias);
    vfs_exec_source_put(source);
}

/** @brief A writable private cached mapping is installed copy-on-write.
 * @return None; the shared cache frame remains clean and independently owned.
 */
static void test_private_cache_cow(void)
{
    struct task task = {0};
    struct task_vma vma = {
        .used = 1, .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_PRIVATE | TASK_VMA_FLAG_SHARED_FILE,
        .start = 0x5000000, .end = 0x5001000, .file_limit = sizeof(file_bytes),
        .file_node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
                      .size = sizeof(file_bytes)}
    };
    struct vfs_exec_source *source = exec_fixture_open(&vma.file_node);
    vma.exec_source = source; page_cache_init();
    assert(!task_map_file_vma_page(&task, &vma, vma.start));
    assert((mapped_flags & RELIEFNT_PAGE_COW) && !(mapped_flags & RELIEFNT_PAGE_WRITABLE));
    page_cache_release(mapped); assert(page_cache_reclaimable_pages() == 1);
    page_cache_invalidate_node(&vma.file_node); assert(!allocations);
    vfs_exec_source_put(source);
}

/** @brief A clean reclaimed page can be reconstructed from the real VFS source.
 * @return None; verifies another provider read and every byte of the new frame.
 */
static void test_clean_reload(void)
{
    struct task task = {0};
    struct task_vma vma = {
        .used = 1, .prot = LINUX_PROT_READ,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_SHARED_FILE,
        .start = 0x5000000, .end = 0x5001000, .file_limit = sizeof(file_bytes),
        .file_node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
                      .size = sizeof(file_bytes)}
    };
    for (unsigned i=0;i<sizeof(file_bytes);++i) file_bytes[i]=(unsigned char)(i%251);
    struct vfs_exec_source *source=exec_fixture_open(&vma.file_node);
    vma.exec_source=source; page_cache_init();
    assert(!task_map_file_vma_page(&task,&vma,vma.start));
    assert(!memcmp((void *)(uintptr_t)mapped,file_bytes,4096));
    page_cache_release(mapped); unsigned before=reads;
    assert(page_cache_reclaim(1)==1 && !allocations);
    assert(!task_map_file_vma_page(&task,&vma,vma.start));
    assert(reads==before+1 && !memcmp((void *)(uintptr_t)mapped,file_bytes,4096));
    page_cache_release(mapped); page_cache_invalidate_node(&vma.file_node);
    assert(!allocations); vfs_exec_source_put(source);
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "shared-writable")) {
        test_shared_writable_invalidation();
        puts("PASS shared writable fault preserves modified cache data");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "private-cow")) {
        test_private_cache_cow();
        puts("PASS writable private cache mapping uses copy-on-write");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "clean-reload")) {
        test_clean_reload();
        puts("PASS reclaimed clean file page reloads from the VFS provider");
        return 0;
    }
    struct task task = {0};
    struct task_vma prefix = {
        .used = 1, .prot = LINUX_PROT_READ,
        .flags = TASK_VMA_FLAG_FILE | TASK_VMA_FLAG_LAZY | TASK_VMA_FLAG_SHARED_FILE,
        .start = 0x5000000, .end = 0x5001000, .file_offset = 0,
        .file_limit = 0x800, .file_node = {.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_FAT32,.size = sizeof(file_bytes)}
    };
    struct vfs_exec_source *source=exec_fixture_open(&prefix.file_node);
    prefix.exec_source=source;
    struct task_vma text = prefix;
    text.start += 4096;
    text.end += 4096;
    text.prot |= LINUX_PROT_EXEC;
    text.file_limit = sizeof(file_bytes);
    memset(file_bytes, 0x53, sizeof(file_bytes));
    page_cache_init();
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &prefix, prefix.start) == 0);
    uint64_t prefix_page = mapped;
    assert(((unsigned char *)(uintptr_t)prefix_page)[0] == 0x53);
    assert(((unsigned char *)(uintptr_t)prefix_page)[0x900] == 0);
    /* usercopy can fault on a cold ELF page while syscall I/O is async. */
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    uint64_t text_page = mapped;
    assert(((unsigned char *)(uintptr_t)text_page)[0x900] == 0x53);
    assert(text_page != prefix_page);
    unsigned reads_before = reads;
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    assert(mapped == text_page);
    assert(reads == reads_before);

    struct task_vma cold = text;
    cold.file_offset = 4096;
    unsigned allocated_before = allocations;
    read_error = -RELIEFOS_EIO;
    assert(task_map_file_vma_page(&task, &cold, cold.start) == -RELIEFOS_EIO);
    assert(allocations == allocated_before && mapped == text_page);
    read_error = 0;
    short_read = true;
    assert(task_map_file_vma_page(&task, &cold, cold.start) == -RELIEFOS_EIO);
    assert(allocations == allocated_before && mapped == text_page);
    short_read = false;
    storage_set_io_async_context(true);
    assert(task_map_file_vma_page(&task, &cold, cold.start) == 0);
    assert(memcmp((void *)(uintptr_t)mapped, file_bytes + 4096, 4096) == 0);
    page_cache_release(mapped);
    page_cache_release(text_page);
    page_cache_release(text_page);
    if (page_cache_owns(prefix_page)) page_cache_release(prefix_page);
    else mm_free_page(prefix_page);
    struct storage_node resized = prefix.file_node;
    resized.size += 4096;
    page_cache_invalidate_node(&resized);
    assert(!allocations);
    /* Every cache slot is pinned; another executable page must still load. */
    uint64_t pinned[4096];
    for (unsigned i = 0; i < 4096; ++i) {
        text.file_node.first_cluster = i + 1;
        assert(task_map_file_vma_page(&task, &text, text.start) == 0);
        pinned[i] = mapped;
    }
    text.file_node.first_cluster = 4097;
    struct task_vma shared = text;
    shared.flags |= TASK_VMA_FLAG_SHARED;
    assert(task_map_file_vma_page(&task, &shared, shared.start) == 0);
    uint64_t shared_page = mapped;
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    assert(mapped == shared_page && page_cache_owns(mapped));
    assert(memcmp((void *)(uintptr_t)mapped, file_bytes, 4096) == 0);
    page_cache_release(mapped);
    page_cache_release(shared_page); /* Empty overflow metadata is reclaimed. */
    fail_metadata = true;
    unsigned pinned_allocations = allocations;
    assert(task_map_file_vma_page(&task, &shared, shared.start) == -RELIEFOS_EIO);
    assert(allocations == pinned_allocations);
    assert(task_map_file_vma_page(&task, &text, text.start) == 0);
    assert(!page_cache_owns(mapped));
    mm_free_page(mapped);
    fail_metadata = false;
    for (unsigned i = 0; i < 4096; ++i) {
        page_cache_release(pinned[i]);
        text.file_node.first_cluster = i + 1;
        page_cache_invalidate_node(&text.file_node);
    }
    assert(!allocations);
    vfs_exec_source_put(source);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
    puts("PASS pinned cache growth: shared identity, allocation failure and private fallback without leaks");
    puts("PASS ELF segments sharing a file page: independent zero-fill and intact code cache");
    puts("PASS cold ELF usercopy faults: synchronous I/O, cache hits, errors and short-read cleanup");
}
