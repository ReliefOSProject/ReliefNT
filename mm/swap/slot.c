/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/memory_stats.h>
#include <linux/errno.h>

extern void memory_event_add(enum memory_event, uint64_t) __attribute__((weak));

static struct kernel_spinlock swap_lock=KERNEL_SPINLOCK_INIT;
static struct swap_area *areas;
static uint32_t next_serial;
static uint64_t allocation_clock;

/** @brief Borrow a prepared slot by its valid page index.
 * @param area Prepared area. @param page Index at most last_page.
 * @return Borrowed metadata; caller protects live areas with swap_lock.
 */
static struct swap_slot *slot_at(struct swap_area *area,uint32_t page)
{ return &area->blocks[page/SWAP_SLOTS_PER_BLOCK]->slots[page%SWAP_SLOTS_PER_BLOCK]; }

/** @brief Find a published area named by the upper half of a token.
 * @param token Opaque token. @return Borrowed area or NULL with swap_lock held.
 */
static struct swap_area *find_area(uint64_t token)
{
    uint32_t serial=(uint32_t)(token>>32);
    for (struct swap_area *a=areas; a; a=a->next) if (a->serial==serial) return a;
    return NULL;
}

/** @brief Publish preallocated slot metadata with a never-reused identity.
 * @param area Owned prepared area. @return Zero or negative errno.
 */
int swap_area_publish(struct swap_area *area)
{
    if (!area || !area->blocks || area->published) return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    int rc=0;
    if (area->published) rc=-EBUSY;
    else if (next_serial==UINT32_MAX) rc=-EOVERFLOW;
    else {
        area->serial=++next_serial;area->published=true;
        struct swap_area **end=&areas;while (*end) end=&(*end)->next;*end=area;
    }
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return rc;
}

/** @brief Retire an area only after every allocated/in-flight slot is released.
 * @param area Published area. @return Zero or -EBUSY/-EINVAL.
 */
int swap_area_unpublish(struct swap_area *area)
{
    if (!area) return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area **link=&areas;while (*link && *link!=area) link=&(*link)->next;
    int rc=!*link ? -EINVAL : area->draining || area->free!=area->total ? -EBUSY : 0;
    if (!rc) {*link=area->next;area->next=NULL;area->published=false;}
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return rc;
}

/** @brief Disable allocations without detaching a referenced area.
 * @param area Published area. @return Zero, EBUSY or EINVAL.
 * swap_lock serializes this state with allocation and area publication.
 */
int swap_area_begin_drain(struct swap_area *area)
{
    if (!area) return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *item=areas;while (item && item!=area) item=item->next;
    int rc=!item ? -EINVAL : area->draining ? -EBUSY : 0;
    if (!rc) area->draining=true;
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return rc;
}

/** @brief Re-enable allocations after an unsuccessful evacuation.
 * @param area Still-owned area, nullable. Slot references are unchanged.
 */
void swap_area_abort_drain(struct swap_area *area)
{
    if (!area) return;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *item=areas;while (item && item!=area) item=item->next;
    if (item) area->draining=false;
    kernel_spin_unlock_irqrestore(&swap_lock,flags);
}

/** @brief Remove a draining area only when all slot references are gone.
 * @param area Deactivation-owned published area. @return Zero or negative errno.
 */
int swap_area_finish_drain(struct swap_area *area)
{
    if (!area) return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area **link=&areas;while (*link && *link!=area) link=&(*link)->next;
    int rc=!*link || !area->draining ? -EINVAL : area->free!=area->total ? -EBUSY : 0;
    if (!rc) {*link=area->next;area->next=NULL;area->published=false;area->draining=false;}
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return rc;
}

/** @brief Reserve one slot with priority selection and equal-priority fairness.
 * @return Opaque token or zero; no heap or I/O while swap_lock is held.
 */
uint64_t swap_slot_alloc(void)
{
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *best=NULL;
    for (struct swap_area *a=areas; a; a=a->next)
        if (!a->draining && a->free && (!best || a->priority>best->priority ||
            (a->priority==best->priority && a->last_allocation<best->last_allocation))) best=a;
    uint64_t token=0;
    if (best) {
        uint32_t page=best->free_head;struct swap_slot *slot=slot_at(best,page);
        best->free_head=slot->next;--best->free;
        slot->next=0;slot->refs=1;slot->state=SWAP_SLOT_RESERVED;
        if (allocation_clock==UINT64_MAX) {
            for (struct swap_area *a=areas; a; a=a->next) a->last_allocation=0;
            allocation_clock=0;
        }
        best->last_allocation=++allocation_clock;
        token=((uint64_t)best->serial<<32)|page;
    }
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return token;
}

/** @brief Acquire a second reference to one allocated slot.
 * @param token Live opaque token. @return Zero or -EINVAL/-EOVERFLOW.
 */
int swap_slot_get(uint64_t token)
{
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *area=find_area(token);uint32_t page=(uint32_t)token;
    struct swap_slot *slot=area && page && page<=area->last_page ? slot_at(area,page) : NULL;
    int rc=!slot || !slot->refs ? -EINVAL : slot->refs==UINT32_MAX ? -EOVERFLOW : 0;
    if (!rc) ++slot->refs;
    kernel_spin_unlock_irqrestore(&swap_lock,flags);return rc;
}

/** @brief Return an unreferenced slot to its area's intrusive free list.
 * @param area Live area. @param slot Unreferenced slot. @param page Slot index.
 * swap_lock is held; in-flight work always owns its own reference.
 */
static void release_slot(struct swap_area *area,struct swap_slot *slot,uint32_t page)
{
    slot->state=SWAP_SLOT_FREE;slot->next=area->free_head;
    area->free_head=page;++area->free;
}

/** @brief Consume one reference, recycling only after the last in-flight user.
 * @param token Owned opaque reference; zero is ignored. A caller must not use
 * a token after its final put because a later allocation can reuse that slot.
 */
void swap_slot_put(uint64_t token)
{
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *area=find_area(token);uint32_t page=(uint32_t)token;
    struct swap_slot *slot=area && page && page<=area->last_page ? slot_at(area,page) : NULL;
    if (slot && slot->refs && !--slot->refs) release_slot(area,slot,page);
    kernel_spin_unlock_irqrestore(&swap_lock,flags);
}

/** @brief Run one synchronous transfer while holding an independent slot ref.
 * @param token Held slot. @param physical Caller-retained aligned page.
 * @param write True for an initial write, false for a valid read.
 * @return Zero or terminal errno. Existing valid data is never overwritten.
 */
static int submit(uint64_t token,uint64_t physical,bool write)
{
    if (!physical || (physical&4095u)) return -EINVAL;
    uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    struct swap_area *area=find_area(token);uint32_t page=(uint32_t)token;
    struct swap_slot *slot=area && page && page<=area->last_page ? slot_at(area,page) : NULL;
    int rc=!slot || !slot->refs ? -EINVAL : slot->refs==UINT32_MAX ? -EOVERFLOW :
        slot->state!=(write ? SWAP_SLOT_RESERVED : SWAP_SLOT_VALID) ? -EBUSY : 0;
    if (!rc) {++slot->refs;slot->state=write ? SWAP_SLOT_WRITING : SWAP_SLOT_READING;}
    kernel_spin_unlock_irqrestore(&swap_lock,flags);if (rc) return rc;
    rc=swap_backend_page_io(area,page,physical,write);
    /* This backend has no queued request; no EAGAIN may masquerade as pending. */
    if (rc==-EAGAIN) rc=-EIO;
    kernel_spin_lock_irqsave(&swap_lock,&flags);
    slot->state=write && rc ? SWAP_SLOT_RESERVED : SWAP_SLOT_VALID;
    if (!--slot->refs) release_slot(area,slot,page);
    kernel_spin_unlock_irqrestore(&swap_lock,flags);
    if (!rc && memory_event_add) memory_event_add(write ? MEMORY_EVENT_PSWPOUT : MEMORY_EVENT_PSWPIN, 1);
    return rc;
}

/** @brief Write an allocated slot once without replacing an earlier valid copy.
 * @param token Held slot. @param physical Retained aligned physical page.
 * @return Zero or terminal errno.
 */
int swap_slot_submit_write(uint64_t token,uint64_t physical) { return submit(token,physical,true); }
/** @brief Read an allocated valid slot, preserving backing on failure.
 * @param token Held slot. @param physical Retained aligned destination page.
 * @return Zero or terminal errno.
 */
int swap_slot_submit_read(uint64_t token,uint64_t physical) { return submit(token,physical,false); }
/** @brief Copy one published area's capacity and used-slot state.
 * @param area Execution-owned published area. @param total Capacity output.
 * @param used In-use slots output. @param priority Signed priority output.
 * @return Zero or -EINVAL. swap_lock protects counters; no I/O/allocation.
 */
int swap_area_report(struct swap_area *area, uint64_t *total, uint64_t *used,
                     int32_t *priority)
{
    if (!area || !total || !used || !priority) return -EINVAL;
    uint64_t flags; kernel_spin_lock_irqsave(&swap_lock, &flags);
    struct swap_area *a = areas; while (a && a != area) a = a->next;
    if (!a) { kernel_spin_unlock_irqrestore(&swap_lock, flags); return -EINVAL; }
    *total = a->total; *used = a->total - a->free; *priority = a->priority;
    kernel_spin_unlock_irqrestore(&swap_lock, flags); return 0;
}

/** @brief Snapshot usable system totals across all published areas.
 * @param out Required writable result, NULL ignored. No allocation or I/O.
 * Draining capacity contributes only its still-used slots, never free slots.
 */
void swap_get_stats(struct swap_stats *out)
{
    if (!out) return;
    struct swap_stats stats={0};uint64_t flags;kernel_spin_lock_irqsave(&swap_lock,&flags);
    for (struct swap_area *a=areas; a; a=a->next) {
        uint64_t used=a->total-a->free, total=a->draining ? used : a->total;
        uint64_t free=a->draining ? 0 : a->free;
        stats.total=total>UINT64_MAX-stats.total ? UINT64_MAX : stats.total+total;
        stats.free=free>UINT64_MAX-stats.free ? UINT64_MAX : stats.free+free;
        stats.used=used>UINT64_MAX-stats.used ? UINT64_MAX : stats.used+used;
    }
    kernel_spin_unlock_irqrestore(&swap_lock,flags);*out=stats;
}
/** @brief No requests are queued by the synchronous backend.
 * @param budget Ignored bounded request budget.
 */
void swap_io_poll(uint32_t budget) { (void)budget; }
