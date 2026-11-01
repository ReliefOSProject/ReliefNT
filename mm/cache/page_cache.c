/* File-page cache. Only proven reloadable, unmodified pages may be discarded. */
#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <linux/errno.h>

#define PAGE_CACHE_MAX 4096u
#define PAGE_CACHE_OVERFLOW_ENTRIES 64u
#define PAGE_CACHE_PAGE_SIZE 4096ULL

struct page_cache_overflow;
struct page_cache_entry;
struct cache_tree_link { struct page_cache_entry *left, *right; int height; };
struct page_cache_entry {
    uint8_t used;
    uint8_t clean;
    uint8_t retired;
    uint8_t size_authoritative;
    uint8_t dirty;
    uint32_t refs;
    uint32_t valid_bytes;
    uint64_t phys;
    uint64_t offset;
    uint64_t serial;
    struct storage_node node;
    struct page_cache_overflow *owner;
    struct page_cache_entry *free_next;
    struct cache_tree_link key_tree, phys_tree;
};

/* Fixed slots bound ordinary idle retention. Overflow contains pinned pages
 * and also idle pages that cannot safely be discarded. Empty overflow chunks
 * may be kept for reuse by budgeted reclaim, which only returns data frames. */
struct page_cache_overflow {
    struct page_cache_overflow *next, *prev, *available_next, *available_prev;
    struct page_cache_entry *free_head;
    uint32_t used;
    struct page_cache_entry entries[PAGE_CACHE_OVERFLOW_ENTRIES];
};

static struct page_cache_entry entries[PAGE_CACHE_MAX];
static struct page_cache_overflow *overflow;
static struct page_cache_overflow *available_overflow;
static struct page_cache_entry *fixed_free, *key_root, *phys_root;
static struct page_cache_report cache_report;
#ifdef RELIEFNT_CACHE_PROBE
/** @brief Test-only observer for cache work; absent from production builds.
 * @param kind Index, availability or candidate category chosen by the caller.
 * No cache ownership transfers; fixture instrumentation must not reenter cache.
 */
extern void page_cache_test_probe(unsigned kind);
#define CACHE_PROBE(kind) page_cache_test_probe(kind)
#else
#define CACHE_PROBE(kind) ((void)0)
#endif
static struct kernel_spinlock cache_lock;
static uint32_t cache_hits;
static uint64_t cache_serial;
/* The cursor is protected by cache_lock. Every chunk unlink advances it before
 * the chunk is freed, so no unowned overflow pointer survives retirement. */
static struct page_cache_overflow *pressure_chunk;
static uint32_t pressure_fixed_index, pressure_chunk_index;
static bool pressure_in_overflow, pressure_cycle_pending;
static uint64_t pressure_epoch;

/** @brief Advance a pressure cursor before an overflow owner is unlinked.
 * @param chunk Chunk being removed under cache_lock. No allocation or callback.
 */
static void pressure_forget_chunk(struct page_cache_overflow *chunk)
{
    if (!pressure_in_overflow || pressure_chunk != chunk) return;
    pressure_chunk = chunk->next;
    pressure_chunk_index = 0;
    if (!pressure_chunk) {
        pressure_in_overflow = false;
        pressure_fixed_index = 0;
        pressure_cycle_pending = true;
    }
}

/** @brief Recognize a supported clustered-file identity flag combination.
 * @param flags File flags. @return FAT/exFAT kind, or zero for unknown flags.
 */
static uint32_t clustered_kind(uint32_t flags)
{
    if (flags == STORAGE_NODE_FLAG_FAT32) return STORAGE_NODE_FLAG_FAT32;
    if (flags == STORAGE_NODE_FLAG_EXFAT ||
        flags == (STORAGE_NODE_FLAG_EXFAT | STORAGE_NODE_FLAG_EXFAT_NOFAT)) return STORAGE_NODE_FLAG_EXFAT;
    return 0;
}

/** @brief Compare stable inode incarnations, ignoring mutable clustered fields.
 * @param a First key. @param b Second key. @return Nonzero for one incarnation.
 */
static int node_same_inode(const struct storage_node *a, const struct storage_node *b)
{
    if (!a || !b || a->type != b->type || a->volume_id != b->volume_id ||
        a->mount_generation != b->mount_generation) return 0;
    uint32_t clustered = clustered_kind(a->flags);
    if (clustered && clustered == clustered_kind(b->flags) && a->inode_generation && b->inode_generation)
        return a->inode_generation == b->inode_generation;
    return a->flags == b->flags && a->first_cluster == b->first_cluster &&
           a->inode_generation == b->inode_generation && a->entry_id == b->entry_id;
}

/* Both indexes are intrusive AVL trees, so a lookup/insert/removal holds the
 * IRQ-safe lock for logarithmic rather than capacity-proportional work. The
 * identity ordering deliberately normalizes exactly the clustered identities
 * which node_same_inode treats as equal; mutable file sizes are not keys. */
/** @brief Compare unsigned scalar keys without subtraction overflow.
 * @param a First key value. @param b Second key value.
 * @return Negative, zero or positive according to key order. No state changes.
 */
static int compare_u64(uint64_t a, uint64_t b) { return a < b ? -1 : a > b; }
/** @brief Order stable inode identities and aligned file-page offsets.
 * @param a Nonnull borrowed first inode key. @param ao Its aligned byte offset.
 * @param b Nonnull borrowed second inode key. @param bo Its aligned byte offset.
 * @return Negative, zero or positive; equality matches node_same_inode plus
 * offset. Callers stabilize keys; cache_lock protects resident entry keys.
 * Mutable file size is ignored and no ownership or data changes occur.
 */
static int compare_key(const struct storage_node *a, uint64_t ao,
                       const struct storage_node *b, uint64_t bo)
{
    int c;
    if ((c=compare_u64(a->type,b->type)) ||
        (c=compare_u64(a->volume_id,b->volume_id)) ||
        (c=compare_u64(a->mount_generation,b->mount_generation))) return c;
    uint32_t ak=clustered_kind(a->flags), bk=clustered_kind(b->flags);
    bool ac=ak && a->inode_generation, bc=bk && b->inode_generation;
    if ((c=compare_u64(ac,bc))) return c;
    if (ac) {
        if ((c=compare_u64(ak,bk)) || (c=compare_u64(a->inode_generation,b->inode_generation))) return c;
    } else if ((c=compare_u64(a->flags,b->flags)) ||
               (c=compare_u64(a->first_cluster,b->first_cluster)) ||
               (c=compare_u64(a->inode_generation,b->inode_generation)) ||
               (c=compare_u64(a->entry_id,b->entry_id))) return c;
    return compare_u64(ao,bo);
}

/** @brief Select an entry's intrusive index links under cache_lock.
 * @param entry Nonnull cache-owned entry. @param physical Select physical
 * index if true, identity index otherwise. @return Borrowed links valid only
 * while the entry and cache_lock remain held; no ownership transfer.
 */
static struct cache_tree_link *tree_link(struct page_cache_entry *entry, bool physical)
{ CACHE_PROBE(physical ? 2 : 1); return physical ? &entry->phys_tree : &entry->key_tree; }
/** @brief Read one subtree's cached AVL height under cache_lock.
 * @param entry Borrowed subtree root, nullable. @param physical Index selector.
 * @return Zero for NULL or the root's stored height; no ownership change.
 */
static int tree_height(struct page_cache_entry *entry, bool physical)
{ return entry ? tree_link(entry,physical)->height : 0; }
/** @brief Recompute an AVL node's height from its children under cache_lock.
 * @param entry Nonnull borrowed node. @param physical Index selector.
 * Updates only this index's height; does not allocate or move entry ownership.
 */
static void tree_height_update(struct page_cache_entry *entry, bool physical)
{
    struct cache_tree_link *link=tree_link(entry,physical);
    int left=tree_height(link->left,physical), right=tree_height(link->right,physical);
    link->height=1+(left>right ? left : right);
}
/** @brief Rotate an AVL subtree left while cache_lock protects its links.
 * @param root Nonnull borrowed root with a right child. @param physical Index selector.
 * @return New subtree root; links and heights change without frame/ref transfer.
 */
static struct page_cache_entry *tree_rotate_left(struct page_cache_entry *root, bool physical)
{
    struct cache_tree_link *link=tree_link(root,physical);
    struct page_cache_entry *next=link->right;
    struct cache_tree_link *other=tree_link(next,physical);
    link->right=other->left; other->left=root;
    tree_height_update(root,physical); tree_height_update(next,physical); return next;
}
/** @brief Rotate an AVL subtree right while cache_lock protects its links.
 * @param root Nonnull borrowed root with a left child. @param physical Index selector.
 * @return New subtree root; links and heights change without frame/ref transfer.
 */
static struct page_cache_entry *tree_rotate_right(struct page_cache_entry *root, bool physical)
{
    struct cache_tree_link *link=tree_link(root,physical);
    struct page_cache_entry *next=link->left;
    struct cache_tree_link *other=tree_link(next,physical);
    link->left=other->right; other->right=root;
    tree_height_update(root,physical); tree_height_update(next,physical); return next;
}
/** @brief Restore an AVL subtree's balance after a link change under cache_lock.
 * @param root Borrowed root, nullable. @param physical Index selector.
 * @return Balanced root or NULL; updates only index links/heights without
 * allocation, frame release or reference transfer.
 */
static struct page_cache_entry *tree_balance(struct page_cache_entry *root, bool physical)
{
    if (!root) return NULL;
    struct cache_tree_link *link=tree_link(root,physical);
    tree_height_update(root,physical);
    int balance=tree_height(link->left,physical)-tree_height(link->right,physical);
    if (balance>1) {
        struct cache_tree_link *child=tree_link(link->left,physical);
        if (tree_height(child->right,physical)>tree_height(child->left,physical))
            link->left=tree_rotate_left(link->left,physical);
        return tree_rotate_right(root,physical);
    }
    if (balance < -1) {
        struct cache_tree_link *child=tree_link(link->right,physical);
        if (tree_height(child->left,physical)>tree_height(child->right,physical))
            link->right=tree_rotate_right(link->right,physical);
        return tree_rotate_left(root,physical);
    }
    return root;
}
/** @brief Compare two cache entries in one selected index under cache_lock.
 * @param a Nonnull borrowed first entry. @param b Nonnull borrowed second entry.
 * @param physical Select frame ordering if true, file-page key otherwise.
 * @return Negative, zero or positive; neither entry is mutated or retained.
 */
static int tree_compare(const struct page_cache_entry *a, const struct page_cache_entry *b,
                        bool physical)
{
    return physical ? compare_u64(a->phys,b->phys) : compare_key(&a->node,a->offset,&b->node,b->offset);
}
/** @brief Insert one unlinked entry into an AVL index under cache_lock.
 * @param root Borrowed current subtree root, nullable.
 * @param entry Nonnull cache-owned entry with a unique key and unlinked
 * selected-index links. @param physical Index selector.
 * @return New subtree root. Updates links/heights only; no allocation,
 * physical frame ownership, cache reference or other index is changed.
 */
static struct page_cache_entry *tree_insert(struct page_cache_entry *root,
                                             struct page_cache_entry *entry, bool physical)
{
    if (!root) { tree_link(entry,physical)->height=1; return entry; }
    struct cache_tree_link *link=tree_link(root,physical);
    if (tree_compare(entry,root,physical)<0) link->left=tree_insert(link->left,entry,physical);
    else link->right=tree_insert(link->right,entry,physical);
    return tree_balance(root,physical);
}
/** @brief Remove the smallest indexed node from a nonempty subtree.
 * @param root Nonnull borrowed subtree root under cache_lock.
 * @param minimum Required output receiving the detached borrowed node.
 * @param physical Index selector. @return New subtree root, possibly NULL;
 * selected links/heights change but cache, frame and reference ownership do not.
 */
static struct page_cache_entry *tree_take_min(struct page_cache_entry *root,
                                               struct page_cache_entry **minimum, bool physical)
{
    struct cache_tree_link *link=tree_link(root,physical);
    if (!link->left) { *minimum=root; return link->right; }
    link->left=tree_take_min(link->left,minimum,physical); return tree_balance(root,physical);
}
/** @brief Remove the entry with a unique matching key from an AVL subtree.
 * @param root Borrowed root under cache_lock, nullable.
 * @param entry Nonnull borrowed entry identifying the key to remove.
 * @param physical Index selector. @return Rebalanced root, possibly NULL;
 * selected links/heights change without allocation or frame/ref transfer.
 */
static struct page_cache_entry *tree_remove(struct page_cache_entry *root,
                                             struct page_cache_entry *entry, bool physical)
{
    if (!root) return NULL;
    struct cache_tree_link *link=tree_link(root,physical);
    int c=tree_compare(entry,root,physical);
    if (c<0) link->left=tree_remove(link->left,entry,physical);
    else if (c>0) link->right=tree_remove(link->right,entry,physical);
    else {
        struct page_cache_entry *left=link->left, *right=link->right;
        if (!right) return left;
        struct page_cache_entry *replacement;
        right=tree_take_min(right,&replacement,physical);
        struct cache_tree_link *r=tree_link(replacement,physical);
        r->left=left; r->right=right; return tree_balance(replacement,physical);
    }
    return tree_balance(root,physical);
}

/** @brief Find a resident identity while cache_lock is held.
 * @param node Nonnull borrowed file key. @param offset Aligned page offset.
 * @return Borrowed entry or NULL; its address is valid only while cache_lock
 * remains held. No entry reference or physical frame ownership is acquired.
 * Every size snapshot of an inode shares one resident page and write identity.
 */
static struct page_cache_entry *find_entry(const struct storage_node *node,
                                           uint64_t offset)
{
    struct page_cache_entry *entry=key_root;
    while (entry) {
        CACHE_PROBE(1);
        int c=compare_key(node,offset,&entry->node,entry->offset);
        if (!c) return entry;
        entry=c<0 ? entry->key_tree.left : entry->key_tree.right;
    }
    return NULL;
}

/** @brief Bound an inode size to the valid prefix of one file page.
 * @param size Logical file size. @param offset Aligned file page offset.
 * @return Number of bytes within EOF, at most one page.
 */
static uint32_t page_valid_bytes(uint64_t size,uint64_t offset)
{
    if (size <= offset) return 0;
    return size-offset < PAGE_CACHE_PAGE_SIZE ? (uint32_t)(size-offset) : PAGE_CACHE_PAGE_SIZE;
}

/** @brief Determine how much of a resident page this snapshot may request.
 * @param entry Resident entry under cache_lock. @param node Request snapshot.
 * @return Valid byte prefix, bounded by a known authoritative resize.
 */
static uint32_t requested_bytes(const struct page_cache_entry *entry,const struct storage_node *node)
{
    uint64_t size=node->size;
    if (entry->size_authoritative && size>entry->node.size) size=entry->node.size;
    return page_valid_bytes(size,entry->offset);
}

/** @brief Test whether an inode has any lookup-visible cached page.
 * @param node Borrowed stable inode key. @return Nonzero while resident.
 * IRQ-safe, without retaining a frame; storage execution serializes metadata
 * eviction against file-page publication and terminal inode retirement.
 */
int page_cache_has_inode(const struct storage_node *node)
{
    if (!node) return 0;
    uint64_t flags; int found = 0;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    for (uint32_t i = 0; i < PAGE_CACHE_MAX && !found; ++i)
        found = entries[i].used && !entries[i].retired && node_same_inode(&entries[i].node, node);
    for (struct page_cache_overflow *chunk = overflow; chunk && !found; chunk = chunk->next)
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES && !found; ++i)
            found = chunk->entries[i].used && !chunk->entries[i].retired &&
                    node_same_inode(&chunk->entries[i].node, node);
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return found;
}

/** @brief Find a resident physical frame while cache_lock is held.
 * @param phys Aligned frame. @return Borrowed entry or NULL, valid only while
 * cache_lock remains held. No frame or entry reference is acquired.
 */
static struct page_cache_entry *find_phys(uint64_t phys)
{
    struct page_cache_entry *entry=phys_root;
    while (entry) {
        CACHE_PROBE(2);
        int c=compare_u64(phys,entry->phys);
        if (!c) return entry;
        entry=c<0 ? entry->phys_tree.left : entry->phys_tree.right;
    }
    return NULL;
}

/** @brief Decide whether a successful full-page load has an authoritative disk source.
 * @param node File key. @return True for a known disk filesystem, excluding pseudo files.
 * Callers must supply a loader that exactly reconstructs the file page. Unknown,
 * flagless, tmpfs, memfd and mixed device identities remain non-discardable.
 */
static bool reloadable_node(const struct storage_node *node)
{
    uint32_t flags = node->flags;
    return node->type == RELIEFOS_FS_TYPE_FILE &&
           (flags == STORAGE_NODE_FLAG_FAT32 || flags == STORAGE_NODE_FLAG_EXT2 ||
            flags == STORAGE_NODE_FLAG_EXFAT ||
            flags == (STORAGE_NODE_FLAG_EXFAT | STORAGE_NODE_FLAG_EXFAT_NOFAT));
}

/** @brief Test the complete eviction predicate under cache_lock.
 * @param entry Cache slot. @return True only for an idle, proven clean frame.
 */
static bool reclaimable(const struct page_cache_entry *entry)
{
    return entry->used && !entry->retired && entry->clean && entry->refs == 0;
}

/* Free fixed slots and overflow owners with free slots are linked directly.
 * All list changes and indexed entry/counter changes require cache_lock. */
/** @brief Link a previously unlisted overflow chunk with available slots.
 * @param chunk Nonnull cache-owned chunk with a nonempty free list.
 * Caller holds cache_lock; list links change without allocation or ownership transfer.
 */
static void available_add(struct page_cache_overflow *chunk)
{
    chunk->available_prev=NULL; chunk->available_next=available_overflow;
    if (available_overflow) available_overflow->available_prev=chunk;
    available_overflow=chunk;
}
/** @brief Unlink a chunk from the available-overflow list under cache_lock.
 * @param chunk Nonnull cache-owned, currently listed chunk. Clears its list
 * links but does not free the chunk, detach entries or transfer ownership.
 */
static void available_remove(struct page_cache_overflow *chunk)
{
    if (chunk->available_prev) chunk->available_prev->available_next=chunk->available_next;
    else available_overflow=chunk->available_next;
    if (chunk->available_next) chunk->available_next->available_prev=chunk->available_prev;
    chunk->available_next=chunk->available_prev=NULL;
}
/** @brief Publish a newly allocated empty overflow chunk under cache_lock.
 * @param chunk Nonnull zero-initialized, exclusively owned spare metadata.
 * Initializes its entry free list and transfers it to cache ownership; no
 * frames or references are acquired and no allocation occurs under the lock.
 */
static void overflow_link(struct page_cache_overflow *chunk)
{
    chunk->prev=NULL; chunk->next=overflow;
    if (overflow) overflow->prev=chunk;
    overflow=chunk; chunk->free_head=NULL;
    for (uint32_t i=0;i<PAGE_CACHE_OVERFLOW_ENTRIES;++i) {
        struct page_cache_entry *entry=&chunk->entries[i];
        *entry=(struct page_cache_entry){.owner=chunk,.free_next=chunk->free_head};
        chunk->free_head=entry;
    }
    available_add(chunk);
}
/** @brief Remove an empty overflow owner from both cache lists.
 * @param chunk Nonnull cache-owned chunk with zero used entries and a free list.
 * Caller holds cache_lock. Advances any scanner cursor before unlinking; the
 * caller owns the detached metadata and frees it only after releasing the lock.
 */
static void overflow_unlink(struct page_cache_overflow *chunk)
{
    pressure_forget_chunk(chunk);
    available_remove(chunk);
    if (chunk->prev) chunk->prev->next=chunk->next; else overflow=chunk->next;
    if (chunk->next) chunk->next->prev=chunk->prev;
}
/** @brief Reserve an existing free slot while cache_lock remains held.
 * @param owner Required output receiving the overflow owner or NULL for fixed
 * capacity; also set to NULL on miss. @return Borrowed reserved entry or NULL.
 * Updates free lists and overflow occupancy; caller publishes the entry before
 * unlocking. No frame, heap allocation or external reference is acquired.
 */
static struct page_cache_entry *take_free(struct page_cache_overflow **owner)
{
    CACHE_PROBE(3);
    if (fixed_free) {
        struct page_cache_entry *entry=fixed_free;
        fixed_free=entry->free_next; *owner=NULL; return entry;
    }
    CACHE_PROBE(3);
    struct page_cache_overflow *chunk=available_overflow;
    if (!chunk) { *owner=NULL; return NULL; }
    CACHE_PROBE(3);
    struct page_cache_entry *entry=chunk->free_head;
    chunk->free_head=entry->free_next;
    CACHE_PROBE(3);
    if (!chunk->free_head) available_remove(chunk);
    ++chunk->used; *owner=chunk; return entry;
}
/** @brief Remove an indexed entry and return its frame without releasing it.
 * @param entry Live entry under cache_lock. @param idle_clean The caller's
 * already checked reclaimable state; retired entries always pass false.
 * @return Former cache frame; caller releases it after dropping cache_lock.
 * Removes both live indexes as applicable, updates counters and returns the
 * slot to its fixed/overflow free list; does not free a frame or chunk itself.
 */
static uint64_t detach_entry(struct page_cache_entry *entry, bool idle_clean)
{
    uint64_t phys=entry->phys;
    struct page_cache_overflow *owner=entry->owner;
    --cache_report.resident;
    if (idle_clean) --cache_report.reclaimable;
    if (entry->dirty) --cache_report.dirty;
    if (!entry->retired) key_root=tree_remove(key_root,entry,false);
    phys_root=tree_remove(phys_root,entry,true);
    if (owner) {
        if (!owner->free_head) available_add(owner);
        *entry=(struct page_cache_entry){.owner=owner,.free_next=owner->free_head};
        owner->free_head=entry; --owner->used;
    } else {
        *entry=(struct page_cache_entry){.free_next=fixed_free}; fixed_free=entry;
    }
    return phys;
}
/** @brief Hide one cache identity without invalidating existing frame pins.
 * @param entry Nonnull cache-owned live or already-retired entry under cache_lock.
 * Removes its identity index once and adjusts reclaimability; physical index,
 * dirty state and frame ownership persist until the last reference is released.
 */
static void retire_entry(struct page_cache_entry *entry)
{
    if (entry->retired) return;
    if (reclaimable(entry)) --cache_report.reclaimable;
    key_root=tree_remove(key_root,entry,false); entry->retired=1;
}

/** @brief Retain an entry without ever wrapping a live count to zero.
 * @param entry Nonnull live entry under cache_lock. A reclaimable entry leaves
 * that counter before its reference is acquired; saturation pins it forever.
 */
static void retain_entry(struct page_cache_entry *entry)
{
    if (reclaimable(entry)) --cache_report.reclaimable;
    if (entry->refs != UINT32_MAX) ++entry->refs;
}

/** @brief Find unused capacity first, optionally then a replaceable cache slot.
 * @param owner Required output, set to the overflow owner or NULL for a fixed slot.
 * @param replace Whether this caller may inspect clean replacement candidates.
 * @return Borrowed free or replaceable slot, or NULL; valid only while
 * cache_lock remains held. Merely observing a slot transfers no frame or entry
 * ownership and does not remove it from its free list or indexes.
 */
static struct page_cache_entry *find_slot(struct page_cache_overflow **owner, bool replace)
{
    *owner = NULL;
    CACHE_PROBE(3);
    if (fixed_free) return fixed_free;
    CACHE_PROBE(3);
    if (available_overflow) { CACHE_PROBE(3); *owner=available_overflow; return available_overflow->free_head; }
    if (!replace) return NULL;
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        CACHE_PROBE(4);
        if (reclaimable(&entries[i])) return &entries[i];
    }
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next)
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i)
            { CACHE_PROBE(4); if (reclaimable(&chunk->entries[i])) { *owner = chunk; return &chunk->entries[i]; } }
    return NULL;
}

/** @brief Initialize the early boot cache before any users or outstanding frames.
 * @return None; initializes the IRQ-safe lock and all fixed slots.
 */
void page_cache_init(void)
{
    kernel_spin_init(&cache_lock);
    overflow = available_overflow = NULL; fixed_free=key_root=phys_root=NULL;
    cache_report=(struct page_cache_report){0}; cache_hits = 0; cache_serial = 0;
    pressure_chunk = NULL; pressure_fixed_index = pressure_chunk_index = 0;
    pressure_in_overflow = pressure_cycle_pending = false;
    pressure_epoch = 0;
    for (uint32_t i = PAGE_CACHE_MAX; i-- > 0;) {
        entries[i] = (struct page_cache_entry){.free_next=fixed_free}; fixed_free=&entries[i];
    }
    console_info("[memory] file page cache idle_slots=%u page_bytes=%llu idle_capacity_kib=%llu pinned=dynamic\n",
                 PAGE_CACHE_MAX, (unsigned long long)PAGE_CACHE_PAGE_SIZE,
                 (unsigned long long)(PAGE_CACHE_MAX * PAGE_CACHE_PAGE_SIZE / 1024u));
}

/** @brief Find and retain a file page.
 * @param node Borrowed key. @param offset Aligned file offset. @param phys Required result.
 * @return Zero on hit, negative on invalid input or miss; no provider operation.
 */
int page_cache_lookup(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{
    uint64_t flags;
    if (!node || !phys || (offset & (PAGE_CACHE_PAGE_SIZE - 1ULL))) return -1;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry = find_entry(node, offset);
    if (entry && requested_bytes(entry,node)>entry->valid_bytes) entry=NULL;
    if (entry) { retain_entry(entry); ++cache_hits; *phys = entry->phys; }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return entry ? 0 : -1;
}

/** @brief Merge a successfully loaded page's newly valid suffix into a pinned hit.
 * @param node Requested snapshot. @param existing Retained resident frame.
 * @param loaded Independently owned fully loaded frame.
 * @return Zero or negative if terminal retirement won the publication race.
 * Caller serializes actual file mutation with faults. Copy is cache-locked so
 * concurrent refreshes never overwrite bytes already made valid and writable.
 */
static int merge_loaded_tail(const struct storage_node *node,uint64_t existing,uint64_t loaded)
{
    uint8_t *destination=mm_phys_to_virt(existing), *source=mm_phys_to_virt(loaded);
    if (!destination || !source) return -1;
    uint64_t flags; kernel_spin_lock_irqsave(&cache_lock,&flags);
    struct page_cache_entry *entry=find_phys(existing);
    if (!entry || entry->retired || !node_same_inode(&entry->node,node)) {
        kernel_spin_unlock_irqrestore(&cache_lock,flags); return -1;
    }
    uint32_t end=requested_bytes(entry,node);
    if (end>entry->valid_bytes) {
        __builtin_memcpy(destination+entry->valid_bytes,source+entry->valid_bytes,end-entry->valid_bytes);
        entry->valid_bytes=end;
    }
    if (!entry->size_authoritative && node->size>entry->node.size) entry->node.size=node->size;
    kernel_spin_unlock_irqrestore(&cache_lock,flags); return 0;
}

/** @brief Refresh a pinned hit without losing old data on allocation or I/O failure.
 * @param node Requested key. @param loader Complete-page provider, nullable.
 * @param context Borrowed loader state. @param existing Retained resident frame.
 * @param fault True when the caller must defer reclaim to the outer fault boundary.
 * @param phys Required result on success. @return Zero, -ENOMEM for a frame
 * or loader allocation failure, or another negative failure.
 */
static int refresh_hit(const struct storage_node *node,page_cache_loader loader,void *context,
                       uint64_t existing,uint64_t *phys,bool fault)
{
    if (!loader) { page_cache_release(existing); return -1; }
    uint64_t loaded=mm_alloc_page();
    if (!loaded && !fault && page_cache_reclaim(1)) loaded=mm_alloc_page();
    if (!loaded) { page_cache_release(existing); return -ENOMEM; }
    int ret=loader(loaded,context);
    if (ret>=0) ret=merge_loaded_tail(node,existing,loaded);
    mm_free_page(loaded);
    if (ret<0) { page_cache_release(existing); return ret == -ENOMEM ? ret : -1; }
    *phys=existing; return 0;
}

/** @brief Load and retain a file page without exposing a partial replacement.
 * @param node Borrowed key. @param offset Aligned file offset.
 * @param loader Provider of the complete reproducible page. @param context Borrowed loader context.
 * @param fault True to defer all clean replacement and reclaim to fault pressure.
 * @param phys Required output on success. @return Zero, PAGE_CACHE_FULL,
 * -ENOMEM for a frame/loader allocation failure, or another negative failure.
 * Slot availability is checked before PMM/provider work, then rechecked at
 * publication. A failed load leaves the old entry intact, except a separately
 * committed clean reclaim after PMM exhaustion may already have evicted it.
 */
static int page_cache_load_internal(const struct storage_node *node, uint64_t offset,
                                    page_cache_loader loader, void *context, uint64_t *phys,
                                    bool fault)
{
    uint64_t flags, page, evicted = 0;
    struct page_cache_overflow *spare = NULL;
    bool attempted = false;
    if (!node || !phys || (offset & (PAGE_CACHE_PAGE_SIZE - 1ULL))) return -1;

    /* Select available capacity before taking a physical frame. No slot pointer
     * escapes the lock: another caller may pin, invalidate or consume it. */
    for (;;) {
        struct page_cache_overflow *owner;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        struct page_cache_entry *entry = find_entry(node, offset);
        if (entry) {
            retain_entry(entry); ++cache_hits;
            bool needs_refresh=requested_bytes(entry,node)>entry->valid_bytes;
            uint64_t existing=entry->phys;
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            kernel_free(spare);
            if (needs_refresh) return refresh_hit(node,loader,context,existing,phys,fault);
            *phys=existing; return 0;
        }
        if (!loader) {
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            kernel_free(spare); return -1;
        }
        bool available = find_slot(&owner, !fault) != NULL || spare != NULL;
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (available) break;
        if (attempted) return fault ? -ENOMEM : PAGE_CACHE_FULL;
        spare = kernel_malloc(sizeof(*spare));
        if (spare) *spare = (struct page_cache_overflow){0};
        attempted = true;
    }

    page = mm_alloc_page();
    if (!page) {
        /* A concurrently published hit needs no frame and must not evict. */
        if (!page_cache_lookup(node, offset, phys)) { kernel_free(spare); return 0; }
        if (!fault && page_cache_reclaim(1)) page = mm_alloc_page();
        if (!page) { kernel_free(spare); return -ENOMEM; }
    }
    int result = loader(page, context);
    if (result < 0) {
        mm_free_page(page); kernel_free(spare); return result == -ENOMEM ? result : -1;
    }

    for (;;) {
        struct page_cache_overflow *owner;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        struct page_cache_entry *entry = find_entry(node, offset);
        if (entry) {
            retain_entry(entry); ++cache_hits;
            bool needs_refresh=requested_bytes(entry,node)>entry->valid_bytes;
            uint64_t existing=entry->phys;
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            int result=needs_refresh ? merge_loaded_tail(node,existing,page) : 0;
            mm_free_page(page); kernel_free(spare);
            if (result<0) { page_cache_release(existing); return -1; }
            *phys=existing; return 0;
        }
        if (cache_serial == UINT64_MAX) {
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            mm_free_page(page); kernel_free(spare); return PAGE_CACHE_FULL;
        }
        entry = find_slot(&owner, !fault);
        if (!entry && spare) {
            overflow_link(spare); spare = NULL;
            entry = find_slot(&owner, false);
        }
        if (entry) {
            if (entry->used) evicted=detach_entry(entry,true);
            entry=take_free(&owner);
            *entry = (struct page_cache_entry){.used = 1,
                .clean = reloadable_node(node) && offset < node->size,
                .refs = 1, .valid_bytes = page_valid_bytes(node->size,offset),
                .phys = page, .offset = offset, .serial = ++cache_serial, .node = *node,
                .owner = owner};
            key_root=tree_insert(key_root,entry,false);
            phys_root=tree_insert(phys_root,entry,true);
            ++cache_report.resident;
            *phys = page;
            kernel_spin_unlock_irqrestore(&cache_lock, flags);
            if (evicted) mm_free_page(evicted);
            kernel_free(spare); return 0;
        }
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (attempted) { mm_free_page(page); return fault ? -ENOMEM : PAGE_CACHE_FULL; }
        spare = kernel_malloc(sizeof(*spare));
        if (spare) *spare = (struct page_cache_overflow){0};
        attempted = true;
    }
}

/** @brief Load a file page with the ordinary replacement/reclaim policy.
 * @param node Borrowed key. @param offset Aligned file offset.
 * @param loader Reproducible page loader. @param context Borrowed loader state.
 * @param phys Required frame result. @return Zero, PAGE_CACHE_FULL, or errno.
 * The caller may hold the normal cache-loading locks; no pointer is retained.
 */
int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{ return page_cache_load_internal(node, offset, loader, context, phys, false); }

/** @brief Load a fault page without unbounded clean-frame replacement/reclaim.
 * @param node Borrowed key. @param offset Aligned file offset.
 * @param loader Reproducible page loader. @param context Borrowed loader state.
 * @param phys Required frame result. @return Zero or negative errno.
 * Fault and loader locks may be held. On resource shortage, the outer fault
 * boundary performs separately budgeted reclaim after those locks are dropped.
 */
int page_cache_load_fault(const struct storage_node *node, uint64_t offset,
                          page_cache_loader loader, void *context, uint64_t *phys)
{ return page_cache_load_internal(node, offset, loader, context, phys, true); }

/** @brief Release one retained cache reference.
 * @param phys Owned aligned cache frame; zero or unknown is a no-op.
 * Idle clean overflow is detached under cache_lock and returned outside it;
 * dirty/unknown and saturated entries remain resident.
 */
void page_cache_release(uint64_t phys)
{
    uint64_t flags, released = 0;
    struct page_cache_overflow *empty = NULL;
    if (!phys) return;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry=find_phys(phys);
    if (entry) {
        bool was_reclaimable=reclaimable(entry);
        if (entry->refs && entry->refs != UINT32_MAX) --entry->refs;
        bool now_reclaimable=reclaimable(entry);
        if (!was_reclaimable && now_reclaimable) ++cache_report.reclaimable;
        struct page_cache_overflow *owner=entry->owner;
        if ((owner && now_reclaimable) || (entry->retired && !entry->refs)) {
            released=detach_entry(entry,now_reclaimable);
            if (owner && !owner->used) { overflow_unlink(owner); empty=owner; }
        }
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    if (released) mm_free_page(released);
    kernel_free(empty);
}

/** @brief Retain a resident frame without inspecting its backing.
 * @param phys Aligned cache frame. @return Zero on hit, negative otherwise.
 */
int page_cache_retain(uint64_t phys)
{
    uint64_t flags;
    if (!phys) return -1;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry = find_phys(phys);
    if (entry) retain_entry(entry);
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return entry ? 0 : -1;
}

/** @brief Test whether a frame is cache-owned without taking a reference.
 * @param phys Candidate frame. @return Nonzero on a current hit.
 */
int page_cache_owns(uint64_t phys)
{
    uint64_t flags;
    if (!phys) return 0;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    int found = find_phys(phys) != NULL;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return found;
}

/** @brief Mark a cache page potentially modified before granting shared write.
 * @param phys Frame retained by the caller. @return None; non-cache frames are ignored.
 * The downgrade is permanent until an explicit future writeback protocol exists.
 */
void page_cache_mark_shared_writable(uint64_t phys)
{
    uint64_t flags;
    if (!phys) return;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry = find_phys(phys);
    if (entry) { if (reclaimable(entry)) --cache_report.reclaimable; entry->clean = 0; }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

/** @brief Record an actual write or admitted shared-cache write fault.
 * @param phys Aligned held frame; zero and non-cache frames are ignored. For a
 * fault the caller marks dirty before writable publication. The cache lock
 * serializes state; no page reference or writeback ownership moves.
 */
void page_cache_note_write(uint64_t phys)
{
    if (!phys) return;
    uint64_t flags; kernel_spin_lock_irqsave(&cache_lock, &flags);
    struct page_cache_entry *entry = find_phys(phys);
    if (entry) {
        if (reclaimable(entry)) --cache_report.reclaimable;
        if (!entry->dirty) { entry->dirty = 1; ++cache_report.dirty; }
        entry->clean = 0;
    }
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

/** @brief Read incrementally maintained frame domains without equating unknown with dirty.
 * @param out Required snapshot, NULL ignored. Cache lock only; no page pin,
 * heap allocation or provider operation occurs.
 */
void page_cache_report(struct page_cache_report *out)
{
    if (!out) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    *out = cache_report;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

/** @brief Count idle proven-clean data frames under the cache lock.
 * @return Snapshot of pages eligible at that instant; no reference is acquired.
 */
uint64_t page_cache_reclaimable_pages(void)
{
    uint64_t flags, count;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    count=cache_report.reclaimable;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return count;
}

/** @brief Detach one eligible frame under cache_lock.
 * @return Detached physical frame to release after unlocking, or zero; overflow
 * metadata remains cache-owned and available for reuse.
 */
static uint64_t detach_clean(void)
{
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
        if (reclaimable(&entries[i])) {
            return detach_entry(&entries[i],true);
        }
    }
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next) {
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
            struct page_cache_entry *entry = &chunk->entries[i];
            if (reclaimable(entry)) {
                return detach_entry(entry,true);
            }
        }
    }
    return 0;
}

/** @brief Return up to budget_pages clean, unreferenced data frames to PMM.
 * @param budget_pages Maximum physical data pages to return. @return Pages actually returned.
 * IRQ-safe and non-sleeping: no provider or heap operation; each frame is
 * detached under cache_lock and its sole PMM reference is released afterward.
 */
uint64_t page_cache_reclaim(uint64_t budget_pages)
{
    uint64_t reclaimed = 0;
    while (reclaimed < budget_pages) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        uint64_t phys = detach_clean();
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (!phys) break;
        mm_free_page(phys); ++reclaimed;
    }
    return reclaimed;
}

/** @brief Inspect a bounded set of cache slots and return actual free frames.
 * @param budget Maximum inspected slots, capped at 32. @param inspected
 * Optional work count. @param cycle_complete Optional full-pass indicator.
 * @param candidates Optional count of resident, non-retired entries visited.
 * @return Pages whose last PMM reference was actually released. The persistent
 * cursor is protected by cache_lock; PMM release occurs outside that lock.
 */
uint32_t page_cache_pressure_scan_count(uint32_t budget, uint32_t *inspected,
                                       bool *cycle_complete, uint32_t *candidates)
{
    uint32_t work = 0, freed = 0, resident = 0;
    bool complete = false;
    if (inspected) *inspected = 0;
    if (cycle_complete) *cycle_complete = false;
    if (candidates) *candidates = 0;
    if (budget > 32) budget = 32;
    while (work < budget && !complete) {
        uint64_t flags, physical = 0;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        if (pressure_cycle_pending) {
            pressure_cycle_pending = false;
            complete = true;
        } else {
            struct page_cache_entry *entry;
            struct page_cache_overflow *owner = NULL;
            if (!pressure_in_overflow) {
                entry = &entries[pressure_fixed_index++];
                if (pressure_fixed_index == PAGE_CACHE_MAX) {
                    pressure_chunk = overflow;
                    pressure_chunk_index = 0;
                    pressure_in_overflow = pressure_chunk != NULL;
                    if (!pressure_in_overflow) {
                        pressure_fixed_index = 0;
                        complete = true;
                    }
                }
            } else {
                owner = pressure_chunk;
                entry = &owner->entries[pressure_chunk_index++];
                if (pressure_chunk_index == PAGE_CACHE_OVERFLOW_ENTRIES) {
                    pressure_chunk = owner->next;
                    pressure_chunk_index = 0;
                    if (!pressure_chunk) {
                        pressure_in_overflow = false;
                        pressure_fixed_index = 0;
                        complete = true;
                    }
                }
            }
            ++work;
            CACHE_PROBE(4);
            if (entry->used && !entry->retired) ++resident;
            if (reclaimable(entry)) {
                physical = detach_entry(entry,true);
            }
        }
        if (complete) ++pressure_epoch;
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (physical && mm_free_page_reclaimed(physical)) ++freed;
    }
    if (inspected) *inspected = work;
    if (cycle_complete) *cycle_complete = complete;
    if (candidates) *candidates = resident;
    return freed;
}

/** @brief Inspect a bounded cache slice without requesting candidate totals.
 * @param budget Maximum slots. @param inspected Optional work output.
 * @param cycle_complete Optional completed-cycle output. @return Freed frames.
 */
uint32_t page_cache_pressure_scan(uint32_t budget, uint32_t *inspected,
                                  bool *cycle_complete)
{ return page_cache_pressure_scan_count(budget, inspected, cycle_complete, NULL); }

/** @brief Snapshot the number of shared cache scanner cycle completions.
 * @return Modulo-64-bit sequence under cache_lock. No pointer or page pin is
 * retained, and no allocator, provider operation or sleeping is entered.
 */
uint64_t page_cache_pressure_epoch(void)
{
    uint64_t flags, result;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    result = pressure_epoch;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    return result;
}

/** @brief Drop matching idle clean pages without holding cache_lock across PMM.
 * @param node Borrowed type/volume/inode identity, nullable for a no-op.
 * @return None; pinned and non-discardable entries remain resident.
 */
void page_cache_invalidate_node(const struct storage_node *node)
{
    if (!node) return;
    for (;;) {
        uint64_t flags, phys = 0;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
            if (reclaimable(&entries[i]) && node_same_inode(&entries[i].node, node)) {
                phys = detach_entry(&entries[i],true); break;
            }
        }
        if (!phys) {
            for (struct page_cache_overflow *chunk = overflow; chunk && !phys; chunk = chunk->next) {
                for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
                    struct page_cache_entry *entry = &chunk->entries[i];
                    if (reclaimable(entry) && node_same_inode(&entry->node, node)) {
                        phys = detach_entry(entry,true); break;
                    }
                }
            }
        }
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (!phys) break;
        mm_free_page(phys);
    }
}

/** @brief Release idle entries previously marked retired.
 * @return None; pinned retired entries remain owned until their last release.
 * PMM and heap releases occur only after cache_lock is released.
 */
static void release_retired_entries(void)
{
    uint64_t flags;
    for (;;) {
        uint64_t phys = 0;
        struct page_cache_overflow *empty = NULL;
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i) {
            if (entries[i].used && entries[i].retired && !entries[i].refs) {
                phys = detach_entry(&entries[i],false); break;
            }
        }
        if (!phys) {
            for (struct page_cache_overflow **link = &overflow; *link && !phys; link = &(*link)->next) {
                struct page_cache_overflow *chunk = *link;
                for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i) {
                    struct page_cache_entry *entry = &chunk->entries[i];
                    if (entry->used && entry->retired && !entry->refs) {
                        phys = detach_entry(entry,false);
                        if (!chunk->used) { overflow_unlink(chunk); empty = chunk; }
                        break;
                    }
                }
                if (empty) break;
            }
        }
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        if (!phys) break;
        mm_free_page(phys); kernel_free(empty);
    }
}

/** @brief Retire a destroyed inode's keys while preserving outstanding pins.
 * @param node Exact destroyed identity, nullable for a no-op.
 * @return None. Caller serializes terminal destruction and reuse against loads.
 */
void page_cache_forget_node(const struct storage_node *node)
{
    if (!node) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i)
        if (entries[i].used && node_same_inode(&entries[i].node, node)) retire_entry(&entries[i]);
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next)
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i)
            if (chunk->entries[i].used && node_same_inode(&chunk->entries[i].node, node))
                retire_entry(&chunk->entries[i]);
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    release_retired_entries();
}

/** @brief Retire cache identities when a volume is authoritatively purged.
 * @param volume Volume slot under storage execution ownership.
 * @return None; physical pins survive until their final release. The caller
 * excludes new file faults and volume reuse; no cache lock spans frame frees.
 */
void page_cache_forget_volume(uint32_t volume)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    for (uint32_t i = 0; i < PAGE_CACHE_MAX; ++i)
        if (entries[i].used && entries[i].node.volume_id == volume) retire_entry(&entries[i]);
    for (struct page_cache_overflow *chunk = overflow; chunk; chunk = chunk->next)
        for (uint32_t i = 0; i < PAGE_CACHE_OVERFLOW_ENTRIES; ++i)
            if (chunk->entries[i].used && chunk->entries[i].node.volume_id == volume)
                retire_entry(&chunk->entries[i]);
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    release_retired_entries();
}

/** @brief Find the next old entry intersecting a committed zero range.
 * @param node Stable inode identity. @param start First byte. @param end Exclusive end.
 * @param after Last visited serial. @param cutoff Final serial at operation start.
 * @return Lowest unvisited matching entry, under cache_lock, or NULL.
 */
static struct page_cache_entry *next_zero_entry(const struct storage_node *node,
                                                 uint64_t start, uint64_t end,
                                                 uint64_t after, uint64_t cutoff)
{
    struct page_cache_entry *best = NULL;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        struct page_cache_overflow *chunk = pass ? overflow : NULL;
        if (pass && !chunk) continue;
        do {
            struct page_cache_entry *array = pass ? chunk->entries : entries;
            uint32_t count = pass ? PAGE_CACHE_OVERFLOW_ENTRIES : PAGE_CACHE_MAX;
            for (uint32_t i = 0; i < count; ++i) {
                struct page_cache_entry *entry = &array[i];
                if (!entry->used || entry->retired || entry->serial <= after ||
                    entry->serial > cutoff || (best && entry->serial >= best->serial) ||
                    !node_same_inode(&entry->node, node) || entry->offset >= end ||
                    (entry->offset < start && start - entry->offset >= PAGE_CACHE_PAGE_SIZE))
                    continue;
                best = entry;
            }
            if (!pass) break;
            chunk = chunk->next;
        } while (chunk);
    }
    return best;
}

/** @brief Apply an already committed zero/punch to resident aliases.
 * @param node Stable inode identity. @param offset Inclusive byte offset.
 * @param length Nonzero, non-overflowing committed range.
 * @return None. Caller serializes the provider mutation with file faults and
 * inode destruction; direct user writes race normally with the operation.
 * Each frame is pinned under cache_lock and cleared through the guaranteed
 * PMM direct map after unlocking; aliases and bytes outside the range survive.
 */
void page_cache_zero_range(const struct storage_node *node, uint64_t offset, uint64_t length)
{
    if (!node || !length || length > UINT64_MAX - offset) return;
    uint64_t flags, cursor = 0, cutoff, end = offset + length;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    cutoff = cache_serial;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
    for (;;) {
        kernel_spin_lock_irqsave(&cache_lock, &flags);
        struct page_cache_entry *entry = next_zero_entry(node, offset, end, cursor, cutoff);
        if (!entry) { kernel_spin_unlock_irqrestore(&cache_lock, flags); break; }
        retain_entry(entry);
        uint64_t phys = entry->phys, base = entry->offset;
        cursor = entry->serial;
        kernel_spin_unlock_irqrestore(&cache_lock, flags);
        uint64_t first = offset > base ? offset - base : 0;
        uint64_t bytes = PAGE_CACHE_PAGE_SIZE - first;
        if (end - (base + first) < bytes) bytes = end - (base + first);
        void *mapping = mm_phys_to_virt(phys);
        __builtin_memset((uint8_t *)mapping + first, 0, (size_t)bytes);
        page_cache_release(phys);
    }
}

/** @brief Publish an authoritative file size to all resident pages.
 * @param node Stable inode key. @param old_size Prior committed file size.
 * @param new_size New committed file size. @return None.
 * Caller serializes file faults and mutations. Equal sizes also refresh the
 * authoritative cap. Newly invalid bytes from shrink, including a shrink
 * first detected against a resident entry's prior size, are zeroed;
 * later growth reloads only the newly valid suffix before publishing a mapping.
 */
void page_cache_resize_node(const struct storage_node *node,uint64_t old_size,uint64_t new_size)
{
    if (!node) return;
    bool shrink=new_size<old_size;
    uint64_t flags; kernel_spin_lock_irqsave(&cache_lock,&flags);
    for (uint32_t pass=0;pass<2;++pass) {
        struct page_cache_overflow *chunk=pass ? overflow : NULL;
        if (pass && !chunk) continue;
        do {
            struct page_cache_entry *array=pass ? chunk->entries : entries;
            uint32_t count=pass ? PAGE_CACHE_OVERFLOW_ENTRIES : PAGE_CACHE_MAX;
            for (uint32_t i=0;i<count;++i) {
                struct page_cache_entry *entry=&array[i];
                if (!entry->used || entry->retired || !node_same_inode(&entry->node,node)) continue;
                if (entry->node.size>new_size) shrink=true;
                entry->node.size=new_size; entry->size_authoritative=1;
                uint32_t valid=page_valid_bytes(new_size,entry->offset);
                if (entry->valid_bytes>valid) entry->valid_bytes=valid;
            }
            if (!pass) break;
            chunk=chunk->next;
        } while (chunk);
    }
    kernel_spin_unlock_irqrestore(&cache_lock,flags);
    if (shrink && new_size<UINT64_MAX)
        page_cache_zero_range(node,new_size,UINT64_MAX-new_size);
}

/** @brief Snapshot resident entry and cache-hit counts.
 * @param count Optional resident entry output. @param hits Optional cumulative hit output.
 * @return None; both values are read under the same cache lock.
 */
void page_cache_stats(uint32_t *count, uint32_t *hits)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&cache_lock, &flags);
    if (count) *count = cache_report.resident > UINT32_MAX ? UINT32_MAX : (uint32_t)cache_report.resident;
    if (hits) *hits = cache_hits;
    kernel_spin_unlock_irqrestore(&cache_lock, flags);
}

/* Native filesystem modules share this core-owned interface. */
EXPORT_SYMBOL(page_cache_invalidate_node);
EXPORT_SYMBOL(page_cache_has_inode);
EXPORT_SYMBOL(page_cache_forget_node);
EXPORT_SYMBOL(page_cache_forget_volume);
EXPORT_SYMBOL(page_cache_resize_node);
EXPORT_SYMBOL(page_cache_zero_range);
