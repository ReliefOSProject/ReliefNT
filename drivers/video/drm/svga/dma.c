/* SPDX-License-Identifier: Apache-2.0 */
#include "dma.h"
#include "submit.h"
#include "../memory.h"
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>

/* The GMR2 remap chunk and OTable populations follow vmwgfx_gmr.c and
 * vmwgfx_mob.c. ReliefNT BOs already retain a contiguous DMA extent. */
#define GMR2_REMAP_PAGES (31u * 1024 / sizeof(PPN64))
#define SVGA_MAX_GMR_IDS 65536u
#define SVGA_GMR1_BYTE_MASK ((UINT64_C(1) << 44) - 1)
#define SVGA_GMR1_MAX_PAGES (256u * 1024 * 1024 / SVGA_PAGE_SIZE)
#define SVGA_GB_CONTEXTS 256u
#define SVGA_GB_SURFACES 32768u
#define SVGA_GB_SHADERS 20000u
#define SVGA_GB_SCREENS 8u
#define SVGA_DX_CONTEXTS 256u
#define SVGA_MAX_MOBS (SVGA_GB_CONTEXTS + SVGA_GB_SURFACES + SVGA_GB_SHADERS + SVGA_GB_SCREENS)
#define SVGA_PT_ENTRIES (SVGA_PAGE_SIZE / sizeof(PPN64))
#define SVGA_REGION_BUCKETS 256u
#define SVGA_WORD_BITS 64u
#define SVGA_BITMAP_WORDS(n) (((n) + SVGA_WORD_BITS - 1) / SVGA_WORD_BITS)

struct svga_page_table {
    uint64_t allocation;
    uint32_t pages;
    SVGAMobFormat depth;
    PPN64 root;
};

struct svga_region {
    struct svga_region *next;
    struct drm_bo *bo;
    uint64_t physical, descriptor;
    struct svga_page_table pt;
    enum svga_region_type type;
    uint32_t id, pages, fence, users;
    bool published;
};

struct svga_otables {
    uint32_t count, pages;
    uint64_t physical;
    struct svga_page_table pt[SVGA_OTABLE_DXCONTEXT + 1];
};

struct svga_otable_cmd {
    SVGA3dCmdHeader header;
    SVGA3dCmdSetOTableBase64 body;
};

struct svga_otable_clear_cmd {
    SVGA3dCmdHeader header;
    SVGA3dCmdSetOTableBase body;
};

static struct {
    struct svga_region *bound[SVGA_REGION_BUCKETS];
    struct svga_region *retired_head, *retired_tail;
    struct svga_region *quarantined;
    struct svga_otables tables;
    uint64_t gmr_ids[SVGA_BITMAP_WORDS(SVGA_MAX_GMR_IDS)];
    uint64_t mob_ids[SVGA_BITMAP_WORDS(SVGA_MAX_MOBS)];
    uint32_t gmr_limit, gmr_budget, gmr_used;
    uint32_t mob_max_size, mob_budget, mob_used;
    int (*reclaim_mobs)(uint32_t pages);
    bool gmr2;
} dma;

_Static_assert(sizeof(struct svga_otable_cmd) == 32, "SVGA OTable packet layout");
_Static_assert(sizeof(struct svga_otable_clear_cmd) == 28, "SVGA OTable clear packet layout");
_Static_assert(sizeof(SVGA3dCmdDefineGBMob64) == 20, "SVGA MOB packet layout");

static uint32_t region_id_alloc(uint64_t *bitmap, uint32_t limit)
{
    /* Use the lowest free ID. A working set within the first bitmap words
     * does not touch the rest of the namespace on each create. */
    for (uint32_t i = 0; i < SVGA_BITMAP_WORDS(limit); ++i) {
        uint64_t available = ~bitmap[i];
        if (!available)
            continue;
        unsigned bit = __builtin_ctzll(available);
        uint32_t id = i * SVGA_WORD_BITS + bit;
        if (id >= limit)
            break;
        bitmap[i] |= UINT64_C(1) << bit;
        return id;
    }
    return SVGA3D_INVALID_ID;
}

static void region_id_free(uint64_t *bitmap, uint32_t id)
{
    bitmap[id / SVGA_WORD_BITS] &= ~(UINT64_C(1) << (id % SVGA_WORD_BITS));
}

static struct svga_region **region_bucket(enum svga_region_type type, const struct drm_bo *bo)
{
    uintptr_t hash = (uintptr_t)bo >> 4;
    return &dma.bound[(hash ^ (hash >> 8) ^ type) % SVGA_REGION_BUCKETS];
}

static struct svga_region **region_link(enum svga_region_type type, const struct drm_bo *bo)
{
    struct svga_region **link = region_bucket(type, bo);
    while (*link && ((*link)->bo != bo || (*link)->type != type))
        link = &(*link)->next;
    return link;
}

static void pt_clear(struct svga_page_table *pt)
{
    if (pt->allocation)
        mm_free_pages(pt->allocation, pt->pages);
}

/* Lay out leaves first and the root last, with one 64-bit PPN per entry.
 * vmwgfx_mob_pt_setup uses this hierarchy for noncontiguous BOs as well. */
static int pt_fill(struct svga_page_table *pt, uint64_t phys, uint32_t pages)
{
    if (!pages || pages > SVGA_PT_ENTRIES * SVGA_PT_ENTRIES ||
        !phys || (phys & (SVGA_PAGE_SIZE - 1)))
        return -EINVAL;
    *pt = (struct svga_page_table){
        .root = phys >> SVGA_PAGE_SHIFT, .depth = SVGA3D_MOBFMT_PT64_0,
    };
    if (pages == 1)
        return 0;
    uint32_t leaves = (pages + SVGA_PT_ENTRIES - 1) / SVGA_PT_ENTRIES;
    uint32_t total = leaves + (leaves > 1);
    uint64_t table = mm_alloc_pages(total);
    if (!table)
        return -ENOMEM;
    PPN64 *entries = mm_phys_to_virt(table);
    if (!entries) {
        mm_free_pages(table, total);
        return -ENOMEM;
    }
    __builtin_memset(entries, 0, (size_t)total * SVGA_PAGE_SIZE);
    for (uint32_t i = 0; i < pages; ++i)
        entries[i] = (phys >> SVGA_PAGE_SHIFT) + i;
    if (leaves > 1) {
        PPN64 *root = entries + leaves * SVGA_PT_ENTRIES;
        for (uint32_t i = 0; i < leaves; ++i)
            root[i] = (table >> SVGA_PAGE_SHIFT) + i;
    }
    *pt = (struct svga_page_table){
        .allocation = table, .pages = total,
        .root = (table >> SVGA_PAGE_SHIFT) + total - 1,
        .depth = leaves > 1 ? SVGA3D_MOBFMT_PT64_2 : SVGA3D_MOBFMT_PT64_1,
    };
    return 0;
}

static void otable_free(struct svga_otables *tables)
{
    for (uint32_t i = 0; i < tables->count; ++i)
        pt_clear(&tables->pt[i]);
    if (tables->physical)
        mm_free_pages(tables->physical, tables->pages);
    *tables = (struct svga_otables){0};
}

/* Like vmwgfx_mob.c, install global tables before any GB object can exist.
 * Device ownership covers the tables from probe through transport shutdown. */
static int otables_setup(void)
{
    static const uint32_t bytes[] = {
        [SVGA_OTABLE_MOB] = SVGA_MAX_MOBS * sizeof(SVGAOTableMobEntry),
        [SVGA_OTABLE_SURFACE] = SVGA_GB_SURFACES * sizeof(SVGAOTableSurfaceEntry),
        [SVGA_OTABLE_CONTEXT] = SVGA_GB_CONTEXTS * sizeof(SVGAOTableContextEntry),
        [SVGA_OTABLE_SHADER] = SVGA_GB_SHADERS * sizeof(SVGAOTableShaderEntry),
        [SVGA_OTABLE_SCREENTARGET] = SVGA_GB_SCREENS * sizeof(SVGAOTableScreenTargetEntry),
        [SVGA_OTABLE_DXCONTEXT] = SVGA_DX_CONTEXTS * sizeof(SVGAOTableDXContextEntry),
    };
    struct svga_otables *tables = &dma.tables;
    tables->count = SVGA_OTABLE_SCREENTARGET + 1;
    if ((svga.caps & SVGA_CAP_DX) && svga_submit_has_context()) {
        svga_write(SVGA_REG_DEV_CAP, SVGA3D_DEVCAP_DXCONTEXT);
        if (svga_read(SVGA_REG_DEV_CAP))
            tables->count = SVGA_OTABLE_DXCONTEXT + 1;
    }
    uint32_t pages[SVGA_OTABLE_DXCONTEXT + 1];
    for (uint32_t i = 0; i < tables->count; ++i) {
        pages[i] = (bytes[i] + SVGA_PAGE_SIZE - 1) >> SVGA_PAGE_SHIFT;
        tables->pages += pages[i];
    }
    int ret = -ENOMEM;
    tables->physical = mm_alloc_pages(tables->pages);
    if (!tables->physical)
        goto fail;
    /* The allocator zeroes all pages. Set validSizeInBytes to zero as Linux
     * does for new tables; the device initializes entries before first use. */
    struct svga_otable_cmd packets[SVGA_OTABLE_DXCONTEXT + 1];
    uint64_t at = tables->physical;
    for (uint32_t i = 0; i < tables->count; ++i) {
        ret = pt_fill(&tables->pt[i], at, pages[i]);
        if (ret)
            goto fail;
        if (tables->pt[i].depth == SVGA3D_MOBFMT_PT64_2) {
            ret = -E2BIG;
            goto fail;
        }
        packets[i] = (struct svga_otable_cmd){
            .header = {SVGA_3D_CMD_SET_OTABLE_BASE64, sizeof(packets[i].body)},
            .body = {.type = i, .baseAddress = tables->pt[i].root,
                     .sizeInBytes = pages[i] * SVGA_PAGE_SIZE, .ptDepth = tables->pt[i].depth},
        };
        at += (uint64_t)pages[i] * SVGA_PAGE_SIZE;
    }
    svga_enable_dma();
    uint32_t fence;
    ret = svga_submit_fenced(packets, tables->count * sizeof(*packets), &fence);
    if (ret)
        goto fail;
    /* After publication, even a failed wait must leave teardown in charge. */
    ret = svga_submit_wait(fence);
    if (ret)
        svga.fifo.fault = true;
    return ret;

fail:
    otable_free(tables);
    return ret;
}

int svga_dma_fini(void)
{
    if (dma.retired_tail) {
        int ret = svga_submit_wait(dma.retired_tail->fence);
        if (ret)
            return ret;
        (void)svga_dma_retire(UINT32_MAX);
    }
    if (dma.gmr_used || dma.mob_used)
        return -EBUSY;

    struct svga_otables *tables = &dma.tables;
    if (!tables->count)
        return 0;
    struct svga_otable_clear_cmd packets[SVGA_OTABLE_DXCONTEXT + 1];
    for (uint32_t i = 0; i < tables->count; ++i) {
        packets[i] = (struct svga_otable_clear_cmd){
            .header = {SVGA_3D_CMD_SET_OTABLE_BASE, sizeof(packets[i].body)},
            .body = {.type = tables->count - i - 1, .ptDepth = SVGA3D_MOBFMT_INVALID},
        };
    }
    uint32_t fence;
    int ret = svga_submit_fenced(packets, tables->count * sizeof(*packets), &fence);
    if (!ret)
        ret = svga_submit_wait(fence);
    if (!ret)
        otable_free(tables);
    return ret;
}

int svga_dma_init(int (*reclaim_mobs)(uint32_t pages))
{
    if (dma.tables.count || dma.gmr_used || dma.mob_used)
        return -EBUSY;
    __builtin_memset(&dma, 0, sizeof(dma));
    dma.reclaim_mobs = reclaim_mobs;
    if (svga.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2)) {
        uint32_t ids = svga_read(SVGA_REG_GMR_MAX_IDS);
        dma.gmr_limit = ids < SVGA_MAX_GMR_IDS ? ids : SVGA_MAX_GMR_IDS;
        dma.gmr2 = (svga.caps & SVGA_CAP_GMR2) != 0;
        if (dma.gmr2)
            dma.gmr_budget = svga_read(SVGA_REG_GMRS_MAX_PAGES);
        else if (svga_read(SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH) >= 2)
            dma.gmr_budget = SVGA_GMR1_MAX_PAGES;
    }
    if (svga.caps & SVGA_CAP_GBOBJECTS) {
        uint32_t size_reg = svga.caps2 & SVGA_CAP2_GB_MEMSIZE_2 ?
            SVGA_REG_GBOBJECT_MEM_SIZE_KB : SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB;
        dma.mob_budget = svga_read(size_reg) / 4;
        dma.mob_max_size = svga_read(SVGA_REG_MOB_MAX_SIZE);
        if (dma.mob_budget && dma.mob_max_size >= SVGA_PAGE_SIZE)
            return otables_setup();
    }
    return 0;
}

bool svga_dma_supported(enum svga_region_type type)
{
    if (type == SVGA_REGION_GMR)
        return dma.gmr_limit && dma.gmr_budget;
    return dma.tables.count && !svga.fifo.fault;
}

uint32_t svga_dma_max_mob_size(void)
{
    return dma.mob_max_size;
}

uint64_t svga_dma_mob_bytes(void)
{
    return (uint64_t)dma.mob_budget << SVGA_PAGE_SHIFT;
}

static int gmr1_publish(struct svga_region *region)
{
    if ((region->physical >> SVGA_PAGE_SHIFT) + region->pages - 1 > UINT32_MAX)
        return -ERANGE;
    region->descriptor = mm_alloc_pages_below(1, SVGA_GMR1_BYTE_MASK);
    if (!region->descriptor)
        return -ENOMEM;
    SVGAGuestMemDescriptor *descriptors = mm_phys_to_virt(region->descriptor);
    if (!descriptors)
        return -ENOMEM;
    descriptors[0] = (SVGAGuestMemDescriptor){
        .ppn = region->physical >> SVGA_PAGE_SHIFT, .numPages = region->pages,
    };
    descriptors[1] = (SVGAGuestMemDescriptor){0};
    int ret = svga_submit_idle();
    if (ret)
        return ret;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    svga_write(SVGA_REG_GMR_ID, region->id);
    svga_write(SVGA_REG_GMR_DESCRIPTOR, region->descriptor >> SVGA_PAGE_SHIFT);
    region->published = true;
    return 0;
}

static int gmr2_publish(struct svga_region *region)
{
    struct gmr2_packet {
        uint32_t define;
        SVGAFifoCmdDefineGMR2 gmr;
        uint32_t remap;
        SVGAFifoCmdRemapGMR2 map;
    };
    uint32_t max = svga_submit_limit();
    if (max < sizeof(struct gmr2_packet) + sizeof(PPN64))
        return -E2BIG;
    uint32_t batch = (max - sizeof(struct gmr2_packet)) / sizeof(PPN64);
    if (batch > GMR2_REMAP_PAGES)
        batch = GMR2_REMAP_PAGES;
    if (batch > region->pages)
        batch = region->pages;
    struct gmr2_packet *packet = kernel_malloc(sizeof(*packet) + batch * sizeof(PPN64));
    if (!packet)
        return -ENOMEM;
    *packet = (struct gmr2_packet){
        .define = SVGA_CMD_DEFINE_GMR2, .gmr = {region->id, region->pages},
        .remap = SVGA_CMD_REMAP_GMR2,
        .map = {.gmrId = region->id, .flags = SVGA_REMAP_GMR2_PPN64},
    };
    uint8_t *ppns = (void *)(packet + 1);
    int ret = 0;
    for (uint32_t offset = 0; offset < region->pages;) {
        unsigned count = region->pages - offset;
        if (count > batch)
            count = batch;
        packet->map.offsetPages = offset;
        packet->map.numPages = count;
        for (unsigned i = 0; i < count; ++i) {
            PPN64 ppn = (region->physical >> SVGA_PAGE_SHIFT) + offset + i;
            __builtin_memcpy(ppns + i * sizeof(ppn), &ppn, sizeof(ppn));
        }
        unsigned skip = offset ? offsetof(struct gmr2_packet, remap) : 0;
        ret = svga_submit((uint8_t *)packet + skip,
                               sizeof(*packet) - skip + count * sizeof(PPN64));
        if (ret)
            break;
        region->published = true;
        offset += count;
    }
    kernel_free(packet);
    return ret;
}

static int mob_publish(struct svga_region *region)
{
    int ret = pt_fill(&region->pt, region->physical, region->pages);
    if (ret)
        return ret;
    const struct {
        SVGA3dCmdHeader header;
        SVGA3dCmdDefineGBMob64 body;
    } cmd = {
        .header = {SVGA_3D_CMD_DEFINE_GB_MOB64, sizeof(SVGA3dCmdDefineGBMob64)},
        .body = {region->id, region->pt.depth, region->pt.root, region->pages * SVGA_PAGE_SIZE},
    };
    ret = svga_submit(&cmd, sizeof(cmd));
    if (!ret)
        region->published = true;
    return ret;
}

static void region_free(struct svga_region *region)
{
    if (region->type == SVGA_REGION_GMR) {
        dma.gmr_used -= region->pages;
        region_id_free(dma.gmr_ids, region->id);
    } else {
        dma.mob_used -= region->pages;
        region_id_free(dma.mob_ids, region->id);
    }
    pt_clear(&region->pt);
    if (region->descriptor)
        mm_free_pages(region->descriptor, 1);
    if (region->bo)
        drm_bo_put(region->bo);
    else if (region->physical)
        mm_free_pages(region->physical, region->pages);
    drm_async_free(region);
}

/* Wait once for enough detaches to satisfy the allocation. Only ask the
 * GB resource layer for victims when the current retirement queue falls short. */
static int reclaim_regions(enum svga_region_type type, uint32_t need)
{
    bool can_reclaim = type == SVGA_REGION_MOB && dma.reclaim_mobs;
    for (;;) {
        uint32_t pages = 0;
        for (const struct svga_region *r = dma.retired_head; r; r = r->next) {
            if (r->type != type)
                continue;
            pages += r->pages;
            if (pages >= need) {
                int ret = svga_submit_wait(r->fence);
                if (!ret)
                    (void)svga_dma_retire(UINT32_MAX);
                return ret;
            }
        }
        if (!can_reclaim)
            return -ENOSPC;
        uint32_t used = dma.mob_used;
        int ret = dma.reclaim_mobs(need - pages);
        if (ret)
            return ret;
        uint32_t freed = used - dma.mob_used;
        if (freed >= need)
            return 0;
        need -= freed;
        can_reclaim = false;
    }
}

static int bind_region(enum svga_region_type type, struct drm_bo *bo,
                       uint32_t bytes, struct svga_region **out)
{
    *out = NULL;
    if (!svga.ready || svga.fifo.fault)
        return -EIO;
    if (!svga_dma_supported(type))
        return -EOPNOTSUPP;
    if (!bytes || bytes > UINT32_MAX - (SVGA_PAGE_SIZE - 1))
        return -EINVAL;
    uint32_t pages = (bytes + SVGA_PAGE_SIZE - 1) >> SVGA_PAGE_SHIFT;
    if (type == SVGA_REGION_MOB && pages > (dma.mob_max_size >> SVGA_PAGE_SHIFT))
        return -E2BIG;
    struct svga_region *existing = bo ? *region_link(type, bo) : NULL;
    if (existing) {
        int ret = svga_region_get(existing);
        if (ret)
            return ret;
        *out = existing;
        return 0;
    }
    (void)svga_dma_retire(32);
    if (svga.fifo.fault)
        return -EIO;
    uint64_t *ids = type == SVGA_REGION_GMR ? dma.gmr_ids : dma.mob_ids;
    uint32_t *used = type == SVGA_REGION_GMR ? &dma.gmr_used : &dma.mob_used;
    uint32_t budget = type == SVGA_REGION_GMR ? dma.gmr_budget : dma.mob_budget;
    uint32_t limit = type == SVGA_REGION_GMR ? dma.gmr_limit : SVGA_MAX_MOBS;
    if (pages > budget)
        return -ENOSPC;
    if (pages > budget - *used) {
        int ret = reclaim_regions(type, pages - (budget - *used));
        if (ret)
            return ret;
        if (pages > budget - *used)
            return -ENOSPC;
    }
    if (svga.fifo.fault)
        return -EIO;
    uint32_t id = region_id_alloc(ids, limit);
    if (id == SVGA3D_INVALID_ID) {
        int ret = reclaim_regions(type, 1);
        if (ret)
            return ret;
        if (svga.fifo.fault)
            return -EIO;
        id = region_id_alloc(ids, limit);
        if (id == SVGA3D_INVALID_ID)
            return -ENOSPC;
    }
    struct svga_region *region = drm_async_alloc(sizeof(*region));
    if (!region) {
        region_id_free(ids, id);
        return -ENOMEM;
    }
    *used += pages;
    *region = (struct svga_region){.type = type, .pages = pages, .id = id, .bo = bo, .users = 1};
    if (bo) {
        drm_bo_get(bo);
        region->physical = drm_bo_page_physical(bo, 0);
    } else if (type == SVGA_REGION_GMR && !dma.gmr2) {
        region->physical = mm_alloc_pages_below(pages, SVGA_GMR1_BYTE_MASK);
    } else {
        region->physical = mm_alloc_pages(pages);
    }
    if (!region->physical) {
        region_free(region);
        return -ENOMEM;
    }
    svga_enable_dma();
    int ret;
    if (type == SVGA_REGION_MOB)
        ret = mob_publish(region);
    else if (dma.gmr2)
        ret = gmr2_publish(region);
    else
        ret = gmr1_publish(region);
    if (ret) {
        svga_region_release(region);
        return ret;
    }
    if (bo) {
        /* Reclaim may have removed any element of this bucket. */
        struct svga_region **head = region_bucket(type, bo);
        region->next = *head;
        *head = region;
    }
    *out = region;
    return 0;
}

int svga_region_bind(enum svga_region_type type, struct drm_bo *bo,
                     struct svga_region **out)
{
    uint64_t size = drm_bo_size(bo);
    return size <= UINT32_MAX ? bind_region(type, bo, size, out) : -E2BIG;
}

int svga_region_create(enum svga_region_type type, uint32_t bytes,
                       struct svga_region **out)
{
    return bind_region(type, NULL, bytes, out);
}

int svga_region_get(struct svga_region *region)
{
    if (region->users == UINT32_MAX)
        return -EOVERFLOW;
    ++region->users;
    return 0;
}

uint32_t svga_region_id(const struct svga_region *region)
{
    return region->id;
}

void *svga_region_pixels(const struct svga_region *region)
{
    return mm_phys_to_virt(region->physical);
}

struct drm_bo *svga_region_bo(const struct svga_region *region)
{
    return region ? region->bo : NULL;
}

uint32_t svga_region_users(const struct svga_region *region)
{
    return region ? region->users : 0;
}

static int region_detach(struct svga_region *region)
{
    if (region->type == SVGA_REGION_MOB) {
        const struct {
            SVGA3dCmdHeader header;
            SVGA3dCmdDestroyGBMob mob;
        } command = {
            .header = {SVGA_3D_CMD_DESTROY_GB_MOB, sizeof(SVGA3dCmdDestroyGBMob)},
            .mob = {.mobid = region->id},
        };
        return svga_submit_fenced(&command, sizeof(command), &region->fence);
    }
    if (dma.gmr2) {
        const uint32_t command[] = {SVGA_CMD_DEFINE_GMR2, region->id, 0};
        return svga_submit_fenced(command, sizeof(command), &region->fence);
    }
    /* GMR1 register writes are outside the FIFO ordering domain. Drain the
     * queue before undefining the ID and accepting a replacement descriptor. */
    int ret = svga_submit_idle();
    if (!ret) {
        svga_write(SVGA_REG_GMR_ID, region->id);
        svga_write(SVGA_REG_GMR_DESCRIPTOR, 0);
    }
    return ret;
}

uint32_t svga_region_release(struct svga_region *region)
{
    if (!region || --region->users)
        return 0;
    uint32_t pages = region->pages;
    if (!region->published) {
        region_free(region);
        return pages;
    }
    if (region->bo) {
        struct svga_region **link = region_link(region->type, region->bo);
        if (*link == region)
            *link = region->next;
    }
    if (region_detach(region)) {
        svga.fifo.fault = true;
        region->next = dma.quarantined;
        dma.quarantined = region;
        return 0;
    }
    if (region->type == SVGA_REGION_GMR && !dma.gmr2) {
        region_free(region);
        return pages;
    }
    region->next = NULL;
    if (dma.retired_tail)
        dma.retired_tail->next = region;
    else
        dma.retired_head = region;
    dma.retired_tail = region;
    (void)svga_dma_retire(8);
    return pages;
}

uint32_t svga_dma_retire(uint32_t budget)
{
    uint32_t completed = 0;
    while (dma.retired_head && completed < budget) {
        struct svga_region *region = dma.retired_head;
        if (svga_submit_poll(region->fence) != 1)
            break;
        dma.retired_head = region->next;
        if (!dma.retired_head)
            dma.retired_tail = NULL;
        region_free(region);
        ++completed;
    }
    return completed;
}
