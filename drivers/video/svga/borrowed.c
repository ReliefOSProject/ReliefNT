#include <reliefnt/export.h>
#include "device.h"
#include <reliefnt/module.h>

/* Bound each IRQ-disabled pass while allowing inline page-number packets to
 * queue in FIFO order without a host round trip between adjacent chunks. */
#define SVGA_GMR_MAPPING_CHUNKS_PER_PASS 4u

static int submit_mapping_chunk_locked(uint32_t id, struct svga_gmr *g)
{
    uint32_t offset = g->mapped_pages + g->map_pending_pages;
    uint32_t count = g->pages - offset;
    if (count > 256u) count = 256u;
    uint32_t ppns[256];
    uint32_t base = (uint32_t)(g->phys >> 12);
    for (uint32_t i = 0; i < count; ++i) ppns[i] = base + offset + i;

    uint32_t words[14u + 256u];
    uint32_t at = 0;
    if (!g->defined) {
        words[at++] = SVGA_CMD_DEFINE_GMR2;
        words[at++] = id;
        words[at++] = g->pages;
    }
    words[at++] = SVGA_CMD_REMAP_GMR2;
    words[at++] = id;
    words[at++] = SVGA_REMAP_GMR2_PPN32;
    words[at++] = offset;
    words[at++] = count;
    for (uint32_t i = 0; i < count; ++i) words[at++] = ppns[i];
    if (!g->mob_defined) {
        const struct svga_mob *mob = &svga.external_mobs[id];
        SVGA3dCmdDefineGBMob define = {g->mobid, mob->depth,
            svga_gb_backing_root(mob), mob->bytes};
        words[at++] = SVGA_3D_CMD_DEFINE_GB_MOB;
        words[at++] = sizeof(define);
        const uint32_t *body = (const uint32_t *)&define;
        for (uint32_t i = 0; i < sizeof(define) / 4; ++i) words[at++] = body[i];
    }
    svga_handle fence = 0;
    int ret = svga_fifo_batch_try_locked(words, at * 4u, &fence);
    if (ret) return ret;
    g->defined = true;
    g->mob_defined = true;
    svga.external_mobs[id].defined = true;
    g->map_pending_pages += count;
    g->map_inflight = true;
    g->progress_fence = fence;
    return 0;
}

/** @brief Construct or progress a borrowed GMR with independent callback-code ownership.
 * @param phys Contiguous page-aligned backing base. @param bytes Backing length.
 * @param owner Required backing identity, transferred only on handle publication.
 * @param owner_put Final backing callback outside the device lock.
 * @param code_owner LIVE callback module or NULL for permanent code.
 * @param out Required in/out native handle, zero on initial creation.
 * @return Zero when ready, EBUSY while progressing, or SVGA error. Task/execution
 * ownership required. Unpublished errors balance the tentative code pin; a
 * published GMR keeps it through final hardware fence and callback return.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_gmr_borrowed_attach(uint64_t phys, uint32_t bytes, void *owner,
                             void (*owner_put)(void *owner), struct module *code_owner,
                             svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    bool retained=!*out;
    if(retained && !try_module_get(code_owner))return SVGA_ENODEV;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) {
        if(retained)module_put(code_owner);
        return SVGA_EBUSY;
    }
    int ret = SVGA_EINVAL;
    struct svga_gmr *g = NULL;
    uint32_t id = 0;
    if (*out) {
        int found = svga_find_gmr(*out);
        if (found < 0) goto done;
        id = (uint32_t)found;
        g = &svga.gmrs[id];
        if (!g->borrowed || g->phys != phys || g->bytes != bytes ||
            g->owner != owner || g->owner_put != owner_put ||
            g->code_owner != code_owner || g->retiring)
            goto done;
    } else {
        ret = SVGA_ENODEV;
        if (!svga.available || !svga.fifo_ready || !svga.gb_active ||
            !(svga.caps & SVGA_CAP_GMR2)) goto done;
        if (!phys || (phys & 4095u) || !bytes || !owner || !owner_put ||
            (phys >> 12) > UINT32_MAX) goto done;
        uint32_t pages = svga_bytes_to_pages(bytes);
        if (!pages || pages > SVGA_MOB_MAX_PAGES ||
            (phys >> 12) > UINT32_MAX - pages || bytes > svga.mob_max_bytes) {
            ret = SVGA_EINVAL;
            goto done;
        }
        for (id = 0; id < svga.gmr_limit && svga.gmrs[id].handle; ++id) {}
        if (id == svga.gmr_limit || id >= SVGA_GMRS) {
            ret = SVGA_ENOMEM;
            goto done;
        }
        if (!svga_gmr_page_budget_allows(pages)) {
            ret = SVGA_ENOMEM;
            goto done;
        }
        svga_handle handle = svga_new_handle(SVGA_KIND_GMR, id);
        if (!handle) { ret = SVGA_ENOMEM; goto done; }
        struct svga_mob mob = {0};
        ret = svga_gb_backing_borrow_locked(&mob, phys, pages, bytes);
        if (ret) goto done;
        ret = svga_mode_lease_acquire_locked();
        if (ret) {
            svga_gb_backing_free_locked(&mob);
            goto done;
        }
        g = &svga.gmrs[id];
        *g = (struct svga_gmr){.handle = handle, .phys = phys, .bytes = bytes,
            .pages = pages, .mobid = svga.external_mob_base + id,
            .owner = owner, .owner_put = owner_put, .code_owner = code_owner, .borrowed = true,
            .mode_lease = true};
        /* Retain the PTable root and ownership bit in the reserved context-sized
         * MOB record. This slot is disjoint from shader and surface IDs. */
        svga.external_mobs[id] = mob;
        *out = handle;
        retained=false;
    }
    ret = SVGA_EBUSY;
    if (g->map_inflight) {
        if (svga_fence_reached_locked(g->progress_fence)) {
            g->mapped_pages += g->map_pending_pages;
            g->map_pending_pages = 0;
            g->map_inflight = false;
        }
    }
    if (g->mapped_pages == g->pages) { ret = 0; goto done; }
    for (uint32_t chunk = 0; chunk < SVGA_GMR_MAPPING_CHUNKS_PER_PASS &&
         g->mapped_pages + g->map_pending_pages < g->pages; ++chunk) {
        ret = submit_mapping_chunk_locked(id, g);
        if (ret) goto done;
    }
    /* A later fence covers every earlier mapping in the same FIFO. Keep the
     * backing owner pinned and the MOB unavailable until that fence passes. */
    ret = SVGA_EBUSY;
done:
    svga_unlock(flags);
    if(retained)module_put(code_owner);
    return ret;
}

/**
 * @brief Resolve a fully mapped borrowed attachment to its backend MOB index.
 * @param handle Original borrowed GMR handle.
 * @param mobid Required writable index, initialized to UINT32_MAX.
 * @return Zero, EBUSY for contention/mapping/retirement progress, or EINVAL for invalid input/handle.
 * Execution-owned task context; try-lock only and no reference transfer. Caller keeps the borrowed backing and callback owner alive until final detach.
 */
int svga_gmr_borrowed_mob_id(svga_handle handle, uint32_t *mobid)
{
    if (!mobid) return SVGA_EINVAL;
    *mobid = UINT32_MAX;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int id = svga_find_gmr(handle), ret = SVGA_EINVAL;
    if (id >= 0) {
        struct svga_gmr *g = &svga.gmrs[id];
        if (g->borrowed && !g->retiring && !g->map_inflight &&
            g->mapped_pages == g->pages && g->mob_defined) {
            *mobid = g->mobid;
            ret = 0;
        } else if (g->borrowed) ret = SVGA_EBUSY;
    }
    svga_unlock(flags);
    return ret;
}

/** @brief Retire hardware backing before invoking and releasing its original callback.
 * @param handle Exact borrowed GMR handle, including partially mapped objects.
 * @return Zero after final callback return, EBUSY while hardware/fences progress,
 * or SVGA error preserving ownership. Task/execution-owned context; callback and
 * final module put run outside the device lock. Code stays pinned through the
 * complete callback, even when that callback drops its own last object reference.
 * Task/execution context for module callers; never enter with the SVGA lock held.
 * Caller retains resources/backing and its independent module call pin through
 * return, and through the real fence or successful teardown when asynchronous.
 */
int svga_gmr_borrowed_detach(svga_handle handle)
{
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int id = svga_find_gmr(handle), ret = SVGA_EINVAL;
    void *owner = NULL;
    void (*owner_put)(void *) = NULL;
    struct module *code_owner = NULL;
    if (id < 0 || !svga.gmrs[id].borrowed) goto done;
    struct svga_gmr *g = &svga.gmrs[id];
    ret = svga_query_prepare_gmr_retire_locked((uint32_t)id);
    if (ret) goto done;
    if (g->retiring) {
        if (!svga_fence_reached_locked(g->retire_fence)) {
            ret = SVGA_EBUSY;
            goto done;
        }
        if (g->mode_lease) {
            ret = svga_mode_lease_release_locked();
            if (ret) goto done;
        }
        owner = g->owner;
        owner_put = g->owner_put;
        code_owner = g->code_owner;
        svga_gb_backing_free_locked(&svga.external_mobs[id]);
        *g = (struct svga_gmr){0};
        ret = 0;
        goto done;
    }
    if (!g->defined && !g->mob_defined && !g->map_inflight) {
        if (g->mode_lease) {
            ret = svga_mode_lease_release_locked();
            if (ret) goto done;
        }
        owner = g->owner;
        owner_put = g->owner_put;
        code_owner = g->code_owner;
        svga_gb_backing_free_locked(&svga.external_mobs[id]);
        *g = (struct svga_gmr){0};
        ret = 0;
        goto done;
    }
    uint32_t words[8] = {SVGA_CMD_DEFINE_GMR2, (uint32_t)id, 0,
                         SVGA_3D_CMD_DESTROY_GB_MOB, 4, g->mobid};
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, 6u * 4u, &fence);
    if (ret) goto done;
    g->retiring = true;
    g->retire_fence = fence;
    ret = SVGA_EBUSY;
done:
    svga_unlock(flags);
    if (!ret && owner_put) {owner_put(owner);module_put(code_owner);}
    return ret;
}

EXPORT_SYMBOL(svga_gmr_borrowed_attach);
EXPORT_SYMBOL(svga_gmr_borrowed_detach);
EXPORT_SYMBOL(svga_gmr_borrowed_mob_id);
