/*
 * ReliefOS kernel heap implementation.
 * Uses the physical page allocator through the kernel's shared direct map.
 */
#include <reliefnt/heap.h>
#include <reliefnt/console.h>
#include <reliefnt/mm.h>

#define KERNEL_HEAP_MAGIC 0x4c484541u
#define KERNEL_PAGE_SIZE 4096u

struct kernel_heap_header {
    uint32_t magic;
    uint32_t pages;
    uint64_t size;
};

/** @brief Report initialization of the page-backed kernel heap.
 * @return None; allocation begins directly from the physical page allocator.
 */
void kernel_heap_init(void)
{
    console_info("[memory] kernel heap allocator=page-backed page_bytes=%u header_bytes=%u\n",
                 KERNEL_PAGE_SIZE, (unsigned)sizeof(struct kernel_heap_header));
}

/** @brief Allocate kernel memory backed by zeroed physical pages.
 * @param size Requested payload bytes.
 * @return Direct-map pointer to the payload, or NULL on invalid size/ENOMEM.
 */
void *kernel_malloc(size_t size)
{
    uint64_t total;
    uint32_t pages;
    uint64_t phys;
    struct kernel_heap_header *header;

    if (!size || size > UINT64_MAX - sizeof(*header)) {
        return NULL;
    }
    total = (uint64_t)size + sizeof(*header);
    pages = (uint32_t)((total + KERNEL_PAGE_SIZE - 1u) / KERNEL_PAGE_SIZE);
    if (!pages || (uint64_t)pages * KERNEL_PAGE_SIZE < total) {
        return NULL;
    }
    phys = mm_alloc_pages(pages);
    if (!phys) {
        return NULL;
    }
    header = (struct kernel_heap_header *)mm_phys_to_virt(phys);
    if (!header) {
        mm_free_pages(phys, pages);
        return NULL;
    }
    header->magic = KERNEL_HEAP_MAGIC;
    header->pages = pages;
    header->size = size;
    return (void *)(header + 1);
}

/** @brief Return a kernel heap allocation's pages to the physical allocator.
 * @param memory Pointer previously returned by kernel_malloc, or NULL.
 * @return None; non-heap and malformed pointers are ignored.
 */
void kernel_free(void *memory)
{
    struct kernel_heap_header *header;
    if (!memory) {
        return;
    }
    header = ((struct kernel_heap_header *)memory) - 1;
    if (header->magic != KERNEL_HEAP_MAGIC || !header->pages) {
        return;
    }
    header->magic = 0;
    uint64_t phys = mm_virt_to_phys(header);
    if (phys) mm_free_pages(phys, header->pages);
}
