/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_EXEC_H
#define RELIEFNT_SVGA_EXEC_H

#include "render.h"
#include "stream.h"

struct svga_validation_refs;
struct svga_context_change;
struct svga_buffer;
struct drm_sync_fence;

/* Valid only under DRM execution ownership. Staged state and unique resource
 * references belong to the batch until commit or rollback. After commit the
 * retirement queue owns the references and no longer needs the caller's OFD. */
struct svga_batch {
    struct svga_batch *next;
    struct drm_file *file;
    struct svga_validation_refs *refs;
    struct svga_context_change *changes;
    struct svga_backing_change *backing_changes;
    struct svga_buffer *mapped_mobs;
    struct svga_context *dx;
    struct svga_context *query_context;
    struct svga_buffer *query_buffer;
    struct svga_fence *fence;
    struct drm_sync_fence *sync;
    uint64_t serial, submitted_tick;
    uint32_t sequence, table_need[SVGA_COTABLE_MAX];
    void *query_tail;
    uint32_t query_tail_bytes, query_tail_capacity;
};

int svga_batch_object(struct svga_batch *batch, struct svga_object *object);
/* These update staged state only. The surface must be validated first. */
void svga_batch_surface_written(struct svga_surface *surface);
void svga_batch_surface_cleaned(struct svga_surface *surface);
/* Retain an object used by a kernel command that does not use its bindings. */
int svga_batch_pin_object(struct svga_batch *batch, struct svga_object *object);
int svga_batch_pin_buffer(struct svga_batch *batch, struct svga_buffer *buffer);
int svga_batch_bo(struct svga_batch *batch, struct drm_bo *bo, struct svga_buffer **out);
int svga_batch_handle(struct svga_batch *batch, uint32_t handle, struct svga_buffer **out);
/* Borrow a MOB through the end of validation, keyed by a batch-pinned BO.
 * Close these leases after publication or rollback of the staged bindings. */
int svga_batch_map_mob(struct svga_batch *batch, struct svga_buffer *buffer,
                       struct svga_region **out);
void svga_batch_unmap_mobs(struct svga_batch *batch);
struct drm_bo *svga_buffer_bo(const struct svga_buffer *buffer);
void svga_buffer_get(struct svga_buffer *buffer);
void svga_buffer_put(struct svga_buffer *buffer);
void svga_buffer_fence(struct svga_buffer *buffer, uint32_t sequence);
bool svga_buffer_cpu_grabbed(const struct svga_buffer *buffer);
int svga_buffer_map(struct svga_buffer *buffer, enum svga_region_type type, uint32_t *id);
int svga_batch_context(struct svga_batch *batch, uint32_t *id, struct svga_context **out);
int svga_batch_surface(struct svga_batch *batch, uint32_t *id, bool optional,
                        struct svga_surface **out);
int svga_batch_image(struct svga_batch *batch, SVGA3dSurfaceImageId *id, bool optional,
                      struct svga_surface **surface, SVGA3dSize *size);

int svga_batch_rebind(struct svga_batch *batch, struct svga_region **slot,
                       struct svga_surface *surface, uint32_t *offset_slot,
                       struct svga_buffer *buffer, uint32_t offset, uint32_t *mob);
int svga_batch_query_wait(struct svga_batch *batch, uint32_t command, const void *body,
                           uint32_t bytes);
int svga_query_end(struct svga_batch *batch, struct svga_context *context,
                    struct svga_buffer *buffer);
void svga_query_finish(const struct svga_batch *batch, bool commit);
/* 1 when legacy query writes to this BO are complete, 0 while pending. */
int svga_query_poll(const struct drm_bo *bo, bool nowait);
int svga_query_wait(const struct drm_bo *bo);
int svga_validate_legacy(struct svga_batch *batch, const struct svga_packet *packet);
int svga_validate_dx(struct svga_batch *batch, const struct svga_packet *packet);
int svga_context_validate_bindings(struct svga_batch *batch, struct svga_context *context);
int svga_context_prepare(struct svga_batch *batch);
void svga_context_finish(struct svga_batch *batch, bool commit);

uint32_t svga_fence_events_retire(uint32_t budget);
void svga_fence_events_release(struct svga_render_file *state);

#endif
