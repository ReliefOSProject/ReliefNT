/*
 * ReliefOS read-only page cache implementation.
 * Entries retain physical pages until invalidated or replaced while idle.
 */
#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>

#define PAGE_CACHE_MAX 4096u
#define PAGE_CACHE_OVERFLOW_ENTRIES 64u
#define PAGE_CACHE_PAGE_SIZE 4096ULL

struct page_cache_entry {
    uint8_t used;
    uint8_t reserved[3];
    uint32_t refs;
    uint64_t phys;
    uint64_t offset;
    struct storage_node node;
};

/* The fixed array bounds idle retention, not the resident file working set.
 * Overflow entries exist only while pinned. Group metadata so the page-backed
 * heap does not consume a whole page for every additional file page. */
struct page_cache_overflow {
    struct page_cache_overflow *next;
    uint32_t used;
    struct page_cache_entry entries[PAGE_CACHE_OVERFLOW_ENTRIES];
};

static struct page_cache_entry entries[PAGE_CACHE_MAX];
static struct page_cache_overflow *overflow;
static struct kernel_spinlock cache_lock;
static uint32_t cache_hits;

static int node_equal(const struct storage_node *a, const struct storage_node *b)
{
    return a && b && a->type == b->type && a->flags == b->flags &&
           a->first_cluster == b->first_cluster && a->volume_id == b->volume_id &&
           a->size == b->size;
}

static struct page_cache_entry *find_entry(const struct storage_node *node,
                                           uint64_t offset)
{
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        if (entries[i].used && entries[i].offset == offset &&
            node_equal(&entries[i].node, node)) {
            return &entries[i];
        }
    }
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next) {
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
            struct page_cache_entry *entry = &chunk->entries[i];
            if (entry->used && entry->offset == offset && node_equal(&entry->node, node)) {
                return entry;
            }
        }
    }
    return NULL;
}

static struct page_cache_entry *find_phys(uint64_t phys)
{
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        if (entries[i].used && entries[i].phys == phys) return &entries[i];
    }
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next) {
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
            if (chunk->entries[i].used && chunk->entries[i].phys == phys)
                return &chunk->entries[i];
        }
    }
    return NULL;
}

void page_cache_init(void)
{
    kernel_spin_init(&cache_lock);
    overflow = NULL;
    cache_hits = 0;
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        entries[i] = (struct page_cache_entry){0};
    }
    console_info("[memory] file page cache idle_slots=%u page_bytes=%llu idle_capacity_kib=%llu pinned=dynamic\n",
                 PAGE_CACHE_MAX, (unsigned long long)PAGE_CACHE_PAGE_SIZE,
                 (unsigned long long)(PAGE_CACHE_MAX * PAGE_CACHE_PAGE_SIZE / 1024u));
}

int page_cache_lookup(const struct storage_node *node, uint64_t offset,
                      uint64_t *phys)
{
    uint64_t flags;
    struct page_cache_entry *entry;
    if (!node || !phys || (offset & (PAGE_CACHE_PAGE_SIZE - 1ULL))) {
        return -1;
    }
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    entry = find_entry(node, offset);
    if (entry) {
        ++entry->refs;
        ++cache_hits;
        *phys = entry->phys;
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return entry ? 0 : -1;
}

int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{
    uint64_t flags;
    struct page_cache_entry *entry = NULL;
    struct page_cache_overflow *spare = NULL;
    uint64_t page;
    uint64_t evicted = 0;
    bool allocation_attempted = false;
    if (page_cache_lookup(node, offset, phys) == 0) {
        return 0;
    }
    if (!node || !loader || !phys || (offset & (PAGE_CACHE_PAGE_SIZE - 1ULL))) {
        return -1;
    }
    page = mm_alloc_page();
    if (!page || loader(page, context) < 0) {
        if (page) {
            mm_free_page(page);
        }
        return -1;
    }
    for (;;) {
        struct page_cache_overflow *owner = NULL;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        /* Loading and metadata allocation run unlocked. Another caller may
         * have published this identity or released a slot in the meantime. */
        entry = find_entry(node, offset);
        if (entry) {
            ++entry->refs;
            *phys = entry->phys;
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            break;
        }
        for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
            if (!entries[i].used || entries[i].refs == 0) {
                entry = &entries[i];
                break;
            }
        }
        for (struct page_cache_overflow *chunk = overflow; !entry && chunk; chunk = chunk->next) {
            for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
                if (!chunk->entries[i].used) {
                    entry = &chunk->entries[i];
                    owner = chunk;
                    break;
                }
            }
        }
        if (!entry && spare) {
            spare->next = overflow;
            overflow = spare;
            owner = spare;
            entry = &spare->entries[0];
            spare = NULL;
        }
        if (!entry) {
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            if (allocation_attempted) break;
            spare = kernel_malloc(sizeof(*spare));
            if (spare) *spare = (struct page_cache_overflow){0};
            allocation_attempted = true;
            continue;
        }
        evicted = entry->used ? entry->phys : 0;
        *entry = (struct page_cache_entry){.used = 1, .refs = 1, .phys = page,
                                         .offset = offset, .node = *node};
        if (owner) ++owner->used;
        *phys = page;
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        break;
    }
    kernel_free(spare);
    if (evicted) mm_free_page(evicted);
    if (!entry) {
        mm_free_page(page);
        return PAGE_CACHE_FULL;
    }
    if (*phys != page) {
        mm_free_page(page);
    }
    return 0;
}

void page_cache_release(uint64_t phys)
{
    uint64_t flags;
    struct page_cache_overflow *empty = NULL;
    uint64_t released = 0;
    if (!phys) {
        return;
    }
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        if (entries[i].used && entries[i].phys == phys) {
            if (entries[i].refs) {
                --entries[i].refs;
            }
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            return;
        }
    }
    for (struct page_cache_overflow **link = &overflow; *link; link = &(*link)->next) {
        struct page_cache_overflow *chunk = *link;
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
            struct page_cache_entry *entry = &chunk->entries[i];
            if (entry->used && entry->phys == phys) {
                if (--entry->refs == 0) {
                    released = entry->phys;
                    *entry = (struct page_cache_entry){0};
                    if (--chunk->used == 0) {
                        *link = chunk->next;
                        empty = chunk;
                    }
                }
                kernel_spin_unlock_irqrestore(&cache_lock, flags);
                if (released) mm_free_page(released);
                kernel_free(empty);
                return;
            }
        }
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

int page_cache_retain(uint64_t phys)
{
    uint64_t flags;
    if (!phys) {
        return -1;
    }
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry = find_phys(phys);
    if (entry) ++entry->refs;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return entry ? 0 : -1;
}

int page_cache_owns(uint64_t phys)
{
    uint64_t flags;
    int found;
    if (!phys) {
        return 0;
    }
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    found = find_phys(phys) != NULL;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return found;
}

/**
 * @brief Drop unreferenced resident pages matching one native backing identity.
 * @param node Borrowed stable type/volume/inode identity, nullable for a no-op.
 * @par Context
 * Acquires the IRQ-safe cache lock and releases only unpinned resident pages. Pinned and overflow entries survive until their normal final release; no file/provider callback or user access.
 */
void page_cache_invalidate_node(const struct storage_node *node)
{
    uint64_t flags;
    if (!node) {
        return;
    }
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    /* Overflow contains only pinned pages; its last release removes them. */
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        const struct storage_node *cached = &entries[i].node;
        if (entries[i].used && cached->type == node->type &&
            cached->volume_id == node->volume_id && cached->first_cluster == node->first_cluster &&
            !entries[i].refs) {
            mm_free_page(entries[i].phys);
            entries[i] = (struct page_cache_entry){0};
        }
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

void page_cache_stats(uint32_t *count, uint32_t *hits)
{
    uint64_t flags;
    uint32_t used = 0;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        used += entries[i].used != 0;
    }
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next)
        used += chunk->used;
    if (count) {
        *count = used;
    }
    if (hits) {
        *hits = cache_hits;
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(page_cache_invalidate_node);
