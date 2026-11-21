/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/device_mapping.h>
#include <reliefnt/mm.h>
#include "../../drivers/video/drm/svga/context.h"

struct drm_bo {
    uint32_t refs, pages;
    uint64_t physical;
    struct device_mapping_access access;
    bool busy, private;
};

struct svga_buffer {
    struct drm_bo *bo;
    unsigned refs;
    bool cpu_grabbed;
};

struct svga_device svga;
static unsigned defined, detached, installed, removed, allocations;
static unsigned page_allocations, fail_alloc_at, fail_region_at, waits;
static uint32_t pending, completed;
static unsigned readbacks, uploads, revocations;
static uint32_t live_surfaces, bound_surfaces;
static unsigned readback_ids[8];
static uint32_t mob_pages = 1;
static struct drm_bo *saved_context;
static bool context_live, context_bound;
static bool dx_context, stall;
static struct {
    uint32_t *words;
    uint32_t bytes;
    uint64_t root;
} mobs[16];
static struct {
    uint32_t id, saved_bytes;
    bool bound;
} cotables[SVGA_COTABLE_MAX];
static uint32_t dx_context_mob;
static unsigned cotable_reads, cotable_binds, binding_updates;
static bool constant_bound, rt_bound, shader_bound, so_bound, query_bound;
static uint32_t resource_mob, live_views;
static uint32_t *resource_words;
static unsigned view_restores;
static unsigned fail_dx_submit;

static void submit_dx(const uint32_t *w, uint32_t bytes)
{
    for (unsigned i = 0; i < bytes / sizeof(*w); i += 2 + w[i + 1] / sizeof(*w)) {
        const uint32_t *cmd = w + i;
        assert(i + 2 < bytes / sizeof(*w));
        assert(!(cmd[1] & 3) && cmd[1] / sizeof(*w) <= bytes / sizeof(*w) - i - 2);
        switch (cmd[0]) {
        case SVGA_3D_CMD_DX_DEFINE_CONTEXT:
            assert(cmd[1] == 4 && !context_live && !context_bound);
            context_live = true;
            break;
        case SVGA_3D_CMD_DX_DESTROY_CONTEXT:
            assert(cmd[1] == 4 && context_live);
            memset(cotables, 0, sizeof(cotables));
            context_live = context_bound = constant_bound = rt_bound = false;
            shader_bound = so_bound = query_bound = false;
            live_views = 0;
            break;
        case SVGA_3D_CMD_DX_BIND_CONTEXT:
            assert(cmd[1] == 12 && context_live);
            if (cmd[3] == SVGA3D_INVALID_ID) {
                assert(context_bound);
                context_bound = false;
                break;
            }
            assert(!context_bound && cmd[3] < sizeof(mobs) / sizeof(mobs[0]) && mobs[cmd[3]].words);
            assert(mobs[cmd[3]].words[0] == (cmd[4] ? 0x12345678u : 0));
            context_bound = true;
            dx_context_mob = cmd[3];
            break;
        case SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER:
            assert(cmd[1] == 20 && context_bound && cmd[2] == 0 && cmd[3] == SVGA3D_SHADERTYPE_VS);
            assert(cmd[4] == (constant_bound ? SVGA3D_INVALID_ID : 1));
            assert(cmd[5] == (constant_bound ? 0 : 16) && cmd[6] == (constant_bound ? 0 : 32));
            constant_bound = !constant_bound;
            ++binding_updates;
            break;
        case SVGA_3D_CMD_DX_SET_RENDERTARGETS:
            assert(cmd[1] == 8 && context_bound && cmd[2] == SVGA3D_INVALID_ID);
            assert(cmd[3] == (rt_bound ? SVGA3D_INVALID_ID : 0));
            assert(!rt_bound || (live_views & 1));
            rt_bound = !rt_bound;
            break;
        case SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW:
            assert(cmd[1] == sizeof(SVGA3dCmdDXDefineRenderTargetView) && context_bound);
            assert(cmd[2] < 2 && !(live_views & (1u << cmd[2])));
            assert(cotables[SVGA_COTABLE_RTVIEW].bound && cmd[3] == 1 && cmd[4] == SVGA3D_A8R8G8B8);
            ++view_restores;
            live_views |= 1u << cmd[2];
            break;
        case SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW:
            assert(cmd[1] == 4 && !rt_bound && cotables[SVGA_COTABLE_RTVIEW].bound);
            assert(cmd[2] < 2 && (live_views & (1u << cmd[2])));
            live_views &= ~(1u << cmd[2]);
            break;
        case SVGA_3D_CMD_DX_BIND_SHADER:
            assert(cmd[1] == 16 && cmd[2] == 0 && cmd[3] == 0 && cotables[SVGA_COTABLE_DXSHADER].bound);
            assert(cmd[4] == (shader_bound ? SVGA3D_INVALID_ID : resource_mob));
            assert(cmd[5] == (shader_bound ? 0 : 128));
            shader_bound = !shader_bound;
            break;
        case SVGA_3D_CMD_DX_BIND_STREAMOUTPUT:
            assert(cmd[1] == 16 && cmd[2] == 0 && cmd[5] == 64 && cotables[SVGA_COTABLE_STREAMOUTPUT].bound);
            assert(cmd[3] == (so_bound ? SVGA3D_INVALID_ID : resource_mob));
            assert(cmd[4] == (so_bound ? 0 : 256));
            so_bound = !so_bound;
            break;
        case SVGA_3D_CMD_DX_SET_SHADER:
            assert(cmd[1] == 8 && cmd[3] == SVGA3D_SHADERTYPE_VS && shader_bound);
            break;
        case SVGA_3D_CMD_DX_SET_STREAMOUTPUT:
            assert(cmd[1] == 4 && so_bound);
            break;
        case SVGA_3D_CMD_DX_READBACK_ALL_QUERY:
            assert(cmd[1] == 4 && cmd[2] == 0 && query_bound && cotables[SVGA_COTABLE_DXQUERY].bound);
            resource_words[0] = 0xaabbccddu;
            query_bound = false;
            break;
        case SVGA_3D_CMD_DX_BIND_ALL_QUERY:
            assert(cmd[1] == 8 && cmd[2] == 0 && cmd[3] == resource_mob && !query_bound);
            assert(cotables[SVGA_COTABLE_DXQUERY].bound);
            query_bound = true;
            break;
        case SVGA_3D_CMD_DX_READBACK_CONTEXT:
            assert(cmd[1] == 4 && context_bound && !constant_bound && !live_views);
            assert(!shader_bound && !so_bound && !rt_bound && !query_bound);
            for (unsigned j = 0; j < SVGA_COTABLE_MAX; ++j)
                assert(!cotables[j].bound);
            mobs[dx_context_mob].words[0] = 0x12345678;
            break;
        case SVGA_3D_CMD_DX_READBACK_COTABLE: {
            unsigned type = cmd[3];
            assert(cmd[1] == 8 && context_bound && type < SVGA_COTABLE_MAX && cotables[type].bound);
            unsigned id = cotables[type].id;
            cotables[type].saved_bytes = mobs[id].bytes;
            mobs[id].words[mobs[id].bytes / sizeof(*w) - 1] = type + 1;
            ++cotable_reads;
            break;
        }
        case SVGA_3D_CMD_DX_SET_COTABLE: {
            uint32_t id = cmd[3], type = cmd[4], valid_bytes = cmd[5];
            assert(cmd[1] == 16 && context_bound && type < SVGA_COTABLE_MAX);
            if (id == SVGA3D_INVALID_ID) {
                assert(cotables[type].bound && cotables[type].saved_bytes && !valid_bytes);
                cotables[type].bound = false;
                break;
            }
            assert(id < sizeof(mobs) / sizeof(mobs[0]) && mobs[id].words);
            assert(valid_bytes == cotables[type].saved_bytes && valid_bytes <= mobs[id].bytes);
            if (valid_bytes)
                assert(mobs[id].words[valid_bytes / sizeof(*w) - 1] == type + 1);
            cotables[type].id = id;
            cotables[type].bound = true;
            ++cotable_binds;
            break;
        }
        default: abort();
        }
    }
}

uint32_t svga_read(uint32_t reg)
{
    switch (reg) {
    case SVGA_REG_GMR_MAX_IDS: return 1;
    case SVGA_REG_GMRS_MAX_PAGES: return 1;
    case SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB: return mob_pages * (SVGA_PAGE_SIZE / 1024);
    case SVGA_REG_MOB_MAX_SIZE: return mob_pages * SVGA_PAGE_SIZE;
    case SVGA_REG_DEV_CAP: return dx_context;
    default: abort();
    }
}

void svga_write(uint32_t reg, uint32_t value)
{
    assert(reg == SVGA_REG_DEV_CAP && value == SVGA3D_DEVCAP_DXCONTEXT);
}

void svga_enable_dma(void) { svga.dma_enabled = true; }
bool svga_submit_has_context(void) { return dx_context; }
uint32_t svga_submit_limit(void) { return 4096; }
int svga_submit_idle(void) { return svga_submit_wait(pending); }
int svga_submit_poll(uint32_t sequence) { return sequence <= completed; }
int svga_submit_wait(uint32_t sequence)
{
    assert(sequence <= pending);
    if (stall) {
        svga.fifo.fault = true;
        return -ETIMEDOUT;
    }
    ++waits;
    if (sequence > completed)
        completed = sequence;
    return 0;
}

int svga_submit_commands(const void *commands, uint32_t bytes, uint32_t context,
                         uint32_t *sequence)
{
    uint32_t w[128];
    assert(bytes <= sizeof(w) && !(bytes & 3));
    memcpy(w, commands, bytes);
    if (svga.fifo.fault)
        return -EIO;
    if (context != SVGA3D_INVALID_ID) {
        assert(dx_context && context == 0 && context_bound);
        if (fail_dx_submit && !--fail_dx_submit)
            return -ENOMEM;
        submit_dx(w, bytes);
        if (sequence)
            *sequence = ++pending;
        return 0;
    }
    unsigned tables = dx_context ? SVGA_OTABLE_DXCONTEXT + 1 : SVGA_OTABLE_SCREENTARGET + 1;
    switch (w[0]) {
    case SVGA_CMD_DEFINE_GMR2:
        assert(w[1] == 0 && svga.dma_enabled);
        if (w[2]) {
            assert(w[2] == 1 && bytes == 40 && !sequence);
            assert(w[3] == SVGA_CMD_REMAP_GMR2 && w[4] == w[1] && w[7] == 1);
            ++defined;
        } else {
            assert(bytes == 12 && sequence);
            ++detached;
        }
        break;
    case SVGA_3D_CMD_DEFINE_GB_MOB64: {
        unsigned id = w[2];
        assert(installed > removed && bytes == 28 && !sequence);
        assert(id < sizeof(mobs) / sizeof(mobs[0]) && !mobs[id].words);
        uint64_t root = ((uint64_t)w[5] << 32 | w[4]) << SVGA_PAGE_SHIFT;
        assert(w[3] == SVGA3D_MOBFMT_PT64_0 || w[3] == SVGA3D_MOBFMT_PT64_1);
        uint64_t data = w[3] == SVGA3D_MOBFMT_PT64_0 ? root :
                        (*(PPN64 *)mm_phys_to_virt(root) << SVGA_PAGE_SHIFT);
        mobs[id].words = mm_phys_to_virt(data);
        mobs[id].root = root;
        mobs[id].bytes = w[6];
        if (mobs[id].words == resource_words)
            resource_mob = id;
        ++defined;
        break;
    }
    case SVGA_3D_CMD_DESTROY_GB_MOB: {
        unsigned id = w[2];
        assert(installed == removed + 1 && sequence && bytes == 12);
        assert(id < sizeof(mobs) / sizeof(mobs[0]) && mobs[id].words && !bound_surfaces);
        assert(!context_bound || (dx_context && id != dx_context_mob));
        if (mobs[id].words == resource_words) {
            assert(!shader_bound && !so_bound && !query_bound);
            assert(resource_words[0] == 0xaabbccddu);
        }
        for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i)
            assert(!cotables[i].bound || cotables[i].id != id);
        mobs[id].words = NULL;
        ++detached;
        break;
    }
    case SVGA_3D_CMD_SET_OTABLE_BASE64:
        assert(bytes == 32 * tables && svga.dma_enabled && sequence && installed == removed);
        for (unsigned i = 0; i < tables; ++i) {
            const uint32_t *packet = w + 8 * i;
            assert(packet[0] == w[0] && packet[1] == 24 && packet[2] == i);
            assert(packet[5] && !(packet[5] & (SVGA_PAGE_SIZE - 1)) && !packet[6]);
        }
        ++installed;
        break;
    case SVGA_3D_CMD_SET_OTABLE_BASE:
        assert(bytes == 28 * tables && sequence && installed == removed + 1);
        assert(!live_surfaces && !context_live && pending == completed);
        for (unsigned i = 0; i < tables; ++i) {
            const uint32_t *packet = w + 7 * i;
            assert(packet[0] == w[0] && packet[1] == 20 && packet[2] == tables - i - 1);
            assert(!packet[3] && !packet[4] && !packet[5] && packet[6] == SVGA3D_MOBFMT_INVALID);
        }
        ++removed;
        break;
    case SVGA_3D_CMD_DEFINE_GB_SURFACE:
        assert(installed == removed + 1 && w[2] < 2 && !(live_surfaces & (1u << w[2])));
        live_surfaces |= 1u << w[2];
        break;
    case SVGA_3D_CMD_DESTROY_GB_SURFACE:
        assert(installed == removed + 1 && w[2] < 2 && (live_surfaces & (1u << w[2])));
        live_surfaces &= ~(1u << w[2]);
        bound_surfaces &= ~(1u << w[2]);
        break;
    case SVGA_3D_CMD_READBACK_GB_SURFACE:
    case SVGA_3D_CMD_INVALIDATE_GB_SURFACE:
        assert(w[1] == 4 && w[2] < 2 && (bound_surfaces & (1u << w[2])));
        if (w[0] == SVGA_3D_CMD_READBACK_GB_SURFACE) {
            assert(sequence && readbacks < sizeof(readback_ids) / sizeof(readback_ids[0]));
            readback_ids[readbacks++] = w[2];
            if (bytes == 12)
                break;
        }
        assert(bytes == 28);
        assert(w[3] == SVGA_3D_CMD_BIND_GB_SURFACE && w[4] == 8 && w[5] == w[2]);
        assert(w[6] == SVGA3D_INVALID_ID);
        bound_surfaces &= ~(1u << w[2]);
        break;
    case SVGA_3D_CMD_BIND_GB_SURFACE:
        assert(w[1] == 8 && w[2] < 2 && w[3] == 0);
        assert((live_surfaces & (1u << w[2])) && !(bound_surfaces & (1u << w[2])));
        bound_surfaces |= 1u << w[2];
        if (bytes == 16)
            break;
        assert(bytes == 28 && sequence);
        assert(w[4] == SVGA_3D_CMD_UPDATE_GB_SURFACE && w[5] == 4 && w[6] == w[2]);
        ++uploads;
        break;
    case SVGA_3D_CMD_DEFINE_GB_CONTEXT:
        assert(!context_live && !context_bound && installed == removed + 1);
        assert(bytes == 32 && w[1] == 4 && w[2] == 0);
        assert(w[3] == SVGA_3D_CMD_BIND_GB_CONTEXT && w[4] == 12);
        assert(w[5] == 0 && w[6] == 0 && !w[7]);
        context_live = context_bound = true;
        break;
    case SVGA_3D_CMD_READBACK_GB_CONTEXT:
        assert(context_live && context_bound && saved_context);
        assert(bytes == 32 && w[1] == 4 && w[2] == 0);
        assert(w[3] == SVGA_3D_CMD_BIND_GB_CONTEXT && w[4] == 12);
        assert(w[5] == 0 && w[6] == SVGA3D_INVALID_ID && !w[7]);
        *(uint32_t *)mm_phys_to_virt(saved_context->physical) = 0x12345678;
        context_bound = false;
        break;
    case SVGA_3D_CMD_BIND_GB_CONTEXT:
        assert(context_live && !context_bound && saved_context);
        assert(bytes == 20 && w[1] == 12 && w[2] == 0 && w[3] == 0 && w[4] == 1);
        assert(*(uint32_t *)mm_phys_to_virt(saved_context->physical) == 0x12345678);
        context_bound = true;
        break;
    case SVGA_3D_CMD_DESTROY_GB_CONTEXT:
        assert(context_live && bytes == 12 && w[1] == 4 && w[2] == 0);
        context_live = context_bound = false;
        break;
    case SVGA_3D_CMD_DX_DEFINE_CONTEXT:
    case SVGA_3D_CMD_DX_DESTROY_CONTEXT:
    case SVGA_3D_CMD_DX_BIND_CONTEXT:
    case SVGA_3D_CMD_DX_READBACK_COTABLE:
    case SVGA_3D_CMD_DX_SET_COTABLE:
        assert(dx_context && installed == removed + 1);
        submit_dx(w, bytes);
        break;
    default: abort();
    }
    if (sequence)
        *sequence = ++pending;
    return 0;
}

void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *p) { free(p); }
void *drm_async_alloc(size_t bytes)
{
    if (fail_region_at && !--fail_region_at)
        return NULL;
    ++allocations;
    return malloc(bytes);
}
void drm_async_free(void *p) { assert(allocations--); free(p); }
uint64_t mm_alloc_pages(uint32_t pages) { return mm_alloc_pages_below(pages, UINT64_MAX); }
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{
    (void)mask;
    if (fail_alloc_at && !--fail_alloc_at)
        return 0;
    void *storage = aligned_alloc(SVGA_PAGE_SIZE, (size_t)pages * SVGA_PAGE_SIZE);
    assert(storage);
    memset(storage, 0, (size_t)pages * SVGA_PAGE_SIZE);
    ++page_allocations;
    return (uintptr_t)storage;
}
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
void mm_free_pages(uint64_t physical, uint32_t pages)
{
    assert(pages && page_allocations);
    for (unsigned i = 0; i < sizeof(mobs) / sizeof(mobs[0]); ++i)
        assert(!mobs[i].words ||
               ((uintptr_t)mobs[i].words != physical && mobs[i].root != physical));
    --page_allocations;
    free(mm_phys_to_virt(physical));
}
void drm_bo_get(struct drm_bo *bo) { ++bo->refs; }
void drm_bo_put(struct drm_bo *bo)
{
    if (!bo || --bo->refs)
        return;
    assert(bo->private);
    mm_free_pages(bo->physical, bo->pages);
    free(bo);
}
int drm_bo_alloc(uint64_t bytes, struct drm_bo **out)
{
    *out = NULL;
    uint32_t pages = (bytes + SVGA_PAGE_SIZE - 1) / SVGA_PAGE_SIZE;
    uint64_t physical = mm_alloc_pages(pages);
    if (!physical)
        return -ENOMEM;
    struct drm_bo *bo = malloc(sizeof(*bo));
    assert(bo);
    *bo = (struct drm_bo){.refs = 1, .pages = pages, .physical = physical, .private = true};
    *out = bo;
    return 0;
}
void svga_query_release(const struct svga_context *context) { assert(context->object.references == 1); }
void svga_object_get(struct svga_object *object) { ++object->references; }
void svga_buffer_get(struct svga_buffer *buffer)
{
    if (buffer)
        ++buffer->refs;
}
void svga_object_put(struct svga_object *object) { assert(object->references > 1); --object->references; }
void svga_batch_surface_written(struct svga_surface *surface) { (void)surface; }
void svga_buffer_put(struct svga_buffer *buffer)
{
    if (buffer && !--buffer->refs)
        drm_bo_put(buffer->bo);
}
struct drm_bo *svga_buffer_bo(const struct svga_buffer *buffer) { return buffer->bo; }
bool svga_buffer_cpu_grabbed(const struct svga_buffer *buffer) { return buffer->cpu_grabbed; }
uint64_t drm_bo_size(const struct drm_bo *bo) { return bo ? (uint64_t)bo->pages * SVGA_PAGE_SIZE : 0; }
bool drm_bo_gpu_busy(struct drm_bo *bo) { return bo->busy; }
int svga_buffer_wait(struct drm_bo *bo) { assert(!bo->busy); return 0; }
struct device_mapping_access *drm_bo_mapping_access(struct drm_bo *bo) { return &bo->access; }
int device_mapping_revoke_cpu(struct device_mapping_access *access)
{
    ++revocations;
    if (access->aliases)
        return -EBUSY;
    access->exposed = access->writable = false;
    return 0;
}
uint64_t drm_bo_page_physical(const struct drm_bo *bo, uint64_t offset)
{
    assert(!offset);
    return bo->physical;
}

static void sharing(enum svga_region_type type)
{
    struct drm_bo a = {.refs = 1, .pages = 1, .physical = SVGA_PAGE_SIZE};
    struct drm_bo b = {.refs = 1, .pages = 1, .physical = 2 * SVGA_PAGE_SIZE};
    struct svga_region *region, *shared, *replacement;
    defined = detached = 0;
    assert(svga_region_bind(type, &a, &region) == 0);
    assert(svga_region_bind(type, &a, &shared) == 0);
    assert(shared == region && defined == 1 && a.refs == 2);
    assert(svga_dma_fini() == -EBUSY);
    svga_region_release(shared);
    assert(detached == 0 && svga_region_bind(type, &b, &replacement) == -ENOSPC);
    svga_region_release(region);
    assert(detached == 1 && a.refs == 2 && pending > completed);
    assert(svga_region_bind(type, &b, &replacement) == 0);
    assert(a.refs == 1 && b.refs == 2 && defined == 2 && completed == pending);
    svga_region_release(replacement);
    assert(svga_submit_wait(pending) == 0 && svga_dma_retire(1) == 1);
    assert(allocations == 0 && b.refs == 1);
}

static void bind_surface(struct svga_surface *surface, struct drm_bo *bo, uint32_t id)
{
    const struct {
        SVGA3dCmdHeader header;
        SVGA3dCmdDefineGBSurface body;
    } define = {
        .header = {SVGA_3D_CMD_DEFINE_GB_SURFACE, sizeof(define.body)},
        .body = {.sid = id, .format = SVGA3D_X8R8G8B8, .numMipLevels = 1, .size = {1, 1, 1}},
    };
    assert(!svga_submit(&define, sizeof(define)));
    *surface = (struct svga_surface){
        .object = {.id = id, .published = true}, .gb = true,
        .info.base.drm_surface_flags = drm_vmw_surface_flag_coherent,
    };
    assert(!svga_region_bind(SVGA_REGION_MOB, bo, &surface->backing));
    const uint32_t bind[] = {SVGA_3D_CMD_BIND_GB_SURFACE, 8, id, 0};
    assert(!svga_submit(bind, sizeof(bind)));
    svga_surface_set_bo(surface, bo);
}

static void destroy_surface(struct svga_surface *surface)
{
    const uint32_t destroy[] = {SVGA_3D_CMD_DESTROY_GB_SURFACE, 4, surface->object.id};
    assert(!svga_submit(destroy, sizeof(destroy)));
    svga_region_release(surface->backing);
    svga_surface_set_bo(surface, NULL);
}

static void pressure(void)
{
    /* Force the eviction to remove the predecessor of the new hash entry. */
    enum { BUCKETS = 256 };
    struct drm_bo bos[BUCKETS + 1], *seen[BUCKETS] = {0};
    struct drm_bo *a = NULL, *b = NULL;
    for (unsigned i = 0; i <= BUCKETS; ++i) {
        uintptr_t hash = (uintptr_t)&bos[i] >> 4;
        unsigned bucket = (hash ^ (hash >> 8)) % BUCKETS;
        if (seen[bucket]) {
            a = seen[bucket];
            b = &bos[i];
            break;
        }
        seen[bucket] = &bos[i];
    }
    assert(a && b);
    *a = (struct drm_bo){.refs = 1, .pages = 1, .physical = SVGA_PAGE_SIZE};
    *b = (struct drm_bo){.refs = 1, .pages = 1, .physical = 2 * SVGA_PAGE_SIZE};
    struct svga_surface victim;
    struct svga_region *replacement, *shared;
    bind_surface(&victim, a, 0);
    victim.dirty_serial = 1;
    a->busy = true;
    assert(svga_region_bind(SVGA_REGION_MOB, b, &replacement) == -ENOSPC && victim.backing);
    a->busy = false;
    assert(!svga_region_bind(SVGA_REGION_MOB, a, &shared) && shared == victim.backing);
    assert(svga_region_bind(SVGA_REGION_MOB, b, &replacement) == -ENOSPC && !readbacks);
    svga_region_release(shared);
    assert(!svga_region_bind(SVGA_REGION_MOB, b, &replacement));
    assert(readbacks == 1 && !bound_surfaces && completed == pending);
    assert(!victim.backing && victim.bo == a && victim.object.published && !victim.dirty_serial);
    assert(!svga_region_bind(SVGA_REGION_MOB, b, &shared) && shared == replacement);
    svga_region_release(shared);
    svga_region_release(replacement);
    a->access = (struct device_mapping_access){.write_epoch = 3, .exposed = true, .writable = true};
    assert(!svga_surface_validate_residency(&victim));
    assert(revocations == 1 && !a->access.exposed && victim.uploaded_epoch == 3);
    assert(uploads == 1 && bound_surfaces == 1 && victim.backing && !victim.needs_rebind);
    assert(completed == pending && a->refs == 2 && b->refs == 1);
    assert(!svga_surface_validate_residency(&victim) && uploads == 1);
    destroy_surface(&victim);
    assert(!svga_submit_wait(pending) && svga_dma_retire(1) == 1 && a->refs == 1);
}

static void shared_surface_pressure(void)
{
    struct drm_bo a = {.refs = 1, .pages = 1, .physical = SVGA_PAGE_SIZE};
    struct drm_bo b = {.refs = 1, .pages = 1, .physical = 2 * SVGA_PAGE_SIZE};
    struct svga_surface first, second;
    struct svga_region *replacement;
    bind_surface(&first, &a, 0);
    bind_surface(&second, &a, 1);
    assert(first.backing == second.backing && a.refs == 2);
    first.dirty_serial = 2;
    second.dirty_serial = 1;
    unsigned before = readbacks;
    assert(!svga_region_bind(SVGA_REGION_MOB, &b, &replacement));
    assert(!bound_surfaces && live_surfaces == 3 && a.refs == 1);
    assert(readbacks == before + 2 && readback_ids[before] == 1 && readback_ids[before + 1] == 0);
    assert(first.needs_rebind && second.needs_rebind);
    svga_region_release(replacement);
    assert(!svga_surface_validate_residency(&first));
    assert(!svga_surface_validate_residency(&second));
    assert(first.backing == second.backing && bound_surfaces == 3);
    destroy_surface(&second);
    destroy_surface(&first);
    assert(!svga_submit_wait(pending) && svga_dma_retire(1) == 1 && a.refs == 1 && b.refs == 1);
}

static void gb_context_pressure(void)
{
    struct svga_context context = {.type = SVGA_CONTEXT_GB, .object.references = 1};
    struct svga_region *replacement;
    mob_pages = (sizeof(SVGAGBContextData) + SVGA_PAGE_SIZE - 1) / SVGA_PAGE_SIZE;
    assert(!svga_dma_init(svga_render_reclaim_mobs));
    assert(!svga_context_publish(&context));
    saved_context = context.state_bo;
    const uint32_t bytes = mob_pages * SVGA_PAGE_SIZE;
    context.object.references = 2;
    assert(svga_region_create(SVGA_REGION_MOB, bytes, &replacement) == -ENOSPC && context.state);
    context.object.references = 1;
    assert(!svga_region_create(SVGA_REGION_MOB, bytes, &replacement));
    assert(!context.state && context_live && !context_bound && saved_context->refs == 1);
    svga_region_release(replacement);
    context.object.references = 2;
    assert(!svga_context_validate_residency(&context));
    assert(context.state && context_live && context_bound && context.state_bo == saved_context);
    const uint32_t destroy[] = {SVGA_3D_CMD_DESTROY_GB_CONTEXT, 4, 0};
    assert(!svga_submit(destroy, sizeof(destroy)));
    svga_context_release_backing(&context);
    assert(!svga_dma_fini() && !page_allocations && !allocations);
    saved_context = NULL;
}

static void dx_context_pressure(void)
{
    dx_context = true;
    mob_pages = 16;
    assert(!svga_dma_init(svga_render_reclaim_mobs));
    struct svga_context context = {.type = SVGA_CONTEXT_DX, .object.references = 2};
    assert(!svga_context_publish(&context));
    struct svga_batch batch = {.dx = &context, .serial = 1};
    const unsigned types[] = {SVGA_COTABLE_BLENDSTATE, SVGA_COTABLE_SAMPLER};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        assert(!svga_context_require_table(&batch, types[i], 0));
        assert(!svga_context_stage(&batch, &context, types[i], 0,
                                   (struct svga_context_value){.defined = true}));
    }
    assert(!svga_context_prepare(&batch) && cotable_binds == 2);
    svga_context_finish(&batch, true);

    unsigned before_waits = waits;
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        batch.table_need[types[i]] = 2 * SVGA_PAGE_SIZE;
    assert(!svga_context_prepare(&batch));
    assert(cotable_reads == 2 && cotable_binds == 4 && waits == before_waits + 1);

    struct svga_surface constant = {.object = {.type = SVGA_SURFACE, .id = 1, .references = 1}};
    ++batch.serial;
    assert(!svga_context_stage_binding(&batch, &context, SVGA_BIND_CONSTANT,
        SVGA3D_SHADERTYPE_VS * SVGA3D_DX_MAX_CONSTBUFFERS,
        (struct svga_context_value){.defined = true, .object = &constant.object, .offset = 16, .size = 32}));
    struct svga_buffer buffer = {.refs = 1};
    assert(!drm_bo_alloc(SVGA_PAGE_SIZE, &buffer.bo));
    resource_words = mm_phys_to_virt(buffer.bo->physical);
    struct svga_region *shared;
    assert(!svga_region_bind(SVGA_REGION_MOB, buffer.bo, &shared));
    const unsigned resource_tables[] = {SVGA_COTABLE_DXSHADER, SVGA_COTABLE_STREAMOUTPUT};
    for (unsigned i = 0; i < 2; ++i) {
        unsigned table = resource_tables[i];
        struct svga_context_value value = {.defined = true, .buffer = &buffer, .mob = shared,
                                           .offset = 128 * (i + 1), .size = 64};
        assert(!svga_context_require_table(&batch, table, 0));
        assert(!svga_context_stage(&batch, &context, table, 0, value));
        value.type = table;
        value.mob = NULL;
        assert(!svga_context_stage_binding(&batch, &context, i ? SVGA_BIND_DX_SO : SVGA_BIND_DX_SHADER,
                                           i ? 0 : SVGA3D_SHADERTYPE_VS, value));
    }
    assert(!svga_context_require_table(&batch, SVGA_COTABLE_DXQUERY, 0));
    assert(!svga_context_stage_binding(&batch, &context, SVGA_DX_QUERY_MOB, 0,
        (struct svga_context_value){.defined = true, .buffer = &buffer, .mob = shared}));
    SVGA3dCmdDXDefineRenderTargetView body = {.sid = 1, .format = SVGA3D_A8R8G8B8};
    const struct svga_packet packet = {.command = SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
                                       .bytes = sizeof(body), .body = &body};
    assert(!svga_context_stage_view(&batch, SVGA_COTABLE_RTVIEW, &packet, &constant));
    body.renderTargetViewId = 1;
    assert(!svga_context_stage_view(&batch, SVGA_COTABLE_RTVIEW, &packet, &constant));
    assert(!svga_context_stage_binding(&batch, &context, SVGA_BIND_RT, 0,
        (struct svga_context_value){.defined = true, .object = &constant.object, .type = SVGA_COTABLE_RTVIEW}));
    assert(!svga_context_prepare(&batch) && cotable_binds == 8);
    svga_context_finish(&batch, true);
    assert(svga_region_users(shared) == 4);
    live_views = 3;
    constant_bound = rt_bound = shader_bound = so_bound = query_bound = true;
    struct svga_region *pressure;
    uint32_t pressure_bytes = mob_pages * SVGA_PAGE_SIZE;
    context.object.references = 1;
    buffer.cpu_grabbed = true;
    assert(svga_region_create(SVGA_REGION_MOB, pressure_bytes, &pressure) == -ENOSPC);
    buffer.cpu_grabbed = false;
    fail_dx_submit = 2;
    assert(svga_region_create(SVGA_REGION_MOB, pressure_bytes, &pressure) == -ENOMEM);
    assert(context_bound && !constant_bound && live_views == 3 && !query_bound);
    assert(svga_region_create(SVGA_REGION_MOB, pressure_bytes, &pressure) == -ENOSPC);
    assert(!context.state && context_live && !context_bound && cotable_reads == 8 && binding_updates == 1);
    assert(svga_region_users(shared) == 1 && mobs[resource_mob].words == resource_words);
    assert(svga_region_release(shared) == 1);
    assert(!svga_region_create(SVGA_REGION_MOB, pressure_bytes, &pressure));
    svga_region_release(pressure);
    context.object.references = 2;
    fail_region_at = 3;
    assert(svga_context_validate_residency(&context) == -ENOMEM);
    assert(!context.state && context_live && !context_bound && cotable_binds == 8);
    fail_dx_submit = 2;
    assert(svga_context_validate_residency(&context) == -ENOMEM);
    assert(context.state && context_bound && cotable_binds == 14 && !constant_bound);
    assert(live_views == 1 && shader_bound && so_bound && view_restores == 1);
    assert(!svga_context_validate_residency(&context));
    assert(constant_bound && rt_bound && query_bound && binding_updates == 2 && cotable_binds == 14);
    assert(resource_words[0] == 0xaabbccddu);
    ++batch.serial;
    assert(svga_context_view_scrubbed(&batch, SVGA_COTABLE_RTVIEW, 1));
    assert(!svga_context_replay_resource(&batch, SVGA_COTABLE_RTVIEW, 1));
    assert(!svga_context_view_scrubbed(&batch, SVGA_COTABLE_RTVIEW, 1));
    assert(live_views == 3 && view_restores == 2);
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        assert(svga_context_lookup(&context, types[i], 0));

    const uint32_t destroy[] = {SVGA_3D_CMD_DX_DESTROY_CONTEXT, 4, 0};
    assert(!svga_submit(destroy, sizeof(destroy)));
    svga_context_release_backing(&context);
    assert(buffer.refs == 1);
    svga_buffer_put(&buffer);
    assert(!svga_dma_fini() && !page_allocations && !allocations && constant.object.references == 1);
    resource_words = NULL;
}

int main(void)
{
    svga.caps = SVGA_CAP_GMR2 | SVGA_CAP_GBOBJECTS | SVGA_CAP_DX;
    fail_alloc_at = 2;
    assert(svga_dma_init(svga_render_reclaim_mobs) == -ENOMEM && !svga_dma_supported(SVGA_REGION_MOB));
    assert(!page_allocations && !installed && !allocations && !svga_dma_fini());
    assert(!svga_dma_init(svga_render_reclaim_mobs) && svga_dma_supported(SVGA_REGION_MOB));
    assert(installed == 1 && !removed && page_allocations && !allocations);
    svga.ready = true;
    sharing(SVGA_REGION_GMR);
    sharing(SVGA_REGION_MOB);
    pressure();
    shared_surface_pressure();
    assert(!svga_dma_fini() && installed == 1 && removed == 1 && !page_allocations);
    gb_context_pressure();
    dx_context_pressure();

    svga.ready = false;
    assert(!svga_dma_init(NULL) && svga_dma_supported(SVGA_REGION_MOB));
    unsigned retained = page_allocations;
    stall = true;
    assert(svga_dma_fini() == -ETIMEDOUT && !svga_dma_supported(SVGA_REGION_MOB));
    assert(page_allocations == retained && pending > completed && svga_dma_init(NULL) == -EBUSY);
    assert(svga_dma_fini() == -EIO && page_allocations == retained && !allocations);
    puts("PASS SVGA DMA: shared regions, GB surface and context residency, DX tables and binding replay, stop failure");
    return 0;
}
