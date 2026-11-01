/* Intrusive AVL index for actual heap-owned pages and large allocations. */
#include "heap_internal.h"

/** @brief Read a subtree height. @param node Borrowed subtree. @return Height or zero. */
static int node_height(const struct heap_tree_node *node)
{
    return node ? node->height : 0;
}

/** @brief Update a node after child changes. @param node Exclusively locked node. */
static void update_height(struct heap_tree_node *node)
{
    int left = node_height(node->left), right = node_height(node->right);
    node->height = 1 + (left > right ? left : right);
}

/** @brief Rotate a tree left. @param root Locked subtree root. @return New root. */
static struct heap_tree_node *rotate_left(struct heap_tree_node *root)
{
    struct heap_tree_node *next = root->right;
    root->right = next->left;
    next->left = root;
    update_height(root);
    update_height(next);
    return next;
}

/** @brief Rotate a tree right. @param root Locked subtree root. @return New root. */
static struct heap_tree_node *rotate_right(struct heap_tree_node *root)
{
    struct heap_tree_node *next = root->left;
    root->left = next->right;
    next->right = root;
    update_height(root);
    update_height(next);
    return next;
}

/** @brief Restore AVL balance after one update. @param root Locked root. @return Balanced root. */
static struct heap_tree_node *rebalance(struct heap_tree_node *root)
{
    update_height(root);
    int balance = node_height(root->left) - node_height(root->right);
    if (balance > 1) {
        if (node_height(root->left->left) < node_height(root->left->right))
            root->left = rotate_left(root->left);
        return rotate_right(root);
    }
    if (balance < -1) {
        if (node_height(root->right->right) < node_height(root->right->left))
            root->right = rotate_right(root->right);
        return rotate_left(root);
    }
    return root;
}

/** @brief Insert a unique node into a locked AVL tree.
 * @param root Current root. @param node Initialized unique node. @return New root.
 */
struct heap_tree_node *heap_tree_insert(struct heap_tree_node *root, struct heap_tree_node *node)
{
    if (!root) return node;
    if ((uintptr_t)node < (uintptr_t)root) root->left = heap_tree_insert(root->left, node);
    else root->right = heap_tree_insert(root->right, node);
    return rebalance(root);
}

/** @brief Remove and return the smallest node from a locked subtree.
 * @param root Nonempty subtree. @param minimum Receives its detached minimum.
 * @return Balanced subtree without that node.
 */
static struct heap_tree_node *detach_min(struct heap_tree_node *root, struct heap_tree_node **minimum)
{
    if (!root->left) {
        *minimum = root;
        return root->right;
    }
    root->left = detach_min(root->left, minimum);
    return rebalance(root);
}

/** @brief Remove an owned node by address from a locked AVL tree.
 * @param root Current root. @param key Node address. @return New root.
 */
struct heap_tree_node *heap_tree_remove(struct heap_tree_node *root, uintptr_t key)
{
    if (!root) return NULL;
    if (key < (uintptr_t)root) root->left = heap_tree_remove(root->left, key);
    else if (key > (uintptr_t)root) root->right = heap_tree_remove(root->right, key);
    else {
        if (!root->left) return root->right;
        if (!root->right) return root->left;
        struct heap_tree_node *successor;
        struct heap_tree_node *right = detach_min(root->right, &successor);
        successor->left = root->left;
        successor->right = right;
        return rebalance(successor);
    }
    return rebalance(root);
}

/** @brief Find an owned node in a locked AVL tree.
 * @param root Current root. @param key Numeric address. @return Borrowed node or NULL.
 */
struct heap_tree_node *heap_tree_find(struct heap_tree_node *root, uintptr_t key)
{
    while (root) {
        if (key == (uintptr_t)root) return root;
        root = key < (uintptr_t)root ? root->left : root->right;
    }
    return NULL;
}
