/* Page-owned, zero-on-allocation small-object heap. */
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/heap.h>
#include "heap_internal.h"

#define SLAB_PAGE_SIZE 4096u
#define SLAB_CLASS_COUNT 8u
#define SLAB_BITMAP_WORDS 4u

/** Metadata lives before, and separately from, all slot payloads. */
struct slab_page {
    struct heap_tree_node tree;
    struct slab_page *partial_prev;
    struct slab_page *partial_next;
    uint64_t phys;
    uint64_t used[SLAB_BITMAP_WORDS];
    uint16_t class_index;
    uint16_t slot_count;
    uint16_t active;
};

#define SLAB_DATA_OFFSET ((sizeof(struct slab_page) + 15u) & ~(size_t)15u)
_Static_assert(SLAB_DATA_OFFSET < SLAB_PAGE_SIZE, "slab metadata must fit in one page");
_Static_assert((SLAB_PAGE_SIZE - SLAB_DATA_OFFSET) / 16u <= SLAB_BITMAP_WORDS * 64u,
               "slab bitmap must represent every slot");
static const uint16_t slab_sizes[SLAB_CLASS_COUNT] = {16, 32, 64, 128, 256, 512, 1024, 2048};
static struct kernel_spinlock slab_lock = KERNEL_SPINLOCK_INIT;
static struct heap_tree_node *slab_pages;
static struct slab_page *partial[SLAB_CLASS_COUNT];

/** @brief Find the smallest fitting size class. @param size Nonzero requested bytes.
 * @return Class index, or SLAB_CLASS_COUNT for an unsupported size.
 */
static unsigned size_class(size_t size)
{
    for (unsigned i = 0; i < SLAB_CLASS_COUNT; ++i)
        if (size <= slab_sizes[i]) return i;
    return SLAB_CLASS_COUNT;
}

uint64_t kernel_heap_small_page_bound(size_t size, uint64_t count)
{
    unsigned c = size_class(size);
    if (!size || c == SLAB_CLASS_COUNT) return UINT64_MAX;
    uint64_t slots = (SLAB_PAGE_SIZE - SLAB_DATA_OFFSET) / slab_sizes[c];
    return count / slots + (count % slots != 0);
}

/** @brief Publish a nonfull page to its class. @param page Owned page under slab_lock. */
static void partial_add(struct slab_page *page)
{
    unsigned c = page->class_index;
    page->partial_prev = NULL;
    page->partial_next = partial[c];
    if (partial[c]) partial[c]->partial_prev = page;
    partial[c] = page;
}

/** @brief Remove a nonfull page from its class. @param page Linked page under slab_lock. */
static void partial_remove(struct slab_page *page)
{
    if (page->partial_prev) page->partial_prev->partial_next = page->partial_next;
    else partial[page->class_index] = page->partial_next;
    if (page->partial_next) page->partial_next->partial_prev = page->partial_prev;
    page->partial_prev = page->partial_next = NULL;
}

/** @brief Reserve a free slot while excluding page release.
 * @param page Nonfull owned slab under slab_lock. @return Reserved but not yet cleared payload.
 */
static void *reserve_slot(struct slab_page *page)
{
    for (unsigned i = 0; i < page->slot_count; ++i) {
        uint64_t bit = UINT64_C(1) << (i % 64u);
        if (!(page->used[i / 64u] & bit)) {
            page->used[i / 64u] |= bit;
            if (++page->active == page->slot_count) partial_remove(page);
            return (uint8_t *)page + SLAB_DATA_OFFSET + (size_t)i * slab_sizes[page->class_index];
        }
    }
    return NULL;
}

/** @brief Reserve and clear a size-class slot.
 * @param size Requested bytes from 1 through 2048. @return Zeroed payload or NULL.
 * Allocates, releases and clears pages outside slab_lock; reserved slots pin their page.
 */
void *slab_alloc_zero(size_t size)
{
    unsigned c = size_class(size);
    uint64_t flags, phys = 0;
    struct slab_page *fresh = NULL, *page;
    void *result = NULL;
    if (!size || c == SLAB_CLASS_COUNT) return NULL;

    kernel_spin_lock_irqsave(&slab_lock, &flags);
    page = partial[c];
    if (page) result = reserve_slot(page);
    kernel_spin_unlock_irqrestore(&slab_lock, flags);
    if (result) goto clear;

    phys = mm_alloc_pages(1);
    if (phys) {
        fresh = mm_phys_to_virt(phys);
        if (!fresh || ((uintptr_t)fresh & (SLAB_PAGE_SIZE - 1u))) {
            mm_free_pages(phys, 1);
            fresh = NULL;
            phys = 0;
        } else {
            fresh->tree.left = fresh->tree.right = NULL;
            fresh->tree.height = 1;
            fresh->partial_prev = fresh->partial_next = NULL;
            fresh->phys = phys;
            for (unsigned i = 0; i < SLAB_BITMAP_WORDS; ++i) fresh->used[i] = 0;
            fresh->class_index = (uint16_t)c;
            fresh->slot_count = (uint16_t)((SLAB_PAGE_SIZE - SLAB_DATA_OFFSET) / slab_sizes[c]);
            fresh->active = 0;
        }
    }

    /* A competing allocator may have published a usable slab while PMM ran. */
    kernel_spin_lock_irqsave(&slab_lock, &flags);
    page = partial[c];
    if (!page && fresh) {
        page = fresh;
        fresh = NULL;
        slab_pages = heap_tree_insert(slab_pages, &page->tree);
        partial_add(page);
    }
    if (page) result = reserve_slot(page);
    kernel_spin_unlock_irqrestore(&slab_lock, flags);
    if (fresh) mm_free_pages(phys, 1);
    if (!result) return NULL;
clear:
    __builtin_memset(result, 0, slab_sizes[c]);
    return result;
}

/** @brief Consume a pointer in an owned slab, rejecting invalid or duplicate slots.
 * @param memory Arbitrary candidate address. @return True for every address in an owned slab.
 * Empty pages are unlinked under slab_lock and returned to PMM after unlocking.
 */
bool slab_free(void *memory)
{
    uintptr_t address = (uintptr_t)memory;
    uintptr_t base = address & ~(uintptr_t)(SLAB_PAGE_SIZE - 1u);
    uint64_t flags, phys = 0;
    kernel_spin_lock_irqsave(&slab_lock, &flags);
    struct slab_page *page = (struct slab_page *)heap_tree_find(slab_pages, base);
    if (!page) {
        kernel_spin_unlock_irqrestore(&slab_lock, flags);
        return false;
    }
    size_t offset = (size_t)(address - base);
    size_t unit = slab_sizes[page->class_index];
    if (offset >= SLAB_DATA_OFFSET) {
        offset -= SLAB_DATA_OFFSET;
        if (!(offset % unit) && offset / unit < page->slot_count) {
            unsigned index = (unsigned)(offset / unit);
            uint64_t bit = UINT64_C(1) << (index % 64u);
            if (page->used[index / 64u] & bit) {
                bool was_full = page->active == page->slot_count;
                page->used[index / 64u] &= ~bit;
                --page->active;
                if (!page->active) {
                    if (!was_full) partial_remove(page);
                    slab_pages = heap_tree_remove(slab_pages, base);
                    phys = page->phys;
                } else if (was_full) partial_add(page);
            }
        }
    }
    kernel_spin_unlock_irqrestore(&slab_lock, flags);
    if (phys) mm_free_pages(phys, 1);
    return true;
}
