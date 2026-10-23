#include "device.h"

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

int svga_gmr_borrowed_attach(uint64_t phys, uint32_t bytes, void *owner,
                             void (*owner_put)(void *owner), svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = SVGA_EINVAL;
    struct svga_gmr *g = NULL;
    uint32_t id = 0;
    if (*out) {
        int found = svga_find_gmr(*out);
        if (found < 0) goto done;
        id = (uint32_t)found;
        g = &svga.gmrs[id];
        if (!g->borrowed || g->phys != phys || g->bytes != bytes ||
            g->owner != owner || g->owner_put != owner_put || g->retiring)
            goto done;
    } else {
        ret = SVGA_ENODEV;
        if (!svga.available || !svga.fifo_ready || !svga.gb_active ||
            !(svga.caps & SVGA_CAP_GMR2)) goto done;
        if (!phys || (phys & 4095u) || !bytes || !owner || !owner_put ||
            (phys >> 12) > UINT32_MAX) goto done;
        uint32_t pages = (bytes + 4095u) / 4096u;
        if (!pages || pages > SVGA_MAX_GUEST_PAGES ||
            (phys >> 12) > UINT32_MAX - pages || bytes > svga.mob_max_bytes) {
            ret = SVGA_EINVAL;
            goto done;
        }
        for (id = 0; id < svga.gmr_limit && svga.gmrs[id].handle; ++id) {}
        if (id == svga.gmr_limit || id >= SVGA_GMRS) {
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
            .pages = pages, .mobid = SVGA_EXTERNAL_MOB_BASE + id,
            .owner = owner, .owner_put = owner_put, .borrowed = true,
            .mode_lease = true};
        /* Retain the PTable root and ownership bit in the reserved context-sized
         * MOB record. This slot is disjoint from shader and surface IDs. */
        svga.external_mobs[id] = mob;
        *out = handle;
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
    return ret;
}

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

int svga_gmr_borrowed_detach(svga_handle handle)
{
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int id = svga_find_gmr(handle), ret = SVGA_EINVAL;
    void *owner = NULL;
    void (*owner_put)(void *) = NULL;
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
    if (!ret && owner_put) owner_put(owner);
    return ret;
}
