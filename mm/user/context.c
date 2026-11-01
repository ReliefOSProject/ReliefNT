/* Private metadata follows a CR3 lifetime, never a movable inline MM address. */
#include "memory_internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/swap.h>

#ifdef RELIEFNT_MM_TEST_STATS
static uint64_t mm_shared_test_steps;
#define MM_SHARED_STEP() (++mm_shared_test_steps)
#else
#define MM_SHARED_STEP() ((void)0)
#endif

/* AVL nodes index roots and both virtual and backing-offset span intervals. */
struct mm_index_node {
    struct mm_index_node *left, *right;
    uint64_t start, end, max_end;
    int height;
    void *owner;
};

/* Six 64-way levels cover all page indices in the low canonical user half. */
#define MM_PAGE_RADIX_BITS 6
#define MM_PAGE_RADIX_LEVELS 6
#define MM_PAGE_RADIX_SIZE (1u << MM_PAGE_RADIX_BITS)
_Static_assert(RELIEFNT_USER_TOP / PAGE_SIZE <=
               (1ULL << (MM_PAGE_RADIX_BITS * MM_PAGE_RADIX_LEVELS)),
               "shared page index must cover the complete user offset range");
struct mm_page_node {
    union { struct mm_page_node *branch; uint64_t phys; } slot[MM_PAGE_RADIX_SIZE];
    unsigned used;
};

struct mm_shared_object {
    uint32_t references;
    struct mm_page_node *pages;
    struct mm_index_node *spans;
};
struct mm_shared_span {
    struct mm_index_node virtual_index, object_index;
    struct mm_shared_span *staged_next;
    uint64_t start, end, offset;
    struct mm_shared_object *object;
};
struct mm_swap_emergency_chunk;
struct mm_swap_record {
    struct mm_index_node index;
    uint64_t token;
    struct mm_swap_emergency_chunk *emergency;
    struct mm_swap_record *free_next;
};

struct mm_swap_emergency_chunk {
    struct mm_swap_emergency_chunk *next, **previous;
    struct mm_swap_record *free;
    uint64_t physical;
    uint32_t used;
};
struct mm_context {
    struct mm_index_node identity_index;
    uint64_t cr3;
    uint64_t generation;
    uint64_t vma_revision;
    uint64_t device_revision;
    uint32_t lock_flags;
    struct mm_index_node *shared;
    struct mm_index_node *swap;
    uint64_t swap_count;
    struct mm_lock_intent *lock_intents;
    uint64_t emergency_physical;
};

/** @brief Ensure one pinned root has private metadata and an incarnation.
 * @param as Borrowed live root. @param emergency Permit reserve only in reclaim.
 * @return Zero or negative ENOMEM. No registry lock is held during allocation.
 */
static int mm_context_ensure_internal(struct address_space *as, bool emergency);

static struct mm_index_node *mm_contexts;
/* Accessed only in execution-owned swap-record operations. */
static struct mm_swap_emergency_chunk *mm_swap_emergency_chunks;
static uint64_t mm_context_generation;
static struct kernel_spinlock mm_context_lock = KERNEL_SPINLOCK_INIT;

/** @brief Get an AVL node's height. @param node Nullable node. @return Height. */
static int mm_index_height(const struct mm_index_node *node)
{ return node ? node->height : 0; }

/** @brief Recompute interval and height augmentation. @param node Live node. */
static void mm_index_update(struct mm_index_node *node)
{
    int left = mm_index_height(node->left), right = mm_index_height(node->right);
    node->height = 1 + (left > right ? left : right);
    node->max_end = node->end;
    if (node->left && node->left->max_end > node->max_end) node->max_end = node->left->max_end;
    if (node->right && node->right->max_end > node->max_end) node->max_end = node->right->max_end;
}

/** @brief Rotate an index right. @param root Subtree root. @return New root. */
static struct mm_index_node *mm_index_rotate_right(struct mm_index_node *root)
{
    struct mm_index_node *left = root->left;
    root->left = left->right;
    left->right = root;
    mm_index_update(root);
    mm_index_update(left);
    return left;
}

/** @brief Rotate an index left. @param root Subtree root. @return New root. */
static struct mm_index_node *mm_index_rotate_left(struct mm_index_node *root)
{
    struct mm_index_node *right = root->right;
    root->right = right->left;
    right->left = root;
    mm_index_update(root);
    mm_index_update(right);
    return right;
}

/** @brief Restore the AVL invariant. @param root Non-null subtree. @return New root. */
static struct mm_index_node *mm_index_balance(struct mm_index_node *root)
{
    mm_index_update(root);
    int balance = mm_index_height(root->left) - mm_index_height(root->right);
    if (balance > 1) {
        if (mm_index_height(root->left->right) > mm_index_height(root->left->left))
            root->left = mm_index_rotate_left(root->left);
        return mm_index_rotate_right(root);
    }
    if (balance < -1) {
        if (mm_index_height(root->right->left) > mm_index_height(root->right->right))
            root->right = mm_index_rotate_right(root->right);
        return mm_index_rotate_left(root);
    }
    return root;
}

/** @brief Order intervals including duplicate offsets. @param a First. @param b Second.
 * @return Negative, zero or positive order; node identity breaks equal starts.
 */
static int mm_index_compare(const struct mm_index_node *a, const struct mm_index_node *b)
{
    if (a->start != b->start) return a->start < b->start ? -1 : 1;
    uintptr_t ap = (uintptr_t)a, bp = (uintptr_t)b;
    return ap < bp ? -1 : ap > bp ? 1 : 0;
}

/** @brief Insert an unlinked node. @param root Nullable subtree. @param node New node.
 * @return Rebalanced root. No memory allocation occurs.
 */
static struct mm_index_node *mm_index_insert(struct mm_index_node *root, struct mm_index_node *node)
{
    if (!root) return node;
    MM_SHARED_STEP();
    if (mm_index_compare(node, root) < 0) root->left = mm_index_insert(root->left, node);
    else root->right = mm_index_insert(root->right, node);
    return mm_index_balance(root);
}

/** @brief Detach the least index node. @param root Non-null subtree.
 * @param out Receives detached node. @return Rebalanced remainder.
 */
static struct mm_index_node *mm_index_take_min(struct mm_index_node *root,
                                                struct mm_index_node **out)
{
    if (!root->left) { *out = root; return root->right; }
    root->left = mm_index_take_min(root->left, out);
    return mm_index_balance(root);
}

/** @brief Remove exactly one embedded index node. @param root Nullable subtree.
 * @param node Node previously inserted in this tree. @return Rebalanced root.
 */
static struct mm_index_node *mm_index_remove(struct mm_index_node *root, struct mm_index_node *node)
{
    if (!root) return NULL;
    MM_SHARED_STEP();
    int order = mm_index_compare(node, root);
    if (order < 0) root->left = mm_index_remove(root->left, node);
    else if (order > 0) root->right = mm_index_remove(root->right, node);
    else {
        if (!root->left) return root->right;
        if (!root->right) return root->left;
        struct mm_index_node *next;
        struct mm_index_node *right = mm_index_take_min(root->right, &next);
        next->left = root->left;
        next->right = right;
        return mm_index_balance(next);
    }
    return mm_index_balance(root);
}

/** @brief Find the first interval intersecting a query.
 * @param root Nullable augmented tree. @param first Inclusive byte offset.
 * @param last Exclusive byte offset. @return Borrowed matching node or NULL.
 */
static struct mm_index_node *mm_index_overlap(struct mm_index_node *root,
                                               uint64_t first, uint64_t last)
{
    if (!root || first >= last || root->max_end <= first) return NULL;
    MM_SHARED_STEP();
    struct mm_index_node *found = mm_index_overlap(root->left, first, last);
    if (found) return found;
    if (root->start < last && root->end > first) return root;
    if (root->start >= last) return NULL;
    return mm_index_overlap(root->right, first, last);
}

/** @brief Test whether one surviving span covers an entire removed interval.
 * @param root Nullable augmented object-span index. @param first First byte.
 * @param last Exclusive end. @return True if one indexed span covers both bounds.
 */
static bool mm_index_covers(struct mm_index_node *root, uint64_t first, uint64_t last)
{
    if (!root || root->max_end < last) return false;
    MM_SHARED_STEP();
    if (mm_index_covers(root->left, first, last)) return true;
    if (root->start <= first && root->end >= last) return true;
    return root->start <= first && mm_index_covers(root->right, first, last);
}

/** @brief Initialize an embedded interval node. @param node Unlinked node.
 * @param first Inclusive bound. @param last Exclusive bound. @param owner Container.
 */
static void mm_index_init(struct mm_index_node *node, uint64_t first, uint64_t last, void *owner)
{
    *node = (struct mm_index_node){.start = first, .end = last, .max_end = last,
                                   .height = 1, .owner = owner};
}

/** @brief Locate a root while the private context registry lock is held.
 * @param cr3 Physical page-table root. @return Borrowed record or NULL.
 */
static struct mm_context *mm_context_find(uint64_t cr3)
{
    struct mm_index_node *node = mm_contexts;
    while (node) {
        MM_SHARED_STEP();
        if (cr3 == node->start) return node->owner;
        node = cr3 < node->start ? node->left : node->right;
    }
    return NULL;
}

/** @brief Borrow root metadata while the caller pins its address-space lifetime.
 * @param as Pinned live root. @return Stable record or NULL.
 */
static struct mm_context *mm_context_borrow(struct address_space *as)
{
    if (!as || !as->cr3) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return ctx;
}

/** @brief Bound fresh child swap-record and shared-span metadata pages.
 * @param as Execution-pinned source root. @param records Source swap leaves.
 * @return Fresh-page upper bound or UINT64_MAX; includes a child context when
 * either class is needed, assumes no free slab slots, and takes no references.
 */
uint64_t mm_fork_metadata_page_bound(struct address_space *as, uint64_t records)
{
    if (!as || !as->cr3) return UINT64_MAX;
    struct mm_context *source = mm_context_borrow(as);
    uint64_t cursor = 0, spans = 0;
    struct mm_index_node *found;
    while (source && (found = mm_index_overlap(source->shared, cursor, RELIEFNT_USER_TOP))) {
        struct mm_shared_span *span = found->owner;
        if (spans == UINT64_MAX) return UINT64_MAX;
        ++spans; cursor = span->end;
    }
    uint64_t slots = kernel_heap_small_page_bound(sizeof(struct mm_swap_record), records);
    uint64_t copies = kernel_heap_small_page_bound(sizeof(struct mm_shared_span), spans);
    uint64_t context = records || spans;
    if (slots == UINT64_MAX || copies == UINT64_MAX || slots > UINT64_MAX - copies ||
        slots + copies > UINT64_MAX - context) return UINT64_MAX;
    return slots + copies + context;
}

/** @brief Transfer an opaque slot reference into one root's sparse index.
 * @param as Live root under the writer. @param page Aligned virtual page.
 * @param token Caller-owned slot. @return Zero on transfer or negative errno.
 */
/** @brief Allocate one record from a reserve page at the audited reclaim edge.
 * @return Owned record or NULL. No heap, I/O or recursive reclaim is entered.
 */
static struct mm_swap_record *mm_swap_emergency_record(void)
{
    struct mm_swap_emergency_chunk *chunk = mm_swap_emergency_chunks;
    while (chunk && !chunk->free) chunk = chunk->next;
    if (!chunk) {
        uint64_t physical = mm_alloc_emergency_page();
        if (!physical) return NULL;
        chunk = mm_phys_to_virt(physical);
        if (!chunk) { mm_free_page(physical); return NULL; }
        *chunk = (struct mm_swap_emergency_chunk){.physical = physical};
        uintptr_t first = ((uintptr_t)chunk + sizeof(*chunk) +
            _Alignof(struct mm_swap_record) - 1) & ~((uintptr_t)_Alignof(struct mm_swap_record) - 1);
        uintptr_t end = (uintptr_t)chunk + PAGE_SIZE;
        while (first <= end && sizeof(struct mm_swap_record) <= end - first) {
            struct mm_swap_record *record = (void *)first;
            record->free_next = chunk->free; chunk->free = record;
            first += sizeof(*record);
        }
        if (!chunk->free) { mm_free_page(physical); return NULL; }
        chunk->next = mm_swap_emergency_chunks;
        chunk->previous = &mm_swap_emergency_chunks;
        if (chunk->next) chunk->next->previous = &chunk->next;
        mm_swap_emergency_chunks = chunk;
    }
    struct mm_swap_record *record = chunk->free;
    chunk->free = record->free_next; ++chunk->used;
    *record = (struct mm_swap_record){.emergency = chunk};
    return record;
}

/** @brief Release a heap or reserve-backed swap-record allocation.
 * @param record Owned record. Frees an empty reserve page immediately.
 */
static void mm_swap_record_free(struct mm_swap_record *record)
{
    struct mm_swap_emergency_chunk *chunk = record->emergency;
    if (!chunk) { kernel_free(record); return; }
    record->free_next = chunk->free; chunk->free = record;
    if (--chunk->used) return;
    *chunk->previous = chunk->next;
    if (chunk->next) chunk->next->previous = chunk->previous;
    mm_free_page(chunk->physical);
}

/** @brief Insert a swap record, optionally admitting the audited reserve.
 * @param as Pinned root. @param page Aligned address. @param token Owned slot.
 * @param emergency True only at the outer-writer reclaim boundary.
 * @return Zero on transfer or negative errno with token still owned by caller.
 */
static int mm_swap_record_insert_internal(struct address_space *as, uint64_t page,
                                          uint64_t token, bool emergency)
{
    if (!as || !token || (page & (PAGE_SIZE - 1)) || page < RELIEFNT_USER_BASE ||
        page >= RELIEFNT_USER_TOP) return -RELIEFOS_EINVAL;
    if (mm_context_ensure_internal(as, emergency) < 0) {
         return -RELIEFOS_ENOMEM;
    }
    struct mm_context *ctx = mm_context_borrow(as);
    if (!ctx || mm_index_overlap(ctx->swap, page, page + PAGE_SIZE)) {

        return -RELIEFOS_EINVAL;
    }
    struct mm_swap_record *record = kernel_malloc(sizeof(*record));
    if (record) *record = (struct mm_swap_record){0};
    else if (emergency) record = mm_swap_emergency_record();
    if (!record) {  return -RELIEFOS_ENOMEM; }
    record->token = token;
    mm_index_init(&record->index, page, page + PAGE_SIZE, record);
    ctx->swap = mm_index_insert(ctx->swap, &record->index);
    ++ctx->swap_count;
    return 0;
}

/** @brief Insert a slot-owning record with ordinary metadata allocation.
 * @param as Writer-pinned root. @param page Aligned user address.
 * @param token Caller-owned slot token. @return Zero after ownership transfer,
 * or negative errno while the caller retains the token; no reserve is used.
 */
int mm_swap_record_insert(struct address_space *as, uint64_t page, uint64_t token)
{ return mm_swap_record_insert_internal(as, page, token, false); }

/** @brief Insert a slot-owning record at the audited reclaim boundary.
 * @param as Writer-pinned root. @param page Aligned user address.
 * @param token Caller-owned slot token. @return Zero after ownership transfer,
 * or negative errno with token retained; reserve metadata may be used.
 */
int mm_swap_record_insert_reclaim(struct address_space *as, uint64_t page, uint64_t token)
{ return mm_swap_record_insert_internal(as, page, token, true); }

/** @brief Borrow a slot token while the caller keeps its root stable.
 * @param as Live root under the writer. @param page Aligned virtual page.
 * @return Full opaque token or zero; the record retains its reference.
 */
uint64_t mm_swap_record_lookup(struct address_space *as, uint64_t page)
{
    if ((page & (PAGE_SIZE - 1)) || page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) return 0;
    struct mm_context *ctx = mm_context_borrow(as);
    struct mm_index_node *found = ctx ? mm_index_overlap(ctx->swap, page, page + PAGE_SIZE) : NULL;
    return found ? ((struct mm_swap_record *)found->owner)->token : 0;
}

/** @brief Find the lowest swap-record address at or above an aligned cursor.
 * @param as Execution-owned root. @param start Inclusive address.
 * @param page Receives the address. @param token Receives the borrowed token.
 * @return True when found, with no allocation or reference acquisition.
 */
bool mm_swap_record_next(struct address_space *as, uint64_t start,
                         uint64_t *page, uint64_t *token)
{
    if (!page || !token || (start & (PAGE_SIZE - 1)) ||
        start < RELIEFNT_USER_BASE || start >= RELIEFNT_USER_TOP) return false;
    struct mm_context *ctx = mm_context_borrow(as);
    struct mm_index_node *node = ctx ? ctx->swap : NULL, *best = NULL;
    while (node) {
        if (node->start >= start) { best = node; node = node->left; }
        else node = node->right;
    }
    if (!best) return false;
    *page = best->start;
    *token = ((struct mm_swap_record *)best->owner)->token;
    return true;
}

/** @brief Release one indexed slot without allocation or I/O.
 * @param as Live root under the writer. @param page Aligned virtual page.
 */
void mm_swap_record_remove(struct address_space *as, uint64_t page)
{
    if ((page & (PAGE_SIZE - 1)) || page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) return;
    struct mm_context *ctx = mm_context_borrow(as);
    struct mm_index_node *found = ctx ? mm_index_overlap(ctx->swap, page, page + PAGE_SIZE) : NULL;
    if (!found) return;
    struct mm_swap_record *record = found->owner;
    ctx->swap = mm_index_remove(ctx->swap, found);
    --ctx->swap_count;
    swap_slot_put(record->token);
    mm_swap_record_free(record);
}

/** @brief Retain one source slot before publishing the child's swap leaf.
 * @param source Live source root. @param target New child root.
 * @param source_page Aligned source address. @param target_page Aligned target address.
 * @return Zero or negative errno.
 */
int mm_swap_copy_record(struct address_space *source, struct address_space *target,
                        uint64_t source_page, uint64_t target_page)
{
    uint64_t token = mm_swap_record_lookup(source, source_page);
    if (!token) {  return -RELIEFOS_EINVAL; }
    int ret = swap_slot_get(token);
    if (ret < 0) {  return ret; }
    ret = mm_swap_record_insert(target, target_page, token);
    if (ret < 0) swap_slot_put(token);
    return ret;
}

/** @brief Retain one source slot before publishing the child's swap leaf.
 * @param source Live source root. @param target New child root.
 * @param page Aligned virtual page. @return Zero or negative errno.
 */
int mm_swap_clone_leaf(struct address_space *source, struct address_space *target,
                       uint64_t page)
{
    return mm_swap_copy_record(source, target, page, page);
}

/** @brief Look up a shared frame by object page index.
 * @param object Borrowed object. @param index Page index. @return Physical page or zero.
 */
static uint64_t mm_page_lookup(const struct mm_shared_object *object, uint64_t index)
{
    if (index >= (1ULL << (MM_PAGE_RADIX_BITS * MM_PAGE_RADIX_LEVELS))) return 0;
    struct mm_page_node *node = object->pages;
    for (int level = MM_PAGE_RADIX_LEVELS - 1; node && level >= 0; --level) {
        MM_SHARED_STEP();
        unsigned slot = (unsigned)((index >> (level * MM_PAGE_RADIX_BITS)) & (MM_PAGE_RADIX_SIZE - 1));
        if (!level) return node->slot[slot].phys;
        node = node->slot[slot].branch;
    }
    return 0;
}

/** @brief Insert one owned frame without publishing partially allocated paths.
 * @param object Borrowed object. @param index Page index. @param phys Owned frame.
 * @return True on success; false leaves the frame with the caller.
 */
static bool mm_page_insert(struct mm_shared_object *object, uint64_t index, uint64_t phys)
{
    if (index >= (1ULL << (MM_PAGE_RADIX_BITS * MM_PAGE_RADIX_LEVELS))) return false;
    struct mm_page_node **link = &object->pages, *parent = NULL;
    for (int level = MM_PAGE_RADIX_LEVELS - 1; level >= 0; --level) {
        MM_SHARED_STEP();
        if (!*link) {
            struct mm_page_node *staged[MM_PAGE_RADIX_LEVELS] = {0};
            for (int i = level; i >= 0; --i) {
                MM_SHARED_STEP();
                staged[i] = kernel_malloc(sizeof(*staged[i]));
                if (!staged[i]) {
                    for (int j = level; j > i; --j) kernel_free(staged[j]);
                    return false;
                }
                *staged[i] = (struct mm_page_node){0};
            }
            for (int i = level; i > 0; --i) {
                unsigned slot = (unsigned)((index >> (i * MM_PAGE_RADIX_BITS)) & (MM_PAGE_RADIX_SIZE - 1));
                staged[i]->slot[slot].branch = staged[i - 1];
                staged[i]->used = 1;
            }
            staged[0]->slot[index & (MM_PAGE_RADIX_SIZE - 1)].phys = phys;
            staged[0]->used = 1;
            *link = staged[level];
            if (parent) ++parent->used;
            return true;
        }
        struct mm_page_node *node = *link;
        unsigned slot = (unsigned)((index >> (level * MM_PAGE_RADIX_BITS)) & (MM_PAGE_RADIX_SIZE - 1));
        if (!level) {
            if (node->slot[slot].phys) return false;
            node->slot[slot].phys = phys;
            ++node->used;
            return true;
        }
        parent = node;
        link = &node->slot[slot].branch;
    }
    return false;
}

/** @brief Release every frame and node in a radix subtree.
 * @param node Nullable subtree. @param level Remaining radix level.
 */
static void mm_page_destroy(struct mm_page_node *node, int level)
{
    if (!node) return;
    for (unsigned i = 0; i < MM_PAGE_RADIX_SIZE; ++i) {
        MM_SHARED_STEP();
        if (level) mm_page_destroy(node->slot[i].branch, level - 1);
        else if (node->slot[i].phys) mm_free_page(node->slot[i].phys);
    }
    kernel_free(node);
}

/** @brief Retire unowned pages only in a removed object interval.
 * @param object Backing object with its surviving span index.
 * @param node Nullable radix subtree. @param level Subtree level.
 * @param base First index in subtree. @param first First removed index.
 * @param last Exclusive removed index. @return Surviving subtree or NULL.
 */
static struct mm_page_node *mm_page_prune(struct mm_shared_object *object,
    struct mm_page_node *node, int level, uint64_t base, uint64_t first, uint64_t last)
{
    if (!node) return NULL;
    uint64_t width = 1ULL << (level * MM_PAGE_RADIX_BITS);
    uint64_t limit = base + width * MM_PAGE_RADIX_SIZE;
    if (last <= base || first >= limit) return node;
    unsigned begin = first > base ? (unsigned)((first - base) / width) : 0;
    unsigned end = last < limit ? (unsigned)((last - base + width - 1) / width) : MM_PAGE_RADIX_SIZE;
    for (unsigned i = begin; i < end; ++i) {
        MM_SHARED_STEP();
        uint64_t index = base + (uint64_t)i * width;
        if (level) {
            if (!node->slot[i].branch) continue;
            node->slot[i].branch = mm_page_prune(object, node->slot[i].branch,
                                                 level - 1, index, first, last);
            if (!node->slot[i].branch) --node->used;
        } else if (node->slot[i].phys &&
                   !mm_index_overlap(object->spans, index * PAGE_SIZE, (index + 1) * PAGE_SIZE)) {
            mm_free_page(node->slot[i].phys);
            node->slot[i].phys = 0;
            --node->used;
        }
    }
    if (node->used) return node;
    kernel_free(node);
    return NULL;
}

/** @brief Initialize and index a span in its shared object.
 * @param span New span with bounds and object set. Adds one object reference.
 */
static void mm_span_register(struct mm_shared_span *span)
{
    mm_index_init(&span->virtual_index, span->start, span->end, span);
    mm_index_init(&span->object_index, span->offset,
                  span->offset + span->end - span->start, span);
    ++span->object->references;
    span->object->spans = mm_index_insert(span->object->spans, &span->object_index);
}

/** @brief Publish an already registered span in one root. @param ctx Root.
 * @param span Registered span, not yet in the root's virtual index.
 */
static void mm_span_publish(struct mm_context *ctx, struct mm_shared_span *span)
{ ctx->shared = mm_index_insert(ctx->shared, &span->virtual_index); }

/** @brief Remove both indexes while keeping the span's object reference.
 * @param ctx Root containing span. @param span Registered, published span.
 */
static void mm_span_unlink(struct mm_context *ctx, struct mm_shared_span *span)
{
    ctx->shared = mm_index_remove(ctx->shared, &span->virtual_index);
    span->object->spans = mm_index_remove(span->object->spans, &span->object_index);
}

/** @brief Reinsert a resized span without changing its object reference.
 * @param ctx Root. @param span Unlinked span with updated bounds.
 */
static void mm_span_reindex(struct mm_context *ctx, struct mm_shared_span *span)
{
    mm_index_init(&span->virtual_index, span->start, span->end, span);
    mm_index_init(&span->object_index, span->offset,
                  span->offset + span->end - span->start, span);
    span->object->spans = mm_index_insert(span->object->spans, &span->object_index);
    mm_span_publish(ctx, span);
}

/** @brief Prune a removed object interval, or free its final owner.
 * @param object Object after span indexes were updated. @param first First byte.
 * @param last Exclusive byte. @param drop_ref True to release one span reference.
 */
static void mm_shared_retire(struct mm_shared_object *object, uint64_t first,
                              uint64_t last, bool drop_ref)
{
    if (drop_ref && !--object->references) {
        mm_page_destroy(object->pages, MM_PAGE_RADIX_LEVELS - 1);
        kernel_free(object);
        return;
    }
    if (first >= last || mm_index_covers(object->spans, first, last)) return;
    object->pages = mm_page_prune(object, object->pages, MM_PAGE_RADIX_LEVELS - 1,
                                  0, first / PAGE_SIZE, last / PAGE_SIZE);
}

/** @brief Create a distinct shared-anonymous RAM object for one new mmap.
 * @param as Pinned root. @param start First page. @param end Exclusive end.
 * @return Zero or negative ENOMEM, without publishing partial state.
 */
int mm_shared_create(struct address_space *as, uint64_t start, uint64_t end)
{
    if (mm_context_ensure(as) < 0) return -RELIEFOS_ENOMEM;
    struct mm_shared_object *object = kernel_malloc(sizeof(*object));
    if (!object) return -RELIEFOS_ENOMEM;
    struct mm_shared_span *span = kernel_malloc(sizeof(*span));
    if (!span) { kernel_free(object); return -RELIEFOS_ENOMEM; }
    struct mm_context *ctx = mm_context_borrow(as);
    *object = (struct mm_shared_object){0};
    *span = (struct mm_shared_span){.start = start, .end = end, .object = object};
    mm_span_register(span);
    mm_span_publish(ctx, span);
    return 0;
}

/** @brief Cut an unmapped interval from indexed shared backing spans.
 * @param as Pinned root. @param start First removed page. @param end Exclusive end.
 * @return Zero or negative ENOMEM before changes. At most one split is staged.
 */
int mm_shared_forget(struct address_space *as, uint64_t start, uint64_t end)
{
    struct mm_context *ctx = mm_context_borrow(as);
    if (!ctx) return 0;
    struct mm_shared_span *right = NULL;
    struct mm_index_node *found = mm_index_overlap(ctx->shared, start, end);
    if (found) {
        struct mm_shared_span *span = found->owner;
        if (span->start < start && span->end > end) {
            right = kernel_malloc(sizeof(*right));
            if (!right) return -RELIEFOS_ENOMEM;
        }
    }
    while ((found = mm_index_overlap(ctx->shared, start, end))) {
        struct mm_shared_span *span = found->owner;
        struct mm_shared_object *object = span->object;
        uint64_t first = start > span->start ? start : span->start;
        uint64_t last = end < span->end ? end : span->end;
        uint64_t object_first = span->offset + first - span->start;
        uint64_t object_last = span->offset + last - span->start;
        uint64_t old_start = span->start, old_end = span->end, old_offset = span->offset;
        mm_span_unlink(ctx, span);
        bool drop = first == old_start && last == old_end;
        if (drop) kernel_free(span);
        else {
            if (first == old_start) {
                span->start = last;
                span->offset = object_last;
            } else {
                span->end = first;
                if (last < old_end) {
                    *right = (struct mm_shared_span){.start = last, .end = old_end,
                        .offset = old_offset + last - old_start, .object = object};
                    mm_span_register(right);
                    mm_span_publish(ctx, right);
                    right = NULL;
                }
            }
            mm_span_reindex(ctx, span);
        }
        mm_shared_retire(object, object_first, object_last, drop);
    }
    kernel_free(right);
    return 0;
}

/** @brief Retain all shared-anonymous spans for an independent fork root.
 * @param src Pinned source root. @param dst Newly created destination root.
 * @return Zero or negative ENOMEM; destination destruction rolls back a failure.
 */
int mm_shared_clone(struct address_space *src, struct address_space *dst)
{
    struct mm_context *source = mm_context_borrow(src);
    if (!source || !source->shared) return 0;
    if (mm_context_ensure(dst) < 0) {
         return -RELIEFOS_ENOMEM;
    }
    struct mm_context *target = mm_context_borrow(dst);
    uint64_t cursor = 0;
    struct mm_index_node *found;
    while ((found = mm_index_overlap(source->shared, cursor, RELIEFNT_USER_TOP))) {
        struct mm_shared_span *span = found->owner;
        struct mm_shared_span *copy = kernel_malloc(sizeof(*copy));
        if (!copy) {  return -RELIEFOS_ENOMEM; }
        *copy = (struct mm_shared_span){.start = span->start, .end = span->end,
            .offset = span->offset, .object = span->object};
        mm_span_register(copy);
        mm_span_publish(target, copy);
        cursor = span->end;
    }
    return 0;
}

/** @brief Install a shared-anonymous page while retaining its backing owner.
 * @param as Pinned root. @param page Aligned virtual address. @param flags PTE flags.
 * @return Zero, -ENOENT for a non-owned range, or -ENOMEM on allocation failure.
 */
int mm_shared_fault(struct address_space *as, uint64_t page, uint64_t flags)
{
    struct mm_context *ctx = mm_context_borrow(as);
    struct mm_index_node *found = ctx ? mm_index_overlap(ctx->shared, page, page + PAGE_SIZE) : NULL;
    if (!found) return -RELIEFOS_ENOENT;
    struct mm_shared_span *span = found->owner;
    uint64_t index = (span->offset + page - span->start) / PAGE_SIZE;
    uint64_t phys = mm_page_lookup(span->object, index);
    if (!phys) {
        phys = mm_alloc_page();
        void *data = phys ? mm_phys_to_virt(phys) : NULL;
        if (!data) { if (phys) mm_free_page(phys); return -RELIEFOS_ENOMEM; }
        __builtin_memset(data, 0, PAGE_SIZE);
        if (!mm_page_insert(span->object, index, phys)) {
            mm_free_page(phys);
            return -RELIEFOS_ENOMEM;
        }
    }
    mm_retain_page(phys);
    if (!address_space_map_user_page(as, page, phys, flags | RELIEFNT_PAGE_SHARED)) {
        mm_free_page(phys);
        return -RELIEFOS_ENOMEM;
    }
    return 0;
}

/** @brief Inspect shared-anonymous backing without allocating or taking a pin.
 * @param as Pinned root. @param page Aligned user page. @return True if resident.
 */
bool mm_shared_resident(struct address_space *as, uint64_t page)
{
    struct mm_context *ctx = mm_context_borrow(as);
    struct mm_index_node *found = ctx ? mm_index_overlap(ctx->shared, page, page + PAGE_SIZE) : NULL;
    if (!found) return false;
    struct mm_shared_span *span = found->owner;
    return mm_page_lookup(span->object, (span->offset + page - span->start) / PAGE_SIZE) != 0;
}

/** @brief Retain source backing objects at the moved virtual offsets.
 * @param as Pinned execution-owned MM. @param source Old first page.
 * @param target Reserved new first page. @param length Aligned source bytes.
 * @return Zero or negative ENOMEM, leaving the original source spans intact.
 */
int mm_shared_alias(struct address_space *as, uint64_t source, uint64_t target,
                    uint64_t length)
{
    struct mm_context *ctx = mm_context_borrow(as);
    if (!ctx) return -RELIEFOS_ENOMEM;
    struct mm_shared_span *staged = NULL;
    uint64_t covered = 0, cursor = source;
    struct mm_index_node *found;
    while ((found = mm_index_overlap(ctx->shared, cursor, source + length))) {
        struct mm_shared_span *span = found->owner;
        uint64_t first = span->start > source ? span->start : source;
        uint64_t last = span->end < source + length ? span->end : source + length;
        struct mm_shared_span *copy = kernel_malloc(sizeof(*copy));
        if (!copy) goto fail;
        *copy = (struct mm_shared_span){.staged_next = staged,
            .start = target + first - source, .end = target + last - source,
            .offset = span->offset + first - span->start, .object = span->object};
        mm_span_register(copy);
        staged = copy;
        covered += last - first;
        cursor = last;
    }
    if (covered != length || mm_shared_forget(as, target, target + length) < 0) goto fail;
    while (staged) {
        struct mm_shared_span *copy = staged;
        staged = copy->staged_next;
        mm_span_publish(ctx, copy);
    }
    return 0;
fail:
    while (staged) {
        struct mm_shared_span *copy = staged;
        staged = copy->staged_next;
        struct mm_shared_object *object = copy->object;
        uint64_t first = copy->offset, last = first + copy->end - copy->start;
        object->spans = mm_index_remove(object->spans, &copy->object_index);
        kernel_free(copy);
        mm_shared_retire(object, first, last, true);
    }
    return -RELIEFOS_ENOMEM;
}


/** @brief Release either a normal or exceptional root context allocation.
 * @param ctx Owned detached context, nullable; releases its original backing.
 */
static void mm_context_free(struct mm_context *ctx)
{
    if (!ctx) return;
    if (ctx->emergency_physical) mm_free_page(ctx->emergency_physical);
    else kernel_free(ctx);
}

/** @brief Ensure a root identity; only reclaim may use the emergency reserve.
 * @param as Live root pinned by the caller. @param emergency Permit exceptional
 * reserve allocation at the audited reclaim boundary. @return Zero or negative
 * ENOMEM; allocation happens without the registry lock, and no root pin moves.
 */
static int mm_context_ensure_internal(struct address_space *as, bool emergency)
{
    if (!as || !as->cr3) return -RELIEFOS_ENOMEM;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    bool exists = mm_context_find(as->cr3) != NULL;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    if (exists) return 0;
    struct mm_context *ctx = kernel_malloc(sizeof(*ctx));
    uint64_t physical = 0;
    if (!ctx && emergency) {
        physical = mm_alloc_emergency_page();
        if (physical) ctx = mm_phys_to_virt(physical);
        if (physical && !ctx) mm_free_page(physical);
    }
    if (!ctx) return -RELIEFOS_ENOMEM;
    *ctx = (struct mm_context){.cr3 = as->cr3, .emergency_physical = physical};
    mm_index_init(&ctx->identity_index, as->cr3, as->cr3 + PAGE_SIZE, ctx);
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    if (!mm_context_find(as->cr3)) {
        ctx->generation = ++mm_context_generation;
        if (!ctx->generation) ctx->generation = ++mm_context_generation;
        mm_contexts = mm_index_insert(mm_contexts, &ctx->identity_index);
        ctx = NULL;
    }
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    mm_context_free(ctx);
    return 0;
}

/** @brief Ensure private context metadata with ordinary allocation only.
 * @param as Caller-pinned live address space. @return Zero or negative ENOMEM;
 * the root is not retained and allocation is outside the registry lock.
 */
int mm_context_ensure(struct address_space *as)
{ return mm_context_ensure_internal(as, false); }

/** @brief Acquire a root plus generation identity for private MM services.
 * @param as Pinned address space. @param out Required identity output.
 * @return Zero or negative ENOMEM; no pointer to movable task state escapes.
 */
int mm_context_identity(struct address_space *as, struct mm_identity *out)
{
    if (!out || mm_context_ensure(as) < 0) return -RELIEFOS_ENOMEM;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    *out = (struct mm_identity){.cr3 = ctx->cr3, .generation = ctx->generation};
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return 0;
}

/** @brief Ensure an incarnation for a live root before a zero-progress wait.
 * @param as Root pinned by the caller's current transaction. @param generation
 * Receives its nonzero generation. @return Zero or ENOMEM/EINVAL.
 * The returned identity is not an asynchronous address-space reference.
 */
int mm_context_ensure_generation(struct address_space *as,uint64_t *generation)
{
    if(!generation)return -RELIEFOS_EINVAL;
    struct mm_identity identity;int ret=mm_context_identity(as,&identity);
    if(ret)return ret;
    *generation=identity.generation;return 0;
}

/** @brief Ensure a reclaim root's generation without requiring ordinary heap.
 * @param as Root pinned by the outer reclaim writer. @param generation Required
 * output receiving a nonzero incarnation. @return Zero or negative errno; an
 * emergency context remains owned until the corresponding root is destroyed.
 */
int mm_context_ensure_reclaim_generation(struct address_space *as, uint64_t *generation)
{
    if (!generation) return -RELIEFOS_EINVAL;
    int rc = mm_context_ensure_internal(as, true);
    if (rc) return rc;
    *generation = mm_context_generation_existing(as);
    return *generation ? 0 : -RELIEFOS_ENOMEM;
}

/** @brief Observe an existing root incarnation without allocating a context.
 * @param as Live root held by its caller. @return Generation, or zero if absent.
 */
uint64_t mm_context_generation_existing(const struct address_space *as)
{
    if (!as || !as->cr3) return 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    uint64_t generation = ctx ? ctx->generation : 0;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return generation;
}

/** @brief Observe the device-leaf revision without retaining or allocating.
 * @param as Live root pinned by the caller, nullable. @return The revision,
 * or zero when no private root context exists. The registry lock is IRQ-safe.
 */
uint64_t mm_context_device_revision(const struct address_space *as)
{
    if (!as || !as->cr3) return 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    uint64_t revision = ctx ? ctx->device_revision : 0;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return revision;
}

/** @brief Publish a device-leaf mutation for suspended bounded walkers.
 * @param as Execution-pinned root, nullable. The caller serializes its PTE
 * mutation; no context is allocated and no reference leaves this call.
 */
void mm_context_device_changed(const struct address_space *as)
{
    if (!as || !as->cr3) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    if (ctx) ++ctx->device_revision;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
}

/** @brief Snapshot indexed nonresident leaves while the caller pins the root.
 * @param as Borrowed execution-pinned root, nullable. @return Slot-owning leaf
 * count or zero without context; no slot or root reference changes hands.
 */
uint64_t mm_swap_count_existing(const struct address_space *as)
{
    struct mm_context *ctx = mm_context_borrow((struct address_space *)as);
    return ctx ? ctx->swap_count : 0;
}

/** @brief Publish one stack-owned checked-population eviction exclusion.
 * @param as Execution-owned root. @param start Inclusive first page.
 * @param end Exclusive end. @param intent Caller-owned stable stack record.
 * @return Zero or negative ENOMEM. It neither changes nor reserves lock quota.
 */
int mm_context_lock_intent_begin(struct address_space *as, uint64_t start,
                                 uint64_t end, struct mm_lock_intent *intent)
{
    if (!intent || start >= end || mm_context_ensure(as) < 0) return -RELIEFOS_ENOMEM;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    *intent = (struct mm_lock_intent){.next = ctx->lock_intents,
        .identity = {.cr3 = ctx->cr3, .generation = ctx->generation},
        .start = start, .end = end};
    ctx->lock_intents = intent;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return 0;
}

/** @brief Retire a checked-population exclusion without allocating or callbacks.
 * @param intent Caller-owned record. A destroyed/reused root is never followed.
 */
void mm_context_lock_intent_end(struct mm_lock_intent *intent)
{
    if (!intent || !intent->identity.generation) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(intent->identity.cr3);
    if (ctx && ctx->generation == intent->identity.generation) {
        struct mm_lock_intent **link = &ctx->lock_intents;
        while (*link && *link != intent) link = &(*link)->next;
        if (*link) *link = intent->next;
    }
    *intent = (struct mm_lock_intent){0};
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
}

/** @brief Query the eviction exclusions belonging to the current root.
 * @param as Execution-owned root. @param start Inclusive first address.
 * @param end Exclusive last address. @return True if any intent intersects.
 */
bool mm_context_lock_intent_intersects(struct address_space *as, uint64_t start,
                                       uint64_t end)
{
    if (!as || !as->cr3 || start >= end) return false;
    uint64_t flags;
    bool found = false;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    for (struct mm_lock_intent *it = ctx ? ctx->lock_intents : NULL; it; it = it->next)
        if (it->start < end && start < it->end) { found = true; break; }
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return found;
}

/** @brief Read the live root's population-relevant mutation revision.
 * @param as Execution-owned live address space. @return Current revision, or
 * zero when no context exists. A walker ensures the context before sampling.
 */
uint64_t mm_context_vma_revision(struct address_space *as)
{
    if (!as || !as->cr3) return 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    uint64_t revision = ctx ? ctx->vma_revision : 0;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return revision;
}

/** @brief Record a population-relevant mutation without allocating.
 * @param as Execution-owned live address space. An absent private context has
 * no active population walker; creation starts with a fresh root generation.
 * While holding the execution writer, VMA mutators call this before publishing,
 * removing, splitting or changing population-relevant metadata. The common
 * leaf-unmap path also calls it before the first resident PTE is removed.
 * A transaction may coalesce multiple changes into one increment.
 * The registry lock is released before any execution suspension or callback.
 */
void mm_context_vma_changed(struct address_space *as)
{
    if (!as || !as->cr3) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    if (ctx) ++ctx->vma_revision;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
}

/** @brief Snapshot the future-lock policy of one live root.
 * @param as Borrowed pinned address space. @return MCL policy bits or zero.
 */
uint32_t mm_lock_flags(struct address_space *as)
{
    if (!as || !as->cr3) return 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    uint32_t result = ctx ? ctx->lock_flags : 0;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    return result;
}

/** @brief Store future-lock bits on an ensured CR3 identity.
 * @param as Borrowed pinned address space. @param value Policy bits to publish.
 */
void mm_context_set_lock_flags(struct address_space *as, uint32_t value)
{
    if (!as || !as->cr3) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    if (ctx) ctx->lock_flags = value;
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
}

/** @brief Detach metadata before a CR3 frame becomes reusable.
 * @param as Root whose final owner is destroying it; NULL is ignored.
 */
void mm_context_destroy(struct address_space *as)
{
    if (!as || !as->cr3) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&mm_context_lock, &flags);
    struct mm_context *ctx = mm_context_find(as->cr3);
    if (ctx) mm_contexts = mm_index_remove(mm_contexts, &ctx->identity_index);
    kernel_spin_unlock_irqrestore(&mm_context_lock, flags);
    while (ctx && ctx->swap) {
        struct mm_swap_record *record = ctx->swap->owner;
        ctx->swap = mm_index_remove(ctx->swap, &record->index);
        swap_slot_put(record->token);
        mm_swap_record_free(record);
    }
    while (ctx && ctx->shared) {
        struct mm_shared_span *span = ctx->shared->owner;
        struct mm_shared_object *object = span->object;
        uint64_t first = span->offset, last = first + span->end - span->start;
        mm_span_unlink(ctx, span);
        kernel_free(span);
        mm_shared_retire(object, first, last, true);
    }
    mm_context_free(ctx);
}
