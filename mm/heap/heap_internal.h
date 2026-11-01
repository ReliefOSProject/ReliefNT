/* Private heap ownership indexes and slab entry points. */
#ifndef RELIEFNT_HEAP_INTERNAL_H
#define RELIEFNT_HEAP_INTERNAL_H

#include <reliefnt/types.h>

/** Intrusive node indexed by its own address; caller owns synchronization. */
struct heap_tree_node {
    struct heap_tree_node *left;
    struct heap_tree_node *right;
    int height;
};

/** @brief Insert a unique, initialized node into an AVL ownership tree.
 * @param root Current root or NULL. @param node Exclusively owned node.
 * @return New root; caller retains node ownership and holds the tree lock.
 */
struct heap_tree_node *heap_tree_insert(struct heap_tree_node *root, struct heap_tree_node *node);
/** @brief Remove an existing address from an AVL ownership tree.
 * @param root Current root. @param key Numeric address of the node.
 * @return New root; caller retains the removed node and holds the tree lock.
 */
struct heap_tree_node *heap_tree_remove(struct heap_tree_node *root, uintptr_t key);
/** @brief Find an owned node without dereferencing an untrusted address.
 * @param root Current root. @param key Numeric address of the node.
 * @return Borrowed node or NULL; valid only while the caller holds the tree lock.
 */
struct heap_tree_node *heap_tree_find(struct heap_tree_node *root, uintptr_t key);
/** @brief Reserve and clear a small-object slot.
 * @param size Requested bytes, 1 through 2048.
 * @return Owned, at least 16-byte-aligned zeroed payload, or NULL on exhaustion.
 * IRQ/task safe; no PMM calls or zeroing while holding the heap lock.
 */
void *slab_alloc_zero(size_t size);
/** @brief Consume any pointer into a currently owned slab page.
 * @param memory Candidate pointer, potentially interior, duplicate or invalid.
 * @return True if it belongs to a slab page, including an invalid slot; false otherwise.
 * Valid live slots release ownership; an empty slab is detached before PMM release.
 */
bool slab_free(void *memory);

#endif
