/* SPDX-License-Identifier: Apache-2.0 */
#include "exec.h"
#include <reliefnt/time.h>

#define QUERY_WAIT_TICKS (5 * RELIEFNT_TICK_HZ)

/* END_QUERY can leave result DMA pending after the command fence. Keep the
 * latest result BO and its device mapping resident. When switching result
 * storage, use the dummy occlusion wait described by Linux vmwgfx_cmd.c.
 * Both the old BO and context then survive through the switching fence. */
static struct {
    struct svga_context *context; /* weak, cleared before destroying the ID */
    struct svga_buffer *buffer;
    struct svga_region *dummy[2];
    uint64_t submitted_tick;
    uint32_t fence;
    enum { QUERY_DIRTY, QUERY_WAITING, QUERY_COMPLETE } state;
} query;

struct svga_query_barrier {
    SVGA3dCmdHeader header;
    union {
        SVGA3dCmdWaitForQuery host;
        SVGA3dCmdWaitForGBQuery gb;
    } body;
};

_Static_assert(sizeof(SVGA3dCmdWaitForQuery) == sizeof(SVGA3dCmdWaitForGBQuery),
               "query commands use one fixed wire size");

static struct svga_query_barrier make_barrier(const struct svga_context *context)
{
    bool gb = context->type == SVGA_CONTEXT_GB;
    uint32_t region = svga_region_id(query.dummy[gb]);
    struct svga_query_barrier barrier = {
        .header = {gb ? SVGA_3D_CMD_WAIT_FOR_GB_QUERY : SVGA_3D_CMD_WAIT_FOR_QUERY,
                   sizeof(barrier.body)},
    };
    if (gb) {
        barrier.body.gb = (SVGA3dCmdWaitForGBQuery){.cid = context->object.id,
            .type = SVGA3D_QUERYTYPE_OCCLUSION, .mobid = region};
    } else {
        barrier.body.host = (SVGA3dCmdWaitForQuery){.cid = context->object.id,
            .type = SVGA3D_QUERYTYPE_OCCLUSION, .guestResult = {.gmrId = region}};
    }
    return barrier;
}

static int prepare_dummy(const struct svga_context *context)
{
    bool gb = context->type == SVGA_CONTEXT_GB;
    struct svga_region **region = &query.dummy[gb];
    if (*region)
        return 0;
    int ret = svga_region_create(gb ? SVGA_REGION_MOB : SVGA_REGION_GMR,
                                 SVGA_PAGE_SIZE, region);
    if (ret)
        return ret;
    SVGA3dQueryResult *result = svga_region_pixels(*region);
    *result = (SVGA3dQueryResult){.totalSize = sizeof(*result),
        .state = SVGA3D_QUERYSTATE_PENDING, .result32 = 0xff};
    return 0;
}

static void release_dummies(void)
{
    for (unsigned i = 0; i < 2; ++i) {
        svga_region_release(query.dummy[i]);
        query.dummy[i] = NULL;
    }
}

/* Stage a result BO switch. A repeated END to the same context and BO adds
 * no command. An aborted submission leaves the committed pin in place. */
int svga_query_end(struct svga_batch *batch, struct svga_context *context,
                    struct svga_buffer *buffer)
{
    if (drm_bo_size(svga_buffer_bo(buffer)) > 4 * SVGA_PAGE_SIZE)
        return -EINVAL;
    struct svga_context *old_context = batch->query_context ? batch->query_context : query.context;
    struct svga_buffer *old_buffer = batch->query_context ? batch->query_buffer : query.buffer;
    int ret = prepare_dummy(context);
    if (ret)
        return ret;
    if (old_context && (old_context != context || old_buffer != buffer)) {
        if (old_context->object.validation_serial != batch->serial) {
            ret = svga_batch_pin_object(batch, &old_context->object);
            if (ret)
                return ret;
        }
        ret = svga_batch_bo(batch, svga_buffer_bo(old_buffer), NULL);
        if (ret)
            return ret;
        struct svga_query_barrier barrier = make_barrier(old_context);
        ret = svga_batch_query_wait(batch, barrier.header.id, &barrier.body, barrier.header.size);
        if (ret)
            return ret;
    }
    batch->query_context = context;
    batch->query_buffer = buffer;
    return 0;
}

void svga_query_finish(const struct svga_batch *batch, bool commit)
{
    if (commit && batch->query_context) {
        svga_buffer_get(batch->query_buffer);
        svga_buffer_put(query.buffer);
        query.context = batch->query_context;
        query.buffer = batch->query_buffer;
        query.state = QUERY_DIRTY;
    } else if (!query.context) {
        release_dummies();
    }
}

static int queue_barrier(bool nowait)
{
    struct svga_query_barrier barrier = make_barrier(query.context);
    int ret = nowait ? svga_submit_try_fenced(&barrier, sizeof(barrier), &query.fence) :
                      svga_submit_fenced(&barrier, sizeof(barrier), &query.fence);
    if (!ret) {
        query.submitted_tick = time_ticks();
        query.state = QUERY_WAITING;
    }
    return ret;
}

int svga_query_poll(const struct drm_bo *bo, bool nowait)
{
    if (!query.context || bo != svga_buffer_bo(query.buffer) || query.state == QUERY_COMPLETE)
        return 1;
    if (query.state == QUERY_DIRTY) {
        int ret = queue_barrier(nowait);
        if (ret)
            return ret;
    }
    int ret = svga_submit_poll(query.fence);
    if (ret == 1) {
        query.state = QUERY_COMPLETE;
    } else if (!ret && time_ticks() - query.submitted_tick >= QUERY_WAIT_TICKS) {
        svga.fifo.fault = true;
        ret = -ETIMEDOUT;
    }
    return ret;
}

int svga_query_wait(const struct drm_bo *bo)
{
    if (!query.context || bo != svga_buffer_bo(query.buffer))
        return 0;
    int ret = query.state == QUERY_DIRTY ? queue_barrier(false) : 0;
    if (!ret && query.state == QUERY_WAITING)
        ret = svga_submit_wait(query.fence);
    if (!ret)
        query.state = QUERY_COMPLETE;
    return ret;
}

void svga_query_release(const struct svga_context *context)
{
    if (query.context != context)
        return;
    int ret = svga_query_wait(svga_buffer_bo(query.buffer));
    if (ret)
        svga.fifo.fault = true;
    /* Dropping the pin on failure quarantines its published mappings. */
    query.context = NULL;
    svga_buffer_put(query.buffer);
    query.buffer = NULL;
    release_dummies();
}
