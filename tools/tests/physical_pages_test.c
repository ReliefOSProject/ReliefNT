#define _GNU_SOURCE
#include <reliefnt/console.h>
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../mm/physical/mm.c"

char __kernel_start[1], __kernel_end[1];

void console_printf(const char *format, ...) { (void)format; }
void *paging_kernel_direct_map(uint64_t phys) { return (void *)(uintptr_t)phys; }
void paging_mmio_allocator_ready(void) { }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
static struct framebuffer framebuffer_fixture;
const struct framebuffer *framebuffer_get(void) { return &framebuffer_fixture; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
static uint64_t private_zero_base;
static unsigned private_zero_checks;
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    assert(lock->state);
    lock->state = 0;
    if (lock == &mm_lock && private_zero_base) {
        uint64_t base = private_zero_base;
        private_zero_base = 0;
        /* Clearing a large private allocation must not hold mm_lock. At
         * unlock its old contents remain, but every page is already owned. */
        assert(*(const unsigned char *)(uintptr_t)base == 0xa5);
        for (unsigned i = 0; i < 8; ++i) {
            uint32_t page = page_index(base) + i;
            assert(page_refs[page] == 1);
            assert(!(free_pages[page / 64] & (1ULL << (page % 64))));
        }
        /* Model a second allocator entering before the first caller has
         * received its zeroed run: the private pages must not be reused. */
        uint64_t other = mm_alloc_page();
        assert(other && (other < base || other >= base + 8 * PAGE_SIZE));
        mm_free_page(other);
        ++private_zero_checks;
    }
}

int main(void)
{
    const uint64_t base = 0x60000000, count = 2048, bytes = count * PAGE_SIZE;
    const uint64_t directmap_phys = 0x12345000ULL;
    assert(mm_virt_to_phys((const void *)(uintptr_t)RELIEFNT_KERNEL_HOLE_START) ==
           RELIEFNT_KERNEL_HOLE_START);
    assert(mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_HOLE_END - 1)) ==
           RELIEFNT_KERNEL_HOLE_END - 1);
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_KERNEL_HOLE_START - 1)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)RELIEFNT_KERNEL_HOLE_END));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)0x00400000ULL));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)(RELIEFNT_USER_TOP - 1)));
    assert(mm_virt_to_phys((const void *)(uintptr_t)
               (RELIEFNT_KERNEL_DIRECT_MAP_BASE + directmap_phys)) == directmap_phys);
    assert(mm_virt_to_phys((const void *)(uintptr_t)
               (RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE - 1)) ==
           RELIEFNT_KERNEL_DIRECT_MAP_SIZE - 1);
    assert(!mm_virt_to_phys((const void *)(uintptr_t)
               (RELIEFNT_KERNEL_DIRECT_MAP_BASE - 1)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)
               (RELIEFNT_KERNEL_DIRECT_MAP_BASE + RELIEFNT_KERNEL_DIRECT_MAP_SIZE)));
    assert(!mm_virt_to_phys((const void *)(uintptr_t)UINT64_MAX));
    assert(mmap((void *)base, bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void *)base);
    struct multiboot2_mmap_entry entry = {.addr = base, .len = bytes, .type = 1};
    struct boot_info boot = {.mmap_addr = (uintptr_t)&entry, .mmap_entry_count = 1,
                             .mmap_entry_size = sizeof(entry)};
    mm_init(&boot);
    const uint64_t usable_base = base + MM_PAGE_COUNT / 8;
    const uint64_t usable_count = count - (MM_PAGE_COUNT / 8) / PAGE_SIZE;
    const uint64_t usable_bytes = usable_count * PAGE_SIZE;
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    for (unsigned round = 0; round < 3; ++round) {
        uint64_t all = mm_alloc_pages(usable_count);
        assert(all == usable_base && !mm_free_memory_kib());
        for (unsigned i = 0; i < usable_count; i += 2) mm_free_page(usable_base + i * PAGE_SIZE);
        assert(mm_free_memory_kib() == usable_bytes / 2048);
        for (unsigned i = 1; i < usable_count; i += 2) mm_free_page(usable_base + i * PAGE_SIZE);
        assert(mm_free_memory_kib() == usable_bytes / 1024);
    }
    uint64_t page = mm_alloc_page();
    mm_retain_page(page);
    mm_free_page(page);
    assert(mm_free_memory_kib() == usable_bytes / 1024 - 4);
    mm_free_page(page);
    mm_free_page(page);
    assert(mm_free_memory_kib() == usable_bytes / 1024);

    /* Exercise ownership counts beyond the old 16-bit saturation point.  The
     * page must stay unavailable until every retained owner releases it. */
    uint64_t heavily_shared = mm_alloc_page();
    assert(heavily_shared);
    for (uint32_t owner = 0; owner < 65536; ++owner) {
        mm_retain_page(heavily_shared);
    }
    for (uint32_t owner = 0; owner < 65535; ++owner) {
        mm_free_page(heavily_shared);
    }
    uint64_t competing_page = mm_alloc_page();
    assert(competing_page && competing_page != heavily_shared);
    mm_free_page(competing_page);
    mm_free_page(heavily_shared);
    competing_page = mm_alloc_page();
    assert(competing_page && competing_page != heavily_shared);
    mm_free_page(competing_page);
    mm_free_page(heavily_shared);
    assert(mm_alloc_page() == heavily_shared);
    mm_free_page(heavily_shared);

    /* The unrepresentable-refcount fallback pins rather than freeing early. */
    uint64_t overflow_page = mm_alloc_page();
    assert(overflow_page);
    uint32_t overflow_index = page_index(overflow_page);
    page_refs[overflow_index] = UINT32_MAX - 1;
    mm_retain_page(overflow_page);
    assert(page_refs[overflow_index] == MM_PAGE_REF_PINNED);
    mm_free_page(overflow_page);
    competing_page = mm_alloc_page();
    assert(competing_page && competing_page != overflow_page);
    mm_free_page(competing_page);
    page_refs[overflow_index] = 1; /* Model the unreachable full drain. */
    mm_free_page(overflow_page);
    assert(mm_alloc_page() == overflow_page);
    mm_free_page(overflow_page);

    assert(!mm_alloc_pages(count + 1));
    uint64_t block = mm_alloc_pages(8);
    assert(block == usable_base);
    memset((void *)block, 0xa5, 8 * PAGE_SIZE);
    mm_free_page(block);
    mm_free_pages(block + 3 * PAGE_SIZE, 3);
    assert(mm_alloc_pages(3) == block + 3 * PAGE_SIZE);
    assert(mm_alloc_page() == block);
    for (unsigned i = 0; i < PAGE_SIZE; ++i) assert(!((unsigned char *)block)[i]);
    mm_free_pages(block, 8);
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    block = mm_alloc_pages(8);
    memset((void *)(uintptr_t)block, 0xa5, 8 * PAGE_SIZE);
    mm_free_pages(block, 8);
    private_zero_base = block;
    assert(mm_alloc_pages(8) == block);
    assert(!private_zero_base && private_zero_checks == 1);
    for (unsigned i = 0; i < 8 * PAGE_SIZE; ++i)
        assert(!((const unsigned char *)(uintptr_t)block)[i]);
    mm_free_pages(block, 8);
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    mm_free_page(base); /* Bitmap storage remains reserved. */
    assert(mm_free_memory_kib() == usable_bytes / 1024);
    munmap((void *)base, bytes);

    /* The allocator's framebuffer fallback receives a CPU direct-map alias.
     * It must reserve the corresponding physical page, not the alias bits. */
    const uint64_t fb_mem_base = 0x70000000ULL;
    const uint64_t fb_mem_bytes = 2048ULL * PAGE_SIZE;
    assert(mmap((void *)fb_mem_base, fb_mem_bytes, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
           (void *)fb_mem_base);
    struct multiboot2_mmap_entry fb_entry = {
        .addr = fb_mem_base, .len = fb_mem_bytes, .type = 1,
    };
    struct boot_info fb_boot = {
        .mmap_addr = (uintptr_t)&fb_entry,
        .mmap_entry_count = 1,
        .mmap_entry_size = sizeof(fb_entry),
    };
    const uint64_t fb_phys = fb_mem_base + 0x100000ULL;
    framebuffer_fixture = (struct framebuffer){
        .pixels = (uint32_t *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + fb_phys),
        .pitch = PAGE_SIZE,
        .height = 1,
        .available = true,
    };
    mm_init(&fb_boot);
    uint32_t fb_page_index = page_index(fb_phys);
    assert(!(free_pages[fb_page_index / 64] & (1ULL << (fb_page_index % 64))));
    assert(munmap((void *)fb_mem_base, fb_mem_bytes) == 0);
    puts("PASS physical pages: >256 free fragments, repeated full reclamation, contiguous allocation and refs");
}
