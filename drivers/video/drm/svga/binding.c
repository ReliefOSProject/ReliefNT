/* SPDX-License-Identifier: Apache-2.0 */
#include "context.h"
#include <reliefnt/heap.h>

/* One packet for each scalar binding and one per array. Grouping the latter
 * fills gaps with INVALID_ID and keeps the scrub/rebind in one submission. */
#define PIPELINE_WORDS (SVGA3D_NUM_SHADERTYPE * \
    (7 * SVGA3D_DX_MAX_CONSTBUFFERS + 4 + 4 + SVGA3D_DX_MAX_SRVIEWS) + \
    3 + 3 * SVGA3D_DX_MAX_VERTEXBUFFERS + 5 + \
    3 + SVGA3D_DX_MAX_RENDER_TARGETS + 3 + 3 * SVGA3D_DX_MAX_SOTARGETS + \
    2 * (3 + SVGA3D_MAX_UAVIEWS) + 3 + 4)

struct pipeline {
    uint32_t *cursor;
    const struct svga_context *context;
    bool restore;
};

static void emit(struct pipeline *p, uint32_t command, const uint32_t *body, unsigned count,
                 const uint32_t *array, unsigned entries)
{
    *p->cursor++ = command;
    *p->cursor++ = (count + entries) * sizeof(uint32_t);
    __builtin_memcpy(p->cursor, body, count * sizeof(uint32_t));
    p->cursor += count;
    if (entries) {
        __builtin_memcpy(p->cursor, array, entries * sizeof(uint32_t));
        p->cursor += entries;
    }
}

static uint32_t object_id(const struct pipeline *p, const struct svga_context_value *value)
{
    return p->restore && value && value->object ? value->object->id : SVGA3D_INVALID_ID;
}

static unsigned collect_views(const struct pipeline *p, unsigned key, unsigned start,
                              unsigned max, uint32_t *ids, unsigned *first)
{
    unsigned count = 0;
    *first = max;
    for (unsigned i = 0; i < max; ++i) {
        const struct svga_context_value *value = svga_context_lookup(p->context, key, start + i);
        ids[i] = p->restore && value ? value->id : SVGA3D_INVALID_ID;
        if (value) {
            if (!count)
                *first = i;
            count = i + 1;
        }
    }
    return count;
}

static void shader_stage(struct pipeline *p, uint32_t type)
{
    for (unsigned i = 0; i < SVGA3D_DX_MAX_CONSTBUFFERS; ++i) {
        const struct svga_context_value *v = svga_context_lookup(p->context, SVGA_BIND_CONSTANT,
            type * SVGA3D_DX_MAX_CONSTBUFFERS + i);
        if (v) {
            const uint32_t body[] = {i, type, object_id(p, v), p->restore ? v->offset : 0,
                                     p->restore ? v->size : 0};
            emit(p, SVGA_3D_CMD_DX_SET_SINGLE_CONSTANT_BUFFER, body, 5, NULL, 0);
        }
    }
    const struct svga_context_value *shader = svga_context_lookup(p->context, SVGA_BIND_DX_SHADER, type);
    if (shader) {
        const uint32_t body[] = {p->restore ? shader->id : SVGA3D_INVALID_ID, type};
        emit(p, SVGA_3D_CMD_DX_SET_SHADER, body, 2, NULL, 0);
    }
    uint32_t ids[SVGA3D_DX_MAX_SRVIEWS];
    unsigned first;
    unsigned end = collect_views(p, SVGA_BIND_SRV, type * SVGA3D_DX_MAX_SRVIEWS,
                                 SVGA3D_DX_MAX_SRVIEWS, ids, &first);
    if (end) {
        const uint32_t body[] = { first, type };
        emit(p, SVGA_3D_CMD_DX_SET_SHADER_RESOURCES, body, 2, ids + first, end - first);
    }
}

static void buffers(struct pipeline *p, unsigned key, uint32_t command, unsigned max)
{
    uint32_t words[3 * SVGA3D_DX_MAX_VERTEXBUFFERS];
    unsigned first = max, end = 0;
    for (unsigned i = 0; i < max; ++i) {
        const struct svga_context_value *v = svga_context_lookup(p->context, key, i);
        bool attach = p->restore && v;
        words[3 * i] = object_id(p, v);
        words[3 * i + 1] = attach ? (key == SVGA_BIND_VERTEX ? v->type : v->offset) : 0;
        words[3 * i + 2] = attach ? (key == SVGA_BIND_VERTEX ? v->offset : v->size) : 0;
        if (v) {
            if (!end)
                first = i;
            end = i + 1;
        }
    }
    if (!end)
        return;
    if (key == SVGA_BIND_SO_TARGET)
        first = 0;
    const uint32_t start = first;
    emit(p, command, &start, 1, words + 3 * first, 3 * (end - first));
}

static void outputs(struct pipeline *p)
{
    uint32_t ids[SVGA3D_MAX_UAVIEWS];
    unsigned first;
    unsigned end = collect_views(p, SVGA_BIND_RT, 0, SVGA3D_DX_MAX_RENDER_TARGETS, ids, &first);
    const struct svga_context_value *ds = svga_context_lookup(p->context, SVGA_BIND_DS, 0);
    if (ds || end) {
        const uint32_t dsv = p->restore && ds ? ds->id : SVGA3D_INVALID_ID;
        emit(p, SVGA_3D_CMD_DX_SET_RENDERTARGETS, &dsv, 1, ids, end);
    }

    const struct svga_context_value *splice = svga_context_lookup(p->context, SVGA_DX_UAV_SPLICE, 0);
    end = collect_views(p, SVGA_BIND_UAV, 0, SVGA3D_MAX_UAVIEWS, ids, &first);
    if (splice && end)
        emit(p, SVGA_3D_CMD_DX_SET_UA_VIEWS, &splice->id, 1, ids, end);
    end = collect_views(p, SVGA_BIND_CS_UAV, 0, SVGA3D_MAX_UAVIEWS, ids, &first);
    if (end) {
        const uint32_t start = first;
        emit(p, SVGA_3D_CMD_DX_SET_CS_UA_VIEWS, &start, 1, ids + first, end - first);
    }
}

int svga_dx_pipeline(const struct svga_context *context, bool restore, uint32_t query_mob)
{
    const struct svga_context_value *query = svga_context_lookup(context, SVGA_DX_QUERY_MOB, 0);
    uint32_t *stream = kernel_malloc(PIPELINE_WORDS * sizeof(uint32_t));
    if (!stream)
        return -ENOMEM;
    struct pipeline pipeline = { .cursor = stream, .context = context, .restore = restore };
    for (unsigned type = SVGA3D_SHADERTYPE_MIN; type < SVGA3D_SHADERTYPE_MAX; ++type)
        shader_stage(&pipeline, type);

    buffers(&pipeline, SVGA_BIND_VERTEX, SVGA_3D_CMD_DX_SET_VERTEX_BUFFERS, SVGA3D_DX_MAX_VERTEXBUFFERS);
    const struct svga_context_value *index = svga_context_lookup(context, SVGA_BIND_INDEX, 0);
    if (index) {
        const uint32_t body[] = { object_id(&pipeline, index), restore ? index->type : 0,
                                  restore ? index->offset : 0 };
        emit(&pipeline, SVGA_3D_CMD_DX_SET_INDEX_BUFFER, body, 3, NULL, 0);
    }
    outputs(&pipeline);
    const struct svga_context_value *so = svga_context_lookup(context, SVGA_BIND_DX_SO, 0);
    if (so) {
        const uint32_t id = restore ? so->id : SVGA3D_INVALID_ID;
        emit(&pipeline, SVGA_3D_CMD_DX_SET_STREAMOUTPUT, &id, 1, NULL, 0);
    }
    buffers(&pipeline, SVGA_BIND_SO_TARGET, SVGA_3D_CMD_DX_SET_SOTARGETS, SVGA3D_DX_MAX_SOTARGETS);
    if (query) {
        const uint32_t body[] = {context->object.id, query_mob};
        emit(&pipeline, restore ? SVGA_3D_CMD_DX_BIND_ALL_QUERY : SVGA_3D_CMD_DX_READBACK_ALL_QUERY,
             body, restore ? 2 : 1, NULL, 0);
    }
    uint32_t bytes = (pipeline.cursor - stream) * sizeof(*stream);
    uint32_t fence;
    bool readback = query && !restore;
    int ret = bytes ? svga_submit_commands(stream, bytes, context->object.id, readback ? &fence : NULL) : 0;
    kernel_free(stream);
    /* Pressure can return without waiting for MOB retirement. Complete query
     * writes before allowing the next CPU grab of the result BO. */
    return !ret && readback ? svga_submit_wait(fence) : ret;
}
