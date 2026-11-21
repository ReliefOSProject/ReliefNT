/* SPDX-License-Identifier: Apache-2.0 */
#include "context.h"
#include <reliefnt/heap.h>

#define STATE_BUCKETS 64u
#define BINDING_BUCKETS 16u
#define STATE_LIMIT 65536u

struct svga_context_slot {
    struct svga_context_slot *next;
    struct svga_context_value value;
    uint64_t staged;
    uint32_t id;
    unsigned key;
    bool scrubbed;
};

struct svga_view_definition {
    unsigned references;
    SVGA3dCmdHeader header;
    uint32_t body[];
};

struct svga_cotable {
    struct drm_bo *bo;
    struct svga_region *region;
};

struct svga_dx_state {
    struct svga_context_slot *slots[STATE_BUCKETS];
    struct svga_context_slot *bindings[BINDING_BUCKETS];
    struct svga_cotable tables[SVGA_COTABLE_MAX];
    unsigned count, scrubbed_count;
    bool bindings_scrubbed;
};

struct svga_context_change {
    struct svga_context_change *next;
    struct svga_dx_state *state;
    struct svga_context_slot *slot;
    struct svga_context_value before;
};

static struct svga_context *lru_head, *lru_tail;

static int replay_bound_resources(struct svga_context *context);
static int scrub_resources(struct svga_context *context);

static struct svga_context_slot **slot_link(struct svga_dx_state *state, unsigned key, uint32_t id)
{
    uint32_t hash = id * 2654435761u + key * 2246822519u;
    struct svga_context_slot **link = key < SVGA_BIND_LEGACY_TEXTURE ?
        &state->slots[hash >> 26] : &state->bindings[hash >> 28];
    while (*link && ((*link)->key != key || (*link)->id != id))
        link = &(*link)->next;
    return link;
}

struct svga_cotable_bind {
    SVGA3dCmdHeader header;
    SVGA3dCmdDXSetCOTable body;
};

struct svga_cotable_readback {
    SVGA3dCmdHeader header;
    SVGA3dCmdDXReadbackCOTable body;
};

static struct svga_cotable_bind cotable_bind(const struct svga_context *context, unsigned key,
                                            struct svga_region *region, uint32_t valid_bytes)
{
    return (struct svga_cotable_bind){
        SVGA_HEADER(SVGA_3D_CMD_DX_SET_COTABLE, SVGA3dCmdDXSetCOTable),
        {.cid = context->object.id, .mobid = region ? svga_region_id(region) : SVGA3D_INVALID_ID,
         .type = key, .validSizeInBytes = valid_bytes},
    };
}

static struct svga_cotable_readback cotable_readback(const struct svga_context *context, unsigned key)
{
    return (struct svga_cotable_readback){
        SVGA_HEADER(SVGA_3D_CMD_DX_READBACK_COTABLE, SVGA3dCmdDXReadbackCOTable),
        {.cid = context->object.id, .type = key},
    };
}

static void cotable_release(struct svga_cotable *table)
{
    svga_region_release(table->region);
    drm_bo_put(table->bo);
    *table = (struct svga_cotable){0};
}

static int cotable_alloc(uint32_t bytes, struct svga_cotable *table)
{
    int ret = drm_bo_alloc(bytes, &table->bo);
    if (!ret)
        ret = svga_region_bind(SVGA_REGION_MOB, table->bo, &table->region);
    if (ret)
        cotable_release(table);
    return ret;
}

static void context_lru_remove(struct svga_context *context)
{
    if (context->lru_prev)
        context->lru_prev->lru_next = context->lru_next;
    else if (lru_head == context)
        lru_head = context->lru_next;
    else
        return;
    if (context->lru_next)
        context->lru_next->lru_prev = context->lru_prev;
    else
        lru_tail = context->lru_prev;
    context->lru_prev = context->lru_next = NULL;
}

static void context_lru_touch(struct svga_context *context)
{
    if (context == lru_tail || !context->state)
        return;
    context_lru_remove(context);
    context->lru_prev = lru_tail;
    if (lru_tail)
        lru_tail->lru_next = context;
    else
        lru_head = context;
    lru_tail = context;
}

int svga_context_publish(struct svga_context *context)
{
    uint32_t id = context->object.id;
    int ret;

    if (context->type == SVGA_CONTEXT_HOST) {
        const uint32_t define[] = {SVGA_3D_CMD_CONTEXT_DEFINE, sizeof(uint32_t), id};
        ret = svga_submit(define, sizeof(define));
    } else {
        bool dx = context->type == SVGA_CONTEXT_DX;
        ret = drm_bo_alloc(dx ? sizeof(SVGADXContextMobFormat) : sizeof(SVGAGBContextData),
                           &context->state_bo);
        if (ret)
            return ret;
        ret = svga_region_bind(SVGA_REGION_MOB, context->state_bo, &context->state);
        if (ret)
            return ret;
        /* GB and DX use the same define and bind field layout. */
        const uint32_t define[] = {
            dx ? SVGA_3D_CMD_DX_DEFINE_CONTEXT : SVGA_3D_CMD_DEFINE_GB_CONTEXT, 4, id,
            dx ? SVGA_3D_CMD_DX_BIND_CONTEXT : SVGA_3D_CMD_BIND_GB_CONTEXT, 12,
            id, svga_region_id(context->state), 0,
        };
        ret = svga_submit(define, sizeof(define));
    }
    if (!ret) {
        context->object.published = true;
        context_lru_touch(context);
    }
    return ret;
}

/* Stage every MOB before making the snapshot visible to the device. A
 * failed allocation leaves the context and its saved tables detached. */
static int restore_context(struct svga_context *context)
{
    struct svga_region *tables[SVGA_COTABLE_MAX] = {0}, *state;
    int ret = svga_region_bind(SVGA_REGION_MOB, context->state_bo, &state);
    if (ret)
        return ret;

    const bool dx = context->type == SVGA_CONTEXT_DX;
    struct {
        uint32_t bind[5];
        struct svga_cotable_bind tables[SVGA_COTABLE_MAX];
    } packets = {{dx ? SVGA_3D_CMD_DX_BIND_CONTEXT : SVGA_3D_CMD_BIND_GB_CONTEXT,
                   12, context->object.id, svga_region_id(state), 1}, {0}};
    unsigned count = 0;
    if (dx && context->dx) {
        for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i) {
            struct drm_bo *bo = context->dx->tables[i].bo;
            if (!bo)
                continue;
            ret = svga_region_bind(SVGA_REGION_MOB, bo, &tables[i]);
            if (ret)
                goto release;
            packets.tables[count++] = cotable_bind(context, i, tables[i], drm_bo_size(bo));
        }
    }
    ret = svga_submit(&packets, sizeof(packets.bind) + count * sizeof(packets.tables[0]));
    if (ret)
        goto release;

    context->state = state;
    if (dx && context->dx)
        for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i)
            context->dx->tables[i].region = tables[i];
    return 0;

release:
    for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i)
        svga_region_release(tables[i]);
    svga_region_release(state);
    return ret;
}

int svga_context_validate_residency(struct svga_context *context)
{
    if (context->state_bo && !context->state) {
        int ret = restore_context(context);
        if (ret)
            return ret;
    }
    struct svga_dx_state *state = context->dx;
    if (state && state->bindings_scrubbed) {
        int ret = replay_bound_resources(context);
        if (ret)
            return ret;

        struct svga_context_slot *query = *slot_link(state, SVGA_DX_QUERY_MOB, 0);
        struct svga_region *mob = query ? query->value.mob : NULL;
        bool acquired = query && !mob;
        if (acquired) {
            ret = svga_region_bind(SVGA_REGION_MOB, svga_buffer_bo(query->value.buffer), &mob);
            if (ret)
                return ret;
        }
        ret = svga_dx_pipeline(context, true, mob ? svga_region_id(mob) : SVGA3D_INVALID_ID);
        if (acquired) {
            if (!ret)
                query->value.mob = mob;
            else
                svga_region_release(mob);
        }
        if (ret)
            return ret;
        state->bindings_scrubbed = false;
    }
    context_lru_touch(context);
    return 0;
}

static uint32_t detach_slot_mob(struct svga_context_slot *slot)
{
    if (!slot)
        return 0;
    uint32_t pages = svga_region_release(slot->value.mob);
    slot->value.mob = NULL;
    return pages;
}

/* Like vmwgfx_context.c, detach COTables before their DX context. All
 * backing survives until the queued detaches retire, then the context
 * owns the saved contents for its next bind. */
static int evict_context(struct svga_context *context, uint32_t *pages)
{
    const bool dx = context->type == SVGA_CONTEXT_DX;
    struct svga_dx_state *state = dx ? context->dx : NULL;
    struct {
        struct svga_cotable_readback readback;
        struct svga_cotable_bind unbind;
    } tables[SVGA_COTABLE_MAX];
    unsigned count = 0;

    if (!dx)
        svga_query_release(context);
    if (svga.fifo.fault)
        return -EIO;
    if (state) {
        if (!state->bindings_scrubbed) {
            int ret = svga_dx_pipeline(context, false, SVGA3D_INVALID_ID);
            if (ret)
                return ret;
            state->bindings_scrubbed = true;
        }
        int ret = scrub_resources(context);
        if (ret)
            return ret;
        for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i) {
            if (!state->tables[i].region)
                continue;
            tables[count].readback = cotable_readback(context, i);
            tables[count++].unbind = cotable_bind(context, i, NULL, 0);
        }
    }
    const uint32_t id = context->object.id;
    const uint32_t detach[] = {
        dx ? SVGA_3D_CMD_DX_READBACK_CONTEXT : SVGA_3D_CMD_READBACK_GB_CONTEXT, 4, id,
        dx ? SVGA_3D_CMD_DX_BIND_CONTEXT : SVGA_3D_CMD_BIND_GB_CONTEXT, 12, id, SVGA3D_INVALID_ID, 0,
    };
    uint32_t table_bytes = count * sizeof(tables[0]);
    uint8_t packets[sizeof(tables) + sizeof(detach)];
    __builtin_memcpy(packets, tables, table_bytes);
    __builtin_memcpy(packets + table_bytes, detach, sizeof(detach));
    int ret = svga_submit(packets, table_bytes + sizeof(detach));
    if (ret)
        return ret;

    context_lru_remove(context);
    uint32_t freed = svga_region_release(context->state);
    context->state = NULL;
    if (state) {
        for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i) {
            struct svga_cotable *table = &state->tables[i];
            freed += svga_region_release(table->region);
            table->region = NULL;
        }
        /* Resource scrubs and query readback precede the releases. A MOB
         * shared with another context, a surface or a legacy query remains
         * resident until that consumer releases its own lease. */
        for (unsigned i = 0; i < STATE_BUCKETS; ++i)
            for (struct svga_context_slot *slot = state->slots[i]; slot; slot = slot->next)
                freed += detach_slot_mob(slot);
        freed += detach_slot_mob(*slot_link(state, SVGA_DX_QUERY_MOB, 0));
    }
    *pages = freed < *pages ? *pages - freed : 0;
    return svga.fifo.fault ? -EIO : 0;
}

int svga_render_reclaim_mobs(uint32_t pages)
{
    int ret = svga_surface_reclaim_mobs(&pages);
    if (ret || !pages)
        return ret;

    for (struct svga_context *context = lru_head, *next; context; context = next) {
        next = context->lru_next;
        if (context->object.references != 1)
            continue;
        const struct svga_context_value *query = svga_context_lookup(context, SVGA_DX_QUERY_MOB, 0);
        if (query && (svga_buffer_cpu_grabbed(query->buffer) || drm_bo_gpu_busy(svga_buffer_bo(query->buffer))))
            continue;
        ret = evict_context(context, &pages);
        if (ret || !pages)
            return ret;
    }
    return -ENOSPC;
}

static struct svga_dx_state *get_state(struct svga_context *context)
{
    if (!context->dx) {
        context->dx = kernel_malloc(sizeof(*context->dx));
        if (context->dx)
            *context->dx = (struct svga_dx_state){0};
    }
    return context->dx;
}

const struct svga_context_value *svga_context_lookup(const struct svga_context *context,
                                                      unsigned key, uint32_t id)
{
    if (!context->dx)
        return NULL;
    struct svga_context_slot *slot = *slot_link(context->dx, key, id);
    return slot && slot->value.defined ? &slot->value : NULL;
}

static void value_put(struct svga_context_value *value)
{
    if (value->object)
        svga_object_put(value->object);
    svga_region_release(value->mob);
    svga_buffer_put(value->buffer);
    if (value->view && !--value->view->references)
        kernel_free(value->view);
}

static void delete_slot(struct svga_dx_state *state, struct svga_context_slot *slot)
{
    *slot_link(state, slot->key, slot->id) = slot->next;
    --state->count;
    kernel_free(slot);
}

int svga_context_stage(struct svga_batch *batch, struct svga_context *context,
                       unsigned key, uint32_t id, struct svga_context_value value)
{
    struct svga_dx_state *state = value.defined ? get_state(context) : context->dx;
    if (!state)
        return value.defined ? -ENOMEM : 0;
    struct svga_context_slot **link = slot_link(state, key, id);
    struct svga_context_slot *slot = *link;
    bool created = !slot;
    if (created) {
        if (!value.defined)
            return 0;
        if (state->count == STATE_LIMIT)
            return -ENOSPC;
        slot = kernel_malloc(sizeof(*slot));
        if (!slot)
            return -ENOMEM;
        *slot = (struct svga_context_slot){.id = id, .key = key};
        *link = slot;
        ++state->count;
    }
    if (value.mob) {
        int ret = svga_region_get(value.mob);
        if (ret) {
            if (created)
                delete_slot(state, slot);
            return ret;
        }
    }
    if (slot->staged != batch->serial) {
        struct svga_context_change *change = kernel_malloc(sizeof(*change));
        if (!change) {
            svga_region_release(value.mob);
            if (created)
                delete_slot(state, slot);
            return -ENOMEM;
        }
        *change = (struct svga_context_change){.next = batch->changes, .state = state,
            .slot = slot, .before = slot->value};
        batch->changes = change;
        slot->staged = batch->serial;
        slot->value = (struct svga_context_value){0};
    }
    if (value.object)
        svga_object_get(value.object);
    svga_buffer_get(value.buffer);
    if (value.view)
        ++value.view->references;
    value_put(&slot->value);
    slot->value = value;
    return 0;
}

static void mark_binding_written(unsigned key, const struct svga_context_value *value)
{
    if (!value->object || value->object->type != SVGA_SURFACE)
        return;
    switch (key) {
    case SVGA_BIND_LEGACY_RT:
    case SVGA_BIND_RT:
    case SVGA_BIND_DS:
    case SVGA_BIND_UAV:
    case SVGA_BIND_CS_UAV:
    case SVGA_BIND_SO_TARGET:
        svga_batch_surface_written((struct svga_surface *)value->object);
        break;
    }
}

int svga_context_stage_binding(struct svga_batch *batch, struct svga_context *context,
                               unsigned key, uint32_t slot, struct svga_context_value value)
{
    value.active = true;
    int ret = svga_context_stage(batch, context, key, slot, value);
    if (!ret)
        mark_binding_written(key, &value);
    return ret;
}

int svga_context_remove_bindings(struct svga_batch *batch, struct svga_context *context,
                                 unsigned type, uint32_t id, unsigned first, unsigned last)
{
    if (!context->dx)
        return 0;
    for (unsigned i = 0; i < BINDING_BUCKETS; ++i) {
        for (struct svga_context_slot *slot = context->dx->bindings[i]; slot; slot = slot->next) {
            const struct svga_context_value *value = &slot->value;
            if (!value->defined || !value->active || value->type != type || value->id != id ||
                slot->key < first || slot->key > last)
                continue;
            int ret = svga_context_stage(batch, context, slot->key, slot->id,
                (struct svga_context_value){0});
            if (ret)
                return ret;
        }
    }
    return 0;
}

int svga_context_validate_bindings(struct svga_batch *batch, struct svga_context *context)
{
    if (!context->dx)
        return 0;
    for (unsigned i = 0; i < BINDING_BUCKETS; ++i) {
        for (const struct svga_context_slot *slot = context->dx->bindings[i]; slot; slot = slot->next) {
            const struct svga_context_value *value = &slot->value;
            if (!value->defined || !value->active)
                continue;
            int ret = value->object ? svga_batch_object(batch, value->object) : 0;
            if (!ret && value->buffer)
                ret = svga_batch_bo(batch, svga_buffer_bo(value->buffer), NULL);
            if (ret)
                return ret;
            mark_binding_written(slot->key, value);
        }
    }
    return 0;
}

int svga_context_stage_view(struct svga_batch *batch, unsigned table,
                            const struct svga_packet *packet, struct svga_surface *surface)
{
    const uint32_t *body = packet->body;
    if (svga_context_lookup(batch->dx, table, body[0]))
        return -EINVAL;
    int ret = svga_context_require_table(batch, table, body[0]);
    if (ret)
        return ret;

    struct svga_view_definition *view = kernel_malloc(sizeof(*view) + packet->bytes);
    if (!view)
        return -ENOMEM;
    *view = (struct svga_view_definition){.header = {packet->command, packet->bytes}};
    __builtin_memcpy(view->body, packet->body, packet->bytes);
    ret = svga_context_stage(batch, batch->dx, table, body[0],
        (struct svga_context_value){.object = &surface->object, .view = view, .defined = true});
    if (ret)
        kernel_free(view);
    return ret;
}

static struct svga_context_slot *scrubbed_slot(const struct svga_batch *batch, unsigned table, uint32_t id)
{
    struct svga_dx_state *state = batch->dx->dx;
    if (!state || !state->scrubbed_count)
        return NULL;
    struct svga_context_slot *slot = *slot_link(state, table, id);
    /* A definition or bind staged by this execbuf is already part of its
     * command stream. It has no independent hardware state to restore. */
    return slot && slot->scrubbed && slot->staged != batch->serial ? slot : NULL;
}

bool svga_context_view_scrubbed(const struct svga_batch *batch, unsigned table, uint32_t id)
{
    const struct svga_context_slot *slot = scrubbed_slot(batch, table, id);
    return slot && slot->value.view;
}

#define RESOURCE_CHUNK_WORDS 256
#define RESOURCE_CHUNK_SLOTS 32

struct resource_commands {
    uint32_t words[RESOURCE_CHUNK_WORDS];
    struct {
        struct svga_context_slot *slot;
        struct svga_region *mob;
    } entries[RESOURCE_CHUNK_SLOTS];
    unsigned bytes, count;
};

static void set_scrubbed(struct svga_dx_state *state, struct svga_context_slot *slot, bool scrubbed)
{
    if (slot->scrubbed == scrubbed)
        return;
    slot->scrubbed = scrubbed;
    if (scrubbed)
        ++state->scrubbed_count;
    else
        --state->scrubbed_count;
}

static int submit_resources(const struct svga_context *context, struct resource_commands *commands,
                            bool restore)
{
    if (!commands->bytes)
        return 0;
    int ret = svga_submit_commands(commands->words, commands->bytes, context->object.id, NULL);
    if (ret)
        return ret;
    for (unsigned i = 0; i < commands->count; ++i) {
        struct svga_context_slot *slot = commands->entries[i].slot;
        if (commands->entries[i].mob)
            slot->value.mob = commands->entries[i].mob;
        set_scrubbed(context->dx, slot, !restore);
    }
    commands->count = commands->bytes = 0;
    return 0;
}

static int finish_resources(const struct svga_context *context, struct resource_commands *commands,
                             bool restore, int ret)
{
    if (!ret)
        ret = submit_resources(context, commands, restore);
    for (unsigned i = 0; i < commands->count; ++i)
        svga_region_release(commands->entries[i].mob);
    return ret;
}

static int enqueue_resource(const struct svga_context *context, struct resource_commands *commands,
                            struct svga_context_slot *slot, bool restore)
{
    static const uint32_t destroy_view[] = {
        [SVGA_COTABLE_RTVIEW] = SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
        [SVGA_COTABLE_DSVIEW] = SVGA_3D_CMD_DX_DESTROY_DEPTHSTENCIL_VIEW,
        [SVGA_COTABLE_SRVIEW] = SVGA_3D_CMD_DX_DESTROY_SHADERRESOURCE_VIEW,
        [SVGA_COTABLE_UAVIEW] = SVGA_3D_CMD_DX_DESTROY_UA_VIEW,
    };
    if (slot->scrubbed != restore || !slot->value.defined)
        return 0;
    const struct svga_context_value *value = &slot->value;
    bool has_mob = value->buffer &&
                   (slot->key == SVGA_COTABLE_DXSHADER || slot->key == SVGA_COTABLE_STREAMOUTPUT);
    if (!value->view && !has_mob)
        return 0;
    for (unsigned i = 0; i < commands->count; ++i)
        if (commands->entries[i].slot == slot)
            return 0;

    uint32_t command[6];
    const void *source = command;
    unsigned bytes = value->view ? (restore ? sizeof(value->view->header) + value->view->header.size :
                                               3 * sizeof(uint32_t)) : sizeof(command);
    if (bytes > sizeof(commands->words))
        return -E2BIG;
    if (commands->count == RESOURCE_CHUNK_SLOTS || bytes > sizeof(commands->words) - commands->bytes) {
        int ret = submit_resources(context, commands, restore);
        if (ret)
            return ret;
    }

    struct svga_region *acquired = NULL;
    if (value->view) {
        if (restore) {
            source = &value->view->header;
        } else {
            command[0] = destroy_view[slot->key];
            command[1] = sizeof(uint32_t);
            command[2] = slot->id;
        }
    } else {
        uint32_t mob = SVGA3D_INVALID_ID;
        if (restore) {
            if (!value->mob) {
                int ret = svga_region_bind(SVGA_REGION_MOB, svga_buffer_bo(value->buffer), &acquired);
                if (ret)
                    return ret;
            }
            mob = svga_region_id(acquired ? acquired : value->mob);
        }
        uint32_t offset = restore ? value->offset : 0;
        if (slot->key == SVGA_COTABLE_DXSHADER) {
            const uint32_t bind[] = {SVGA_3D_CMD_DX_BIND_SHADER, sizeof(SVGA3dCmdDXBindShader),
                                     context->object.id, slot->id, mob, offset};
            __builtin_memcpy(command, bind, sizeof(bind));
        } else {
            const uint32_t bind[] = {SVGA_3D_CMD_DX_BIND_STREAMOUTPUT, sizeof(SVGA3dCmdDXBindStreamOutput),
                                     slot->id, mob, offset, value->size};
            __builtin_memcpy(command, bind, sizeof(bind));
        }
    }
    __builtin_memcpy((uint8_t *)commands->words + commands->bytes, source, bytes);
    commands->entries[commands->count].slot = slot;
    commands->entries[commands->count++].mob = acquired;
    commands->bytes += bytes;
    return 0;
}

/* DX views depend on surface IDs and are destroyed before COTable readback.
 * Shader and SO entries retain their declarations and detach only their MOB.
 * Update progress per submitted chunk so partial eviction remains retryable. */
static int scrub_resources(struct svga_context *context)
{
    struct resource_commands commands = {0};
    for (unsigned i = 0; i < STATE_BUCKETS; ++i) {
        for (struct svga_context_slot *slot = context->dx->slots[i]; slot; slot = slot->next) {
            int ret = enqueue_resource(context, &commands, slot, false);
            if (ret)
                return finish_resources(context, &commands, false, ret);
        }
    }
    return finish_resources(context, &commands, false, 0);
}

static int replay_bound_resources(struct svga_context *context)
{
    struct resource_commands commands = {0};
    struct svga_dx_state *state = context->dx;
    for (unsigned i = 0; i < BINDING_BUCKETS; ++i) {
        for (const struct svga_context_slot *binding = state->bindings[i]; binding; binding = binding->next) {
            if (binding->key < SVGA_BIND_RT || binding->key > SVGA_BIND_DX_SO ||
                binding->key == SVGA_BIND_SO_TARGET || !binding->value.defined)
                continue;
            struct svga_context_slot *slot = *slot_link(state, binding->value.type, binding->value.id);
            if (slot) {
                int ret = enqueue_resource(context, &commands, slot, true);
                if (ret)
                    return finish_resources(context, &commands, true, ret);
            }
        }
    }
    return finish_resources(context, &commands, true, 0);
}

int svga_context_replay_resource(const struct svga_batch *batch, unsigned table, uint32_t id)
{
    struct svga_context_slot *slot = scrubbed_slot(batch, table, id);
    if (!slot)
        return 0;
    struct resource_commands commands = {0};
    int ret = enqueue_resource(batch->dx, &commands, slot, true);
    return finish_resources(batch->dx, &commands, true, ret);
}

static const uint16_t entry_bytes[SVGA_COTABLE_MAX] = {
    [SVGA_COTABLE_RTVIEW] = sizeof(SVGACOTableDXRTViewEntry),
    [SVGA_COTABLE_DSVIEW] = sizeof(SVGACOTableDXDSViewEntry),
    [SVGA_COTABLE_SRVIEW] = sizeof(SVGACOTableDXSRViewEntry),
    [SVGA_COTABLE_ELEMENTLAYOUT] = sizeof(SVGACOTableDXElementLayoutEntry),
    [SVGA_COTABLE_BLENDSTATE] = sizeof(SVGACOTableDXBlendStateEntry),
    [SVGA_COTABLE_DEPTHSTENCIL] = sizeof(SVGACOTableDXDepthStencilEntry),
    [SVGA_COTABLE_RASTERIZERSTATE] = sizeof(SVGACOTableDXRasterizerStateEntry),
    [SVGA_COTABLE_SAMPLER] = sizeof(SVGACOTableDXSamplerEntry),
    [SVGA_COTABLE_STREAMOUTPUT] = sizeof(SVGACOTableDXStreamOutputEntry),
    [SVGA_COTABLE_DXQUERY] = sizeof(SVGACOTableDXQueryEntry),
    [SVGA_COTABLE_DXSHADER] = sizeof(SVGACOTableDXShaderEntry),
    [SVGA_COTABLE_UAVIEW] = sizeof(SVGACOTableDXUAViewEntry),
};

int svga_context_require_table(struct svga_batch *batch, unsigned key, uint32_t id)
{
    if (!batch->dx || key >= SVGA_COTABLE_MAX || id >= SVGA_COTABLE_MAX_IDS)
        return -EINVAL;
    uint32_t bytes = (id + 1) * entry_bytes[key];
    if (bytes > batch->table_need[key])
        batch->table_need[key] = bytes;
    return 0;
}

struct cotable_update {
    unsigned key;
    struct svga_cotable replacement;
};

/* Use the Linux readback/copy/set sequence and share one fence when an
 * execbuf grows several tables. No software state changes before SET. */
static int update_tables(struct svga_context *context, struct cotable_update *updates, unsigned count)
{
    struct svga_cotable_readback readbacks[SVGA_COTABLE_MAX];
    struct svga_cotable_bind binds[SVGA_COTABLE_MAX];
    unsigned reads = 0;
    for (unsigned i = 0; i < count; ++i) {
        unsigned key = updates[i].key;
        const struct svga_cotable *old = &context->dx->tables[key];
        if (old->region)
            readbacks[reads++] = cotable_readback(context, key);
        binds[i] = cotable_bind(context, key, updates[i].replacement.region, drm_bo_size(old->bo));
    }
    if (reads) {
        uint32_t fence;
        int ret = svga_submit_fenced(readbacks, reads * sizeof(readbacks[0]), &fence);
        if (!ret)
            ret = svga_submit_wait(fence);
        if (ret)
            return ret;
        for (unsigned i = 0; i < count; ++i) {
            const struct svga_cotable *old = &context->dx->tables[updates[i].key];
            if (old->region)
                __builtin_memcpy(svga_region_pixels(updates[i].replacement.region),
                                 svga_region_pixels(old->region), drm_bo_size(old->bo));
        }
    }

    int ret = svga_submit(binds, count * sizeof(binds[0]));
    if (ret)
        return ret;
    for (unsigned i = 0; i < count; ++i) {
        struct svga_cotable *table = &context->dx->tables[updates[i].key];
        cotable_release(table);
        *table = updates[i].replacement;
        updates[i].replacement = (struct svga_cotable){0};
    }
    return svga.fifo.fault ? -EIO : 0;
}

int svga_context_prepare(struct svga_batch *batch)
{
    if (!batch->dx || !batch->dx->dx)
        return 0;

    struct cotable_update updates[SVGA_COTABLE_MAX];
    unsigned count = 0;
    int ret = 0;
    for (unsigned i = 0; i < SVGA_COTABLE_MAX;) {
        uint32_t bytes = drm_bo_size(batch->dx->dx->tables[i].bo);
        if (batch->table_need[i] <= bytes) {
            ++i;
            continue;
        }
        if (!bytes)
            bytes = SVGA_PAGE_SIZE;
        while (bytes < batch->table_need[i])
            bytes <<= 1;
        updates[count] = (struct cotable_update){.key = i};
        ret = cotable_alloc(bytes, &updates[count].replacement);
        if (ret == -ENOSPC && count) {
            /* Commit the staged group to retire old regions before trying
             * the remaining tables. Some workloads cannot fit all growths. */
            ret = update_tables(batch->dx, updates, count);
            if (ret)
                break;
            count = 0;
            continue;
        }
        if (ret)
            break;
        ++count;
        ++i;
    }
    if (!ret && count)
        ret = update_tables(batch->dx, updates, count);
    for (unsigned i = 0; i < count; ++i)
        cotable_release(&updates[i].replacement);
    return ret;
}

void svga_context_finish(struct svga_batch *batch, bool commit)
{
    while (batch->changes) {
        struct svga_context_change *change = batch->changes;
        batch->changes = change->next;
        struct svga_context_slot *slot = change->slot;
        if (commit) {
            set_scrubbed(change->state, slot, false);
            value_put(&change->before);
        } else {
            value_put(&slot->value);
            slot->value = change->before;
        }
        if (!slot->value.defined)
            delete_slot(change->state, slot);
        kernel_free(change);
    }
}

static void slots_release(struct svga_context_slot *slot)
{
    while (slot) {
        struct svga_context_slot *next = slot->next;
        value_put(&slot->value);
        kernel_free(slot);
        slot = next;
    }
}

void svga_context_release_backing(struct svga_context *context)
{
    context_lru_remove(context);
    svga_region_release(context->state);
    drm_bo_put(context->state_bo);
    struct svga_dx_state *state = context->dx;
    if (!state)
        return;
    for (unsigned i = 0; i < STATE_BUCKETS; ++i)
        slots_release(state->slots[i]);
    for (unsigned i = 0; i < BINDING_BUCKETS; ++i)
        slots_release(state->bindings[i]);
    for (unsigned i = 0; i < SVGA_COTABLE_MAX; ++i)
        cotable_release(&state->tables[i]);
    kernel_free(state);
}
