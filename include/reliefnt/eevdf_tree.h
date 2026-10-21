/* Private augmented red-black index; included after EEVDF type definitions. */
#ifndef RELIEFNT_EEVDF_TREE_H
#define RELIEFNT_EEVDF_TREE_H

static inline bool eevdf_tree_red(const struct eevdf_node *n)
{ return n && n->red; }

static inline void eevdf_tree_update(struct eevdf_node *n)
{
    struct eevdf_entity *e = n->entity;
    n->min_vruntime = e->vruntime;
    n->min_delayed_vruntime = e->delayed ? e->vruntime : EEVDF_VRUNTIME_MAX;
    n->delayed_count = e->delayed ? 1u : 0u;
    struct eevdf_node *children[2] = {n->left, n->right};
    for (unsigned i = 0; i < 2; ++i) {
        struct eevdf_node *child = children[i];
        if (!child) continue;
        if (child->min_vruntime < n->min_vruntime)
            n->min_vruntime = child->min_vruntime;
        if (child->min_delayed_vruntime < n->min_delayed_vruntime)
            n->min_delayed_vruntime = child->min_delayed_vruntime;
        n->delayed_count += child->delayed_count;
    }
}

static inline void eevdf_tree_propagate(struct eevdf_node *n)
{
    for (; n; n = n->parent) eevdf_tree_update(n);
}

static inline bool eevdf_tree_before(const struct eevdf_node *a,
                                      const struct eevdf_node *b)
{
    if (a->entity->deadline != b->entity->deadline)
        return a->entity->deadline < b->entity->deadline;
    /* The former head-inserted list selected the newest enqueue on ties. */
    return a->order > b->order;
}

static inline void eevdf_tree_rotate_left(struct eevdf_queue *q,
                                            struct eevdf_node *n)
{
    struct eevdf_node *right = n->right;
    n->right = right->left;
    if (right->left) right->left->parent = n;
    right->parent = n->parent;
    if (!n->parent) q->root = right;
    else if (n == n->parent->left) n->parent->left = right;
    else n->parent->right = right;
    right->left = n;
    n->parent = right;
    eevdf_tree_update(n);
    eevdf_tree_update(right);
}

static inline void eevdf_tree_rotate_right(struct eevdf_queue *q,
                                             struct eevdf_node *n)
{
    struct eevdf_node *left = n->left;
    n->left = left->right;
    if (left->right) left->right->parent = n;
    left->parent = n->parent;
    if (!n->parent) q->root = left;
    else if (n == n->parent->left) n->parent->left = left;
    else n->parent->right = left;
    left->right = n;
    n->parent = left;
    eevdf_tree_update(n);
    eevdf_tree_update(left);
}

static inline void eevdf_tree_insert(struct eevdf_queue *q,
                                       struct eevdf_node *n)
{
    struct eevdf_node **link = &q->root, *parent = NULL;
    while (*link) {
        parent = *link;
        link = eevdf_tree_before(n, parent) ? &parent->left : &parent->right;
    }
    n->left = n->right = NULL;
    n->parent = parent;
    n->red = true;
    *link = n;
    eevdf_tree_propagate(n);
    while (n->parent && n->parent->red) {
        struct eevdf_node *p = n->parent, *g = p->parent;
        if (p == g->left) {
            struct eevdf_node *uncle = g->right;
            if (eevdf_tree_red(uncle)) {
                p->red = uncle->red = false;
                g->red = true;
                n = g;
            } else {
                if (n == p->right) {
                    n = p;
                    eevdf_tree_rotate_left(q, n);
                    p = n->parent;
                    g = p->parent;
                }
                p->red = false;
                g->red = true;
                eevdf_tree_rotate_right(q, g);
            }
        } else {
            struct eevdf_node *uncle = g->left;
            if (eevdf_tree_red(uncle)) {
                p->red = uncle->red = false;
                g->red = true;
                n = g;
            } else {
                if (n == p->left) {
                    n = p;
                    eevdf_tree_rotate_right(q, n);
                    p = n->parent;
                    g = p->parent;
                }
                p->red = false;
                g->red = true;
                eevdf_tree_rotate_left(q, g);
            }
        }
    }
    q->root->red = false;
}

static inline void eevdf_tree_transplant(struct eevdf_queue *q,
                                           struct eevdf_node *old,
                                           struct eevdf_node *replacement)
{
    if (!old->parent) q->root = replacement;
    else if (old == old->parent->left) old->parent->left = replacement;
    else old->parent->right = replacement;
    if (replacement) replacement->parent = old->parent;
}

/* NULL leaves are black; parent is explicit while the replacement is NULL. */
static inline void eevdf_tree_erase_fix(struct eevdf_queue *q,
                                          struct eevdf_node *n,
                                          struct eevdf_node *parent)
{
    while (n != q->root && !eevdf_tree_red(n)) {
        if (!parent) break;
        if (n == parent->left) {
            struct eevdf_node *sibling = parent->right;
            if (eevdf_tree_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                eevdf_tree_rotate_left(q, parent);
                sibling = parent->right;
            }
            if (!sibling) { n = parent; parent = n->parent; continue; }
            if (!eevdf_tree_red(sibling->left) && !eevdf_tree_red(sibling->right)) {
                sibling->red = true;
                n = parent;
                parent = n->parent;
            } else {
                if (!eevdf_tree_red(sibling->right)) {
                    if (sibling->left) sibling->left->red = false;
                    sibling->red = true;
                    eevdf_tree_rotate_right(q, sibling);
                    sibling = parent->right;
                }
                sibling->red = parent->red;
                parent->red = false;
                if (sibling->right) sibling->right->red = false;
                eevdf_tree_rotate_left(q, parent);
                n = q->root;
                parent = NULL;
            }
        } else {
            struct eevdf_node *sibling = parent->left;
            if (eevdf_tree_red(sibling)) {
                sibling->red = false;
                parent->red = true;
                eevdf_tree_rotate_right(q, parent);
                sibling = parent->left;
            }
            if (!sibling) { n = parent; parent = n->parent; continue; }
            if (!eevdf_tree_red(sibling->left) && !eevdf_tree_red(sibling->right)) {
                sibling->red = true;
                n = parent;
                parent = n->parent;
            } else {
                if (!eevdf_tree_red(sibling->left)) {
                    if (sibling->right) sibling->right->red = false;
                    sibling->red = true;
                    eevdf_tree_rotate_left(q, sibling);
                    sibling = parent->left;
                }
                sibling->red = parent->red;
                parent->red = false;
                if (sibling->left) sibling->left->red = false;
                eevdf_tree_rotate_right(q, parent);
                n = q->root;
                parent = NULL;
            }
        }
    }
    if (n) n->red = false;
}

static inline void eevdf_tree_erase(struct eevdf_queue *q,
                                      struct eevdf_node *n)
{
    struct eevdf_node *moved = n, *child, *parent, *changed = NULL;
    bool was_red = moved->red;
    if (!n->left) {
        child = n->right;
        parent = n->parent;
        eevdf_tree_transplant(q, n, child);
        changed = parent;
    } else if (!n->right) {
        child = n->left;
        parent = n->parent;
        eevdf_tree_transplant(q, n, child);
        changed = parent;
    } else {
        moved = n->right;
        while (moved->left) moved = moved->left;
        was_red = moved->red;
        child = moved->right;
        if (moved->parent == n) {
            parent = moved;
            if (child) child->parent = moved;
        } else {
            parent = moved->parent;
            changed = parent;
            eevdf_tree_transplant(q, moved, child);
            moved->right = n->right;
            moved->right->parent = moved;
        }
        eevdf_tree_transplant(q, n, moved);
        moved->left = n->left;
        moved->left->parent = moved;
        moved->red = n->red;
        eevdf_tree_update(moved);
    }
    if (changed) eevdf_tree_propagate(changed);
    if (moved != n) eevdf_tree_propagate(moved);
    if (!was_red) eevdf_tree_erase_fix(q, child, parent);
    n->left = n->right = n->parent = NULL;
    n->red = false;
}

static inline bool eevdf_tree_eligible(const struct eevdf_node *n,
                                         int64_t average, bool delayed_only)
{
    return n && (delayed_only
        ? n->delayed_count && n->min_delayed_vruntime <= average
        : n->min_vruntime <= average);
}

/* Descend one root-to-leaf path: subtree minima prune ineligible deadlines. */
static inline struct eevdf_entity *eevdf_tree_find(struct eevdf_queue *q,
                                                    int64_t average,
                                                    bool delayed_only)
{
    struct eevdf_node *n = q->root;
    if (!eevdf_tree_eligible(n, average, delayed_only)) return NULL;
    while (n) {
        if (eevdf_tree_eligible(n->left, average, delayed_only)) {
            n = n->left;
        } else if (n->entity->vruntime <= average &&
                   (!delayed_only || n->entity->delayed)) {
            return n->entity;
        } else {
            n = n->right;
        }
    }
    return NULL;
}
#endif
