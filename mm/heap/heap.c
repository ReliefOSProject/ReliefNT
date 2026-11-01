/* ReliefOS kernel heap: zeroed slab slots and contiguous page-backed runs. */
#include <reliefnt/export.h>
#include <reliefnt/heap.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include "heap_internal.h"

#define KERNEL_PAGE_SIZE 4096u
#define KERNEL_SLAB_MAX 2048u

/** Private metadata at the beginning of each owned large contiguous run. */
struct kernel_heap_header {
    struct heap_tree_node tree;
    uint64_t phys;
    uint32_t pages;
    uint32_t reserved;
    uint64_t padding;
};
_Static_assert(sizeof(struct kernel_heap_header) % 16u == 0, "large heap payload must be aligned");
static struct kernel_spinlock large_lock = KERNEL_SPINLOCK_INIT;
static struct heap_tree_node *large_allocations;

/** @brief Report initialization of the zero-preserving kernel heap.
 * @return None; the physical page allocator supplies pages on demand.
 */
void kernel_heap_init(void)
{
    console_info("[memory] kernel heap allocator=slab+page-backed page_bytes=%u slab_max=%u\n",
                 KERNEL_PAGE_SIZE, KERNEL_SLAB_MAX);
}

/** @brief Allocate zeroed kernel memory with at least 16-byte alignment.
 * @param size Requested payload bytes. @return Owned payload, or NULL on invalid size/ENOMEM.
 * Small allocations use slab slots; larger allocations own a contiguous physical run.
 */
void *kernel_malloc(size_t size)
{
    uint64_t total, page_count, phys, flags;
    struct kernel_heap_header *header;
    if (!size) return NULL;
    if (size <= KERNEL_SLAB_MAX) return slab_alloc_zero(size);
    if (size > UINT64_MAX - sizeof(*header)) return NULL;
    total = (uint64_t)size + sizeof(*header);
    page_count = (total - 1u) / KERNEL_PAGE_SIZE + 1u;
    if (page_count > UINT32_MAX) return NULL;
    phys = mm_alloc_pages((uint32_t)page_count);
    if (!phys) return NULL;
    header = mm_phys_to_virt(phys);
    if (!header || ((uintptr_t)header & (KERNEL_PAGE_SIZE - 1u))) {
        mm_free_pages(phys, (uint32_t)page_count);
        return NULL;
    }
    header->tree.left = header->tree.right = NULL;
    header->tree.height = 1;
    header->phys = phys;
    header->pages = (uint32_t)page_count;
    kernel_spin_lock_irqsave(&large_lock, &flags);
    large_allocations = heap_tree_insert(large_allocations, &header->tree);
    kernel_spin_unlock_irqrestore(&large_lock, flags);
    return header + 1;
}

/** @brief Release one live kernel heap allocation.
 * @param memory Original kernel_malloc result or NULL; invalid pointers are ignored.
 * The allocator detaches ownership under its lock and releases pages afterwards.
 */
void kernel_free(void *memory)
{
    uint64_t flags, phys = 0;
    uint32_t pages = 0;
    uintptr_t address = (uintptr_t)memory;
    if (!memory || slab_free(memory) || address < sizeof(struct kernel_heap_header)) return;
    uintptr_t key = address - sizeof(struct kernel_heap_header);
    kernel_spin_lock_irqsave(&large_lock, &flags);
    struct kernel_heap_header *header = (struct kernel_heap_header *)heap_tree_find(large_allocations, key);
    if (header) {
        large_allocations = heap_tree_remove(large_allocations, key);
        phys = header->phys;
        pages = header->pages;
    }
    kernel_spin_unlock_irqrestore(&large_lock, flags);
    if (phys) mm_free_pages(phys, pages);
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(kernel_free);
EXPORT_SYMBOL(kernel_malloc);
