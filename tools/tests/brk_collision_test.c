/* A Linux brk extension must never replace a VMA already in the process. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <reliefnt/audio.h>
#include <reliefnt/console.h>
#include <reliefnt/drm.h>
#include <reliefnt/syscall.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall_internal.h>
#include "sysv_shm_fixture_stubs.h"

int64_t syscall_mm_brk(uint64_t requested);
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset);
int64_t syscall_mm_munmap(uint64_t addr, uint64_t len);

static struct task *current_task;
static struct task_file framebuffer_file;
static struct framebuffer framebuffer_device;
static const uint64_t framebuffer_alias = 0xffffff8000200000ULL;
static const uint64_t framebuffer_physical = 0x42000000ULL;
static unsigned eager_range_prepares;
static unsigned page_alloc_attempts;
static uint64_t last_device_mapping_phys;
static uint64_t file_mount_flags;
static int file_writable = 1;
struct task *sched_current_task(void) { return current_task; }

uint32_t sched_task_vma_capacity(const struct task *task)
{
    (void)task;
    return SCHED_TASK_VMA_MAX;
}

struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    return index < SCHED_TASK_VMA_MAX ? &sched_task_mm(task)->vmas[index] : NULL;
}

uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t address)
{
    (void)as;
    (void)address;
    return 0;
}

bool address_space_user_page_is_device(const struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return 0; }
bool address_space_prepare_user_range(struct address_space *as, uint64_t start, uint64_t end)
{ (void)as; (void)start; (void)end; ++eager_range_prepares; return true; }
bool address_space_map_user_page(struct address_space *as, uint64_t address, uint64_t phys, uint64_t flags)
{ (void)as; (void)address; (void)flags; last_device_mapping_phys = phys; return true; }
bool address_space_user_range_free(const struct address_space *as, uint64_t start, uint64_t end)
{ (void)as; (void)start; (void)end; return true; }
bool address_space_user_page_next(const struct address_space *as, uint64_t start,
                                  uint64_t end, uint64_t *page, uint64_t *phys,
                                  uint64_t *flags)
{
    (void)as; (void)start; (void)end;
    if (page) *page = 0;
    if (phys) *phys = 0;
    if (flags) *flags = 0;
    return false;
}
void smp_flush_user_tlb(void) {}
int page_cache_owns(uint64_t phys) { (void)phys; return 0; }
void page_cache_release(uint64_t phys) { (void)phys; }
void mm_free_page(uint64_t phys) { (void)phys; }
uint64_t mm_alloc_page(void) { ++page_alloc_attempts; return 0; }
uint64_t mm_free_memory_kib(void) { return UINT64_MAX; }
int storage_inode_put(struct storage_inode_ref *reference) { (void)reference; return 0; }
void storage_inode_retain(struct storage_inode_ref *reference) { (void)reference; }
int storage_inode_get(const struct storage_node *node, struct storage_inode_ref **out)
{ (void)node; *out = NULL; return 0; }
int storage_inode_refresh(struct storage_node *node) { (void)node; return 0; }
int storage_node_mount_flags(const struct storage_node *node, uint64_t *flags)
{ (void)node; *flags = file_mount_flags; return 0; }
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd == 7 ? &framebuffer_file : NULL; }
int file_can_read(const struct task_file *file) { return file == &framebuffer_file; }
int file_can_write(const struct task_file *file) { return file == &framebuffer_file && file_writable; }
int task_shm_mmap_check(const struct task_file *file, uint32_t flags, uint32_t prot, uint32_t *maximum)
{ (void)file; (void)flags; (void)prot; (void)maximum; return -1; }
const struct framebuffer *framebuffer_get(void) { return &framebuffer_device; }
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    return value >= framebuffer_alias && value - framebuffer_alias < 4096
               ? framebuffer_physical + value - framebuffer_alias : 0;
}
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    return phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE &&
           len <= RELIEFNT_KERNEL_DIRECT_MAP_SIZE - phys;
}
void console_printf(const char *fmt, ...) { (void)fmt; }

void console_log(enum console_log_level level, const char *fmt, ...)
{
    (void)level; (void)fmt;
}
void *kernel_malloc(size_t size) { return __builtin_malloc(size); }
void kernel_free(void *ptr) { __builtin_free(ptr); }
void drm_bo_get(struct drm_bo *bo) { (void)bo; }
void drm_bo_put(struct drm_bo *bo) { (void)bo; }
uint64_t drm_bo_page_physical(const struct drm_bo *bo, uint64_t offset)
{ (void)bo; (void)offset; return 0; }
int drm_file_mmap_bo(struct task_file *file, uint64_t offset, uint64_t length,
                     struct drm_bo **bo, uint64_t *bo_offset)
{ (void)file; (void)offset; (void)length; (void)bo; (void)bo_offset; return -1; }
int drm_prime_file_mmap(struct task_file *file, uint64_t flags,
                        uint64_t protection)
{ (void)file; (void)flags; (void)protection; return -1; }
struct task_file *task_file_get(struct task_file *source) { return source; }
void task_file_put(struct task_file *description) { (void)description; }
int audio_device_mmap(struct task *task, struct task_file *file,
                      uint64_t offset, uint64_t length, uint32_t prot,
                      struct audio_pcm_mmap *mapping)
{ (void)task; (void)file; (void)offset; (void)length; (void)prot; (void)mapping; return -1; }
int audio_pcm_mmap_retain(struct audio_pcm *pcm,
                          enum audio_pcm_mmap_region region)
{ (void)pcm; (void)region; return 0; }
void audio_pcm_mmap_release(struct audio_pcm *pcm,
                            enum audio_pcm_mmap_region region)
{ (void)pcm; (void)region; }

int main(void)
{
    const uint64_t page = 4096;
    const uint64_t start = RELIEFNT_USER_HEAP_BASE + 0x20000;
    struct task task = {0};
    task.kind = TASK_KIND_USER;
    task.stack_top = RELIEFNT_USER_TOP - page;
    task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    task.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    task.program_break_base = task.program_break = start;
    task.address_space.vmas[0] = (struct task_vma){
        .used = 1, .start = start + page, .end = start + 2 * page,
        .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_READ,
    };
    current_task = &task;

    /* Linux permits an explicit anonymous W|X reservation for JIT runtimes.
     * Admission remains lazy and unknown protection bits remain invalid. */
    const uint64_t jit_start = 0x0000000040000000ULL;
    assert(syscall_mm_mmap(jit_start, page,
        LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0) == (int64_t)jit_start);
    assert(!eager_range_prepares && !page_alloc_attempts);
    assert(syscall_mm_munmap(jit_start, page) == 0);
    assert(syscall_mm_mmap(jit_start, page, 0x80,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0) == -RELIEFOS_EINVAL);

    framebuffer_file.node.type = RELIEFOS_FS_TYPE_FILE;
    framebuffer_file.node.flags = STORAGE_NODE_FLAG_EXT2;
    framebuffer_file.node.size = page;
    file_mount_flags = MS_NOEXEC;
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EPERM);
    file_mount_flags = MS_RDONLY;
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EROFS);
    file_mount_flags = 0;
    file_writable = 0;
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EACCES);
    file_writable = 1;

    /* Linux brk(2) returns the old break when an existing VMA blocks growth. */
    int64_t result = syscall_mm_brk(start + 2 * page);
    if (result != (int64_t)start) {
        fprintf(stderr, "brk collision returned %#llx, expected %#llx\n",
                (unsigned long long)result, (unsigned long long)start);
    }
    assert(result == (int64_t)start);
    assert(task.program_break == start);
    assert(task.address_space.vmas[0].used);
    assert(task.address_space.vmas[0].start == start + page);
    assert(task.address_space.vmas[0].end == start + 2 * page);
    assert(task.address_space.vmas[0].prot == LINUX_PROT_READ);

    /* A sparse VMA may reserve a 1 TiB span without allocating page tables
     * or walking each page on either publication or munmap. */
    const uint64_t sparse_start = 0x0000010000000000ULL;
    const uint64_t sparse_length = 1ULL << 40;
    struct task sparse = {0};
    sparse.kind = TASK_KIND_USER;
    sparse.stack_top = RELIEFNT_USER_TOP - page;
    sparse.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    sparse.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    current_task = &sparse;
    unsigned prepares_before_sparse = eager_range_prepares;
    unsigned page_allocs_before_sparse = page_alloc_attempts;
    int64_t sparse_mapped = syscall_mm_mmap(sparse_start, sparse_length,
        LINUX_PROT_READ | LINUX_PROT_WRITE,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0);
    assert(sparse_mapped == (int64_t)sparse_start);
    assert(eager_range_prepares == prepares_before_sparse);
    assert(page_alloc_attempts == page_allocs_before_sparse);
    assert(sparse.address_space.vmas[0].used &&
           sparse.address_space.vmas[0].start == sparse_start &&
           sparse.address_space.vmas[0].end == sparse_start + sparse_length);
    assert(syscall_mm_munmap(sparse_start, sparse_length) == 0);
    assert(!sparse.address_space.vmas[0].used);
    assert(eager_range_prepares == prepares_before_sparse);
    assert(page_alloc_attempts == page_allocs_before_sparse);
    puts("PASS sparse 1 TiB mmap reservation: lazy page tables and bounded munmap traversal");

    /* Framebuffer CPU pointers are high direct-map aliases; an fbdev mmap
     * must install the backing PFN, never the alias bits, in the user PTE. */
    struct task fb_task = {0};
    fb_task.kind = TASK_KIND_USER;
    fb_task.stack_top = RELIEFNT_USER_TOP - page;
    fb_task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    fb_task.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    current_task = &fb_task;
    memset(&framebuffer_file, 0, sizeof(framebuffer_file));
    framebuffer_file.flags = TASK_FILE_FLAG_DEV_NODE;
    framebuffer_file.node.first_cluster = STORAGE_DEV_KIND_FB0;
    memset(&framebuffer_device, 0, sizeof(framebuffer_device));
    framebuffer_device.available = true;
    framebuffer_device.pixels = (uint32_t *)(uintptr_t)framebuffer_alias;
    framebuffer_device.pitch = 4096;
    framebuffer_device.height = 2;
    framebuffer_device.max_bytes = 8192;
    const uint64_t fb_vaddr = 0x0000000030000000ULL;
    last_device_mapping_phys = 0;
    int64_t fb_mapped = syscall_mm_mmap(fb_vaddr, page,
        LINUX_PROT_READ | LINUX_PROT_WRITE,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, page);
    if (fb_mapped != (int64_t)fb_vaddr)
        fprintf(stderr, "fbdev mmap returned %lld\n", (long long)fb_mapped);
    assert(fb_mapped == (int64_t)fb_vaddr);
    assert(last_device_mapping_phys == framebuffer_physical + page);
    assert(last_device_mapping_phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE);
    puts("PASS fbdev mmap converts the high CPU alias to a physical PFN");

    /* CLONE_VM threads must observe the same process break, just as they
     * observe the same mappings. The local copy models the task state made
     * by clone before both tasks point at the promoted shared mm. */
    struct task process = {0};
    struct task thread;
    struct task_address_space_state shared_mm;
    process.kind = TASK_KIND_USER;
    process.stack_top = RELIEFNT_USER_TOP - page;
    process.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    process.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    process.program_break_base = process.program_break = start;
    thread = process;
    shared_mm = process.address_space;
    process.shared_mm = thread.shared_mm = &shared_mm;

    current_task = &process;
    result = syscall_mm_brk(start + page);
    if (result != (int64_t)(start + page)) {
        fprintf(stderr, "shared brk growth returned %#llx, current=%#llx base=%#llx\n",
                (unsigned long long)result,
                (unsigned long long)process.program_break,
                (unsigned long long)process.program_break_base);
    }
    assert(result == (int64_t)(start + page));
    current_task = &thread;
    assert(syscall_mm_brk(0) == (int64_t)(start + page));
    puts("PASS brk: collision protection and CLONE_VM shared break");
    return 0;
}
