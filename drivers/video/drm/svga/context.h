/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_CONTEXT_H
#define RELIEFNT_SVGA_CONTEXT_H

#include "exec.h"

/* COTable entry keys are numbered by the wire table type. Binding keys
 * are separate because a context can define many more objects than it binds. */
enum svga_context_key {
    SVGA_LEGACY_SHADER = SVGA_COTABLE_MAX,
    SVGA_BIND_LEGACY_TEXTURE, SVGA_BIND_LEGACY_RT, SVGA_BIND_LEGACY_SHADER,
    SVGA_BIND_CONSTANT, SVGA_BIND_VERTEX, SVGA_BIND_INDEX, SVGA_BIND_RT,
    SVGA_BIND_DS, SVGA_BIND_SRV, SVGA_BIND_UAV, SVGA_BIND_CS_UAV,
    SVGA_BIND_SO_TARGET, SVGA_BIND_DX_SHADER, SVGA_BIND_DX_SO,
    SVGA_DX_UAV_SPLICE, SVGA_DX_QUERY_MOB,
};

struct svga_view_definition;

struct svga_context_value {
    struct svga_object *object;
    struct svga_buffer *buffer;
    struct svga_view_definition *view;
    /* Resident lease in a shader, stream-output or context query slot. */
    struct svga_region *mob;
    uint32_t size, offset, type, id;
    bool defined, active;
};

const struct svga_context_value *svga_context_lookup(const struct svga_context *context,
                                                      unsigned key, uint32_t id);
int svga_context_stage(struct svga_batch *batch, struct svga_context *context,
                       unsigned key, uint32_t id, struct svga_context_value value);
int svga_context_stage_binding(struct svga_batch *batch, struct svga_context *context,
                               unsigned key, uint32_t slot, struct svga_context_value value);
int svga_context_remove_bindings(struct svga_batch *batch, struct svga_context *context,
                                 unsigned type, uint32_t id, unsigned first, unsigned last);
int svga_context_stage_view(struct svga_batch *batch, unsigned table,
                            const struct svga_packet *packet, struct svga_surface *surface);
bool svga_context_view_scrubbed(const struct svga_batch *batch, unsigned table, uint32_t id);
int svga_context_replay_resource(const struct svga_batch *batch, unsigned table, uint32_t id);
int svga_dx_pipeline(const struct svga_context *context, bool restore, uint32_t query_mob);
int svga_context_require_table(struct svga_batch *batch, unsigned key, uint32_t id);

static inline int svga_context_bind_object(struct svga_batch *batch, struct svga_context *context,
                                             unsigned key, uint32_t slot, struct svga_object *object)
{
    return svga_context_stage_binding(batch, context, key, slot,
        (struct svga_context_value){.object = object, .defined = object != NULL});
}

#endif
