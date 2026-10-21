/* ReliefOS EEVDF policy. Time values are microseconds, serialized by sched lock. */
#ifndef RELIEFNT_EEVDF_H
#define RELIEFNT_EEVDF_H
#include <reliefnt/types.h>

#define RELIEFNT_EEVDF_RBTREE 1
#define EEVDF_SLICE_US 10000ULL
#define EEVDF_VRUNTIME_MAX ((int64_t)0x7fffffffffffffffLL)
struct eevdf_node;
struct eevdf_entity {
    /* Same size/offset as the former next link: task layout stays unchanged. */
    struct eevdf_node *node;
    int64_t vruntime, deadline, lag;
    uint64_t remainder, runtime;
    uint32_t weight, cpu;
    bool initialized, queued, delayed;
};
struct eevdf_queue {
    struct eevdf_entity *head;
    int64_t clock;
    struct eevdf_node *root, *tail;
    uint64_t load, next_order;
    /* Sum of (vruntime - clock) * weight, keeping uptime out of products. */
    int64_t weighted_sum;
    uint32_t nr_queued, nr_active;
};
struct eevdf_node {
    struct eevdf_node *left, *right, *parent;
    /* Membership order is only used for unlink and rare order-wrap rebasing. */
    struct eevdf_node *previous, *next;
    struct eevdf_entity *entity;
    struct eevdf_queue *queue;
    uint64_t order;
    int64_t min_vruntime, min_delayed_vruntime;
    int64_t cached_vruntime, cached_deadline;
    uint32_t cached_weight, delayed_count;
    bool red, cached_delayed;
};
#include <reliefnt/eevdf_tree.h>

/* Nodes live beside scheduler task allocations, never allocated under a queue
 * operation. Standalone policy callers bind their own stable node storage. */
static inline void eevdf_bind(struct eevdf_entity *e, struct eevdf_node *node)
{
    *node = (struct eevdf_node){.entity = e};
    e->node = node;
}

/* Linux-compatible nice weights; policy code is independently implemented.
 * Reference: Linux 6.12 fair.c (eligibility, placement, delayed dequeue). */
static inline uint32_t eevdf_weight(int nice)
{
    static const uint32_t weights[40] = {
        88761,71755,56483,46273,36291,29154,23254,18705,14949,11916,
        9548,7620,6100,4904,3906,3121,2501,1991,1586,1277,
        1024,820,655,526,423,335,272,215,172,137,110,87,70,56,45,36,29,23,18,15
    };
    if (nice < -20) nice = -20;
    if (nice > 19) nice = 19;
    return weights[nice + 20];
}
static inline int64_t eevdf_slice(const struct eevdf_entity *e)
{ return (int64_t)(EEVDF_SLICE_US * 1024 / e->weight); }

static inline int64_t eevdf_average(struct eevdf_queue *q, uint64_t *load)
{
    *load = q->load;
    if (!*load) return q->clock;
    int64_t sum = q->weighted_sum;
    /* Floor, including negative values, keeps an entity at V eligible. */
    return q->clock + sum / (int64_t)*load - (sum < 0 && sum % (int64_t)*load != 0);
}

static inline void eevdf_set_clock(struct eevdf_queue *q, int64_t clock)
{
    q->weighted_sum = (int64_t)((__int128)q->weighted_sum -
        ((__int128)clock - q->clock) * q->load);
    q->clock = clock;
}

/* Keep the aggregate and augmented index in step with key/state changes.
 * Deadline-preserving runtime updates only propagate minima, with no rebalance. */
static inline void eevdf_refresh(struct eevdf_entity *e)
{
    struct eevdf_node *n = e->node;
    struct eevdf_queue *q = n ? n->queue : NULL;
    if (!q) return;
    bool rekey = n->cached_deadline != e->deadline;
    if (rekey) eevdf_tree_erase(q, n);
    q->weighted_sum = (int64_t)((__int128)q->weighted_sum -
        ((__int128)n->cached_vruntime - q->clock) * n->cached_weight +
        ((__int128)e->vruntime - q->clock) * e->weight);
    q->load = q->load - n->cached_weight + e->weight;
    if (n->cached_delayed != e->delayed) {
        if (e->delayed) --q->nr_active;
        else ++q->nr_active;
    }
    n->cached_vruntime = e->vruntime;
    n->cached_deadline = e->deadline;
    n->cached_weight = e->weight;
    n->cached_delayed = e->delayed;
    if (rekey) eevdf_tree_insert(q, n);
    else eevdf_tree_propagate(n);
}

static inline void eevdf_set_delayed(struct eevdf_entity *e, bool delayed)
{
    if (e->delayed == delayed) return;
    e->delayed = delayed;
    eevdf_refresh(e);
}

static inline void eevdf_dequeue(struct eevdf_queue *q, struct eevdf_entity *e)
{
    uint64_t load;
    int64_t average = eevdf_average(q, &load);
    e->lag = average - e->vruntime;
    int64_t limit = 2 * eevdf_slice(e);
    if (e->lag > limit) e->lag = limit;
    if (e->lag < -limit) e->lag = -limit;
    struct eevdf_node *n = e->node;
    __int128 weighted_sum = q->weighted_sum;
    if (n && n->queue == q) {
        eevdf_tree_erase(q, n);
        weighted_sum -= ((__int128)n->cached_vruntime - q->clock) * n->cached_weight;
        q->load -= n->cached_weight;
        --q->nr_queued;
        if (!n->cached_delayed) --q->nr_active;
        if (n->previous) n->previous->next = n->next;
        else q->head = n->next ? n->next->entity : NULL;
        if (n->next) n->next->previous = n->previous;
        else q->tail = n->previous;
        n->previous = n->next = NULL;
        n->queue = NULL;
    }
    e->queued = false;
    e->delayed = false;
    /* Remove and rebase before narrowing: the temporary remaining sum can
     * exceed int64 even when the old scanner's normalized result fits. */
    q->weighted_sum = (int64_t)(weighted_sum -
        ((__int128)average - q->clock) * q->load);
    q->clock = average;
}

static inline void eevdf_enqueue(struct eevdf_queue *q, struct eevdf_entity *e, int nice)
{
    uint64_t load;
    int64_t average = eevdf_average(q, &load);
    e->weight = eevdf_weight(nice);
    int64_t lag = e->initialized && load ? e->lag : 0;
    if (load) lag += lag * e->weight / (int64_t)load;
    e->vruntime = average - lag;
    e->deadline = e->vruntime + eevdf_slice(e) / (e->initialized ? 1 : 2);
    e->initialized = true;
    e->queued = true;
    e->delayed = false;
    e->remainder = 0;
    struct eevdf_node *n = e->node;
    /* Renumber oldest-to-newest before unsigned wrap. Relative tie order is
     * unchanged, so the existing tree remains ordered without rebuilding it. */
    if (q->next_order == ~0ULL) {
        uint64_t order = 0;
        for (struct eevdf_node *p = q->tail; p; p = p->previous)
            p->order = ++order;
        q->next_order = order;
    }
    n->order = ++q->next_order;
    n->queue = q;
    n->cached_vruntime = e->vruntime;
    n->cached_deadline = e->deadline;
    n->cached_weight = e->weight;
    n->cached_delayed = false;
    n->previous = NULL;
    n->next = q->head ? q->head->node : NULL;
    if (n->next) n->next->previous = n;
    else q->tail = n;
    q->head = e;
    eevdf_set_clock(q, average);
    q->weighted_sum = (int64_t)((__int128)q->weighted_sum +
        ((__int128)e->vruntime - q->clock) * e->weight);
    q->load += e->weight;
    ++q->nr_queued;
    ++q->nr_active;
    eevdf_tree_insert(q, n);
}

static inline void eevdf_account(struct eevdf_entity *e, uint64_t delta)
{
    e->runtime += delta;
    uint64_t scaled = delta * 1024 + e->remainder;
    e->vruntime += (int64_t)(scaled / e->weight);
    e->remainder = scaled % e->weight;
    if (e->vruntime >= e->deadline) e->deadline = e->vruntime + eevdf_slice(e);
    eevdf_refresh(e);
}

static inline void eevdf_sleep(struct eevdf_queue *q, struct eevdf_entity *e)
{
    uint64_t load;
    if (e->vruntime > eevdf_average(q, &load)) eevdf_set_delayed(e, true);
    else eevdf_dequeue(q, e);
}

static inline void eevdf_yield(struct eevdf_entity *e)
{
    e->deadline += eevdf_slice(e);
    eevdf_refresh(e);
}

static inline void eevdf_reweight(struct eevdf_queue *q, struct eevdf_entity *e, int nice)
{
    uint32_t weight = eevdf_weight(nice), old = e->weight;
    if (weight == old) return;
    if (!e->queued) {
        /* Stored lag is in virtual units of the previous weight. */
        e->lag = e->lag * old / weight;
        e->weight = weight;
        e->remainder = 0;
        return;
    }
    int64_t remaining = e->deadline - e->vruntime;
    bool delayed = e->delayed;
    eevdf_dequeue(q, e);
    e->lag = e->lag * old / weight;
    eevdf_enqueue(q, e, nice);
    e->deadline = e->vruntime + remaining * old / weight;
    e->delayed = delayed;
    eevdf_refresh(e);
}

static inline struct eevdf_entity *eevdf_pick(struct eevdf_queue *q)
{
    for (;;) {
        uint64_t load;
        int64_t average = eevdf_average(q, &load);
        struct eevdf_entity *delayed = eevdf_tree_find(q, average, true);
        if (!delayed) {
            eevdf_set_clock(q, average);
            return eevdf_tree_find(q, average, false);
        }
        eevdf_dequeue(q, delayed);
        /* Once sleep debt has decayed, it must not become a wakeup bonus. */
        delayed->lag = 0;
    }
}
#endif
