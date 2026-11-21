/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_DMA_H
#define RELIEFNT_SVGA_DMA_H

#include "internal.h"
#include "protocol/svga3d_reg.h"

#define SVGA_PAGE_SHIFT 12u
#define SVGA_PAGE_SIZE (1u << SVGA_PAGE_SHIFT)

struct svga_region;
enum svga_region_type { SVGA_REGION_GMR, SVGA_REGION_MOB };

/* Probe limits and establish device-global GB tables before publishing the
 * backend. On partial initialization the caller still runs fini. All DMA
 * operations take place under DRM execution ownership. The optional reclaim
 * callback queues detaches for at least the requested pages or returns an
 * errno. It may release regions but must not allocate a new one. */
int svga_dma_init(int (*reclaim_mobs)(uint32_t pages));
/* After all region users are gone, drain detach fences and remove the tables.
 * On failure their backing remains pinned and the device cannot be reprobed. */
int svga_dma_fini(void);
bool svga_dma_supported(enum svga_region_type type);
uint32_t svga_dma_max_mob_size(void);
uint64_t svga_dma_mob_bytes(void);

/* Bind pins the BO independently of its handles and VMAs and shares its
 * mapping by region type. Create allocates private, zeroed backing. Success
 * gives the caller one reference and orders the mapping before later use. */
int svga_region_bind(enum svga_region_type type, struct drm_bo *bo,
                     struct svga_region **out);
int svga_region_create(enum svga_region_type type, uint32_t bytes,
                       struct svga_region **out);
int svga_region_get(struct svga_region *region);
uint32_t svga_region_id(const struct svga_region *region);
/* Borrowed CPU mapping, valid for the retained region lifetime. */
void *svga_region_pixels(const struct svga_region *region);
struct drm_bo *svga_region_bo(const struct svga_region *region);
/* Live binding references under DRM execution ownership. */
uint32_t svga_region_users(const struct svga_region *region);

/* Consume one reference after the caller's last GPU use was enqueued. The
 * last reference queues detach. Backing and ID remain pinned until its fence.
 * Return the pages freed or queued for retirement by this release. */
uint32_t svga_region_release(struct svga_region *region);
uint32_t svga_dma_retire(uint32_t budget);

#endif
