/* SPDX-License-Identifier: Apache-2.0 */
#include "context.h"

#define BODY(type, packet) ((type *)(packet)->body)
#define FIXED(type, packet) ((packet)->bytes == sizeof(type))
#define COMMAND_HEADER(packet) ((SVGA3dCmdHeader *)(packet)->body - 1)

static int legacy_context(struct svga_batch *batch, void *body, struct svga_context **out)
{
    uint32_t *cid = body;
    int ret = svga_batch_context(batch, cid, out);
    return !ret && (*out)->type == SVGA_CONTEXT_DX ? -EINVAL : ret;
}

static bool box_fits(const SVGA3dSize *size, uint32_t x, uint32_t y, uint32_t z,
                     uint32_t w, uint32_t h, uint32_t d)
{
    return w && h && d && (uint64_t)x + w <= size->width &&
           (uint64_t)y + h <= size->height && (uint64_t)z + d <= size->depth;
}

static bool copy_fits(const SVGA3dSize *src, const SVGA3dSize *dst, const SVGA3dCopyBox *box)
{
    return box_fits(src, box->srcx, box->srcy, box->srcz, box->w, box->h, box->d) &&
           box_fits(dst, box->x, box->y, box->z, box->w, box->h, box->d);
}

static int surface_copy(struct svga_batch *batch, const struct svga_packet *p)
{
    int count = svga_array_count(p->bytes, sizeof(SVGA3dCmdSurfaceCopy), sizeof(SVGA3dCopyBox), INT32_MAX);
    if (count < 0)
        return -EINVAL;
    SVGA3dCmdSurfaceCopy *cmd = p->body;
    struct svga_surface *destination;
    SVGA3dSize src, dst;
    int ret = svga_batch_image(batch, &cmd->src, false, NULL, &src);
    if (!ret)
        ret = svga_batch_image(batch, &cmd->dest, false, &destination, &dst);
    if (ret)
        return ret;
    svga_batch_surface_written(destination);
    const SVGA3dCopyBox *boxes = (const void *)(cmd + 1);
    for (int i = 0; i < count; ++i)
        if (!copy_fits(&src, &dst, &boxes[i]))
            return -EINVAL;
    return 0;
}

static int stretch_copy(struct svga_batch *batch, const struct svga_packet *p)
{
    if (!FIXED(SVGA3dCmdSurfaceStretchBlt, p))
        return -EINVAL;
    SVGA3dCmdSurfaceStretchBlt *cmd = p->body;
    struct svga_surface *destination;
    SVGA3dSize src, dst;
    int ret = svga_batch_image(batch, &cmd->src, false, NULL, &src);
    if (!ret)
        ret = svga_batch_image(batch, &cmd->dest, false, &destination, &dst);
    if (ret)
        return ret;
    svga_batch_surface_written(destination);
    const SVGA3dBox a = cmd->boxSrc, b = cmd->boxDest;
    return box_fits(&src, a.x, a.y, a.z, a.w, a.h, a.d) &&
           box_fits(&dst, b.x, b.y, b.z, b.w, b.h, b.d) ? 0 : -EINVAL;
}

static int surface_dma(struct svga_batch *batch, const struct svga_packet *p)
{
    int count = svga_array_count(p->bytes, sizeof(SVGA3dCmdSurfaceDMA) + sizeof(SVGA3dCmdSurfaceDMASuffix),
                                 sizeof(SVGA3dCopyBox), INT32_MAX);
    if (count < 0)
        return -EINVAL;
    SVGA3dCmdSurfaceDMA *cmd = p->body;
    SVGA3dCmdSurfaceDMASuffix *suffix = (SVGA3dCmdSurfaceDMASuffix *)p->next - 1;
    if (suffix->suffixSize != sizeof(*suffix) || suffix->flags.reserved ||
        (cmd->transfer != SVGA3D_WRITE_HOST_VRAM && cmd->transfer != SVGA3D_READ_HOST_VRAM))
        return -EINVAL;
    struct svga_buffer *buffer;
    int ret = svga_batch_handle(batch, cmd->guest.ptr.gmrId, &buffer);
    if (ret)
        return ret;
    uint64_t bytes = drm_bo_size(svga_buffer_bo(buffer));
    if (cmd->guest.ptr.offset > bytes)
        return -EINVAL;
    bytes -= cmd->guest.ptr.offset;
    if (suffix->maximumOffset > bytes)
        suffix->maximumOffset = bytes;
    struct svga_surface *surface;
    SVGA3dSize size;
    ret = svga_batch_image(batch, &cmd->host, false, &surface, &size);
    if (ret)
        return ret;
    const SVGA3dCopyBox *boxes = (const void *)(cmd + 1);
    for (int i = 0; i < count; ++i) {
        ret = svga_surface_dma_check(surface, &size, &boxes[i], cmd->guest.pitch, suffix->maximumOffset);
        if (ret)
            return ret;
    }
    uint32_t id;
    ret = svga_buffer_map(buffer, SVGA_REGION_GMR, &id);
    if (!ret) {
        cmd->guest.ptr.gmrId = id;
        if (cmd->transfer == SVGA3D_WRITE_HOST_VRAM)
            svga_batch_surface_written(surface);
    }
    return ret;
}

static int surface_array(struct svga_batch *batch, SVGA3dArray *array, bool optional)
{
    struct svga_surface *surface;
    uint32_t id = array->surfaceId;
    int ret = svga_batch_surface(batch, &id, optional, &surface);
    if (ret)
        return ret;
    if (surface && !svga_buffer_fits(surface->bytes, array->offset, array->stride))
        return -EINVAL;
    array->surfaceId = id;
    return 0;
}

static int draw(struct svga_batch *batch, const struct svga_packet *p)
{
    if (p->bytes < sizeof(SVGA3dCmdDrawPrimitives))
        return -EINVAL;
    SVGA3dCmdDrawPrimitives *cmd = p->body;
    if (cmd->numVertexDecls > SVGA3D_MAX_VERTEX_ARRAYS || cmd->numRanges > SVGA3D_MAX_DRAW_PRIMITIVE_RANGES)
        return -EINVAL;
    uint32_t expected = sizeof(*cmd) + cmd->numVertexDecls * sizeof(SVGA3dVertexDecl) +
                        cmd->numRanges * sizeof(SVGA3dPrimitiveRange);
    /* The optional SVGA3dVertexDivisor array is sized by numVertexDecls. */
    if (p->bytes != expected && p->bytes != expected + cmd->numVertexDecls * sizeof(SVGA3dVertexDivisor))
        return -EINVAL;
    struct svga_context *context;
    int ret = legacy_context(batch, cmd, &context);
    if (ret)
        return ret;
    SVGA3dVertexDecl *decl = (void *)(cmd + 1);
    for (unsigned i = 0; i < cmd->numVertexDecls; ++i) {
        ret = surface_array(batch, &decl[i].array, false);
        if (ret)
            return ret;
    }
    SVGA3dPrimitiveRange *range = (void *)(decl + cmd->numVertexDecls);
    for (unsigned i = 0; i < cmd->numRanges; ++i) {
        if (!range[i].primType || range[i].primType >= SVGA3D_PRIMITIVE_MAX ||
            (range[i].indexArray.surfaceId != SVGA3D_INVALID_ID &&
             range[i].indexWidth != 2 && range[i].indexWidth != 4))
            return -EINVAL;
        ret = surface_array(batch, &range[i].indexArray, true);
        if (ret)
            return ret;
    }
    return 0;
}

static int texture_state(struct svga_batch *batch, const struct svga_packet *p)
{
    int count = svga_array_count(p->bytes, sizeof(SVGA3dCmdSetTextureState), sizeof(SVGA3dTextureState), INT32_MAX);
    if (count < 0)
        return -EINVAL;
    struct svga_context *context;
    int ret = legacy_context(batch, p->body, &context);
    if (ret)
        return ret;
    SVGA3dTextureState *state = (void *)(BODY(SVGA3dCmdSetTextureState, p) + 1);
    for (int i = 0; i < count; ++i) {
        if (state[i].stage >= SVGA3D_NUM_TEXTURE_UNITS || state[i].name >= SVGA3D_TS_MAX)
            return -EINVAL;
        if (state[i].name != SVGA3D_TS_BIND_TEXTURE)
            continue;
        uint32_t id = state[i].value;
        struct svga_surface *surface;
        ret = svga_batch_surface(batch, &id, true, &surface);
        if (!ret)
            ret = svga_context_bind_object(batch, context, SVGA_BIND_LEGACY_TEXTURE,
                                          state[i].stage, surface ? &surface->object : NULL);
        if (ret)
            return ret;
        state[i].value = id;
    }
    return 0;
}

static int legacy_shader(struct svga_batch *batch, const struct svga_packet *p)
{
    bool define = p->command == SVGA_3D_CMD_SHADER_DEFINE;
    bool destroy = p->command == SVGA_3D_CMD_SHADER_DESTROY;
    uint32_t *words = p->body;
    if (define ? p->bytes <= sizeof(SVGA3dCmdDefineShader) : p->bytes != sizeof(SVGA3dCmdSetShader))
        return -EINVAL;
    /* SET_SHADER uses type before shid. DEFINE/DESTROY use shid before type. */
    uint32_t id = words[define || destroy ? 1 : 2];
    uint32_t type = words[define || destroy ? 2 : 1];
    if (type < SVGA3D_SHADERTYPE_MIN || type >= SVGA3D_SHADERTYPE_PREDX_MAX)
        return -EINVAL;
    struct svga_context *context;
    int ret = legacy_context(batch, p->body, &context);
    if (ret)
        return ret;
    uint32_t key = id * SVGA3D_NUM_SHADERTYPE_PREDX + type - SVGA3D_SHADERTYPE_MIN;
    const struct svga_context_value *entry = id < SVGA3D_MAX_SHADERIDS ?
        svga_context_lookup(context, SVGA_LEGACY_SHADER, key) : NULL;
    if (define || destroy) {
        if (id >= SVGA3D_MAX_SHADERIDS || define == (entry != NULL))
            return -EINVAL;
        struct svga_shader *shader = NULL;
        if (define && context->type == SVGA_CONTEXT_GB) {
            ret = svga_shader_inline(type, words + 3, p->bytes - sizeof(SVGA3dCmdDefineShader), &shader);
            if (ret)
                return ret;
        }
        ret = svga_context_stage(batch, context, SVGA_LEGACY_SHADER, key,
            (struct svga_context_value){.defined = define, .object = shader ? &shader->object : NULL});
        if (shader)
            svga_object_put(&shader->object);
        if (!ret && context->type == SVGA_CONTEXT_GB)
            COMMAND_HEADER(p)->id = SVGA_3D_CMD_NOP;
        return ret;
    }
    struct svga_object *shader = entry ? entry->object : NULL;
    if (id != SVGA3D_INVALID_ID && !entry) {
        struct svga_shader *user = svga_shader_lookup(batch->file, id);
        if (context->type != SVGA_CONTEXT_GB || !user || user->type != type)
            return -ENOENT;
        shader = &user->object;
    }
    if (shader) {
        ret = svga_batch_object(batch, shader);
        if (ret)
            return ret;
        words[2] = shader->id;
    }
    return svga_context_bind_object(batch, context, SVGA_BIND_LEGACY_SHADER, type, shader);
}

static int query(struct svga_batch *batch, const struct svga_packet *p)
{
    uint32_t command = p->command;
    const bool begin = command == SVGA_3D_CMD_BEGIN_QUERY || command == SVGA_3D_CMD_BEGIN_GB_QUERY;
    const bool end = command == SVGA_3D_CMD_END_QUERY || command == SVGA_3D_CMD_END_GB_QUERY;
    const bool gb = command == SVGA_3D_CMD_BEGIN_GB_QUERY || command == SVGA_3D_CMD_END_GB_QUERY ||
                    command == SVGA_3D_CMD_WAIT_FOR_GB_QUERY;
    if (p->bytes != (begin ? sizeof(SVGA3dCmdBeginGBQuery) : sizeof(SVGA3dCmdEndGBQuery)) ||
        (gb && !svga_render_gb()))
        return -EINVAL;
    uint32_t *words = p->body;
    if (words[1] >= SVGA3D_QUERYTYPE_MAX)
        return -EINVAL;
    struct svga_context *context;
    int ret = legacy_context(batch, words, &context);
    if (ret)
        return ret;
    if (gb && context->type != SVGA_CONTEXT_GB)
        return -EINVAL;
    if (context->type == SVGA_CONTEXT_GB) {
        command = begin ? SVGA_3D_CMD_BEGIN_GB_QUERY : end ? SVGA_3D_CMD_END_GB_QUERY : SVGA_3D_CMD_WAIT_FOR_GB_QUERY;
        COMMAND_HEADER(p)->id = command;
    }
    if (begin)
        return 0;
    struct svga_buffer *buffer;
    ret = svga_batch_handle(batch, words[2], &buffer);
    if (ret)
        return ret;
    if ((words[3] & 3) || !svga_buffer_fits(drm_bo_size(svga_buffer_bo(buffer)), words[3], sizeof(SVGA3dQueryResult)))
        return -EINVAL;
    bool mob = context->type == SVGA_CONTEXT_GB;
    ret = svga_buffer_map(buffer, mob ? SVGA_REGION_MOB : SVGA_REGION_GMR, &words[2]);
    if (!ret && end)
        ret = svga_query_end(batch, context, buffer);
    return ret;
}

static int gb_surface(struct svga_batch *batch, const struct svga_packet *p)
{
    if (!svga_render_gb())
        return -EINVAL;
    switch (p->command) {
    case SVGA_3D_CMD_BIND_GB_SURFACE: {
        if (!FIXED(SVGA3dCmdBindGBSurface, p))
            return -EINVAL;
        uint32_t *words = p->body;
        struct svga_surface *surface;
        int ret = svga_batch_surface(batch, words, false, &surface);
        if (ret)
            return ret;
        if (!surface->gb)
            return -EINVAL;
        struct svga_buffer *buffer = NULL;
        if (words[1] != SVGA3D_INVALID_ID) {
            ret = svga_batch_handle(batch, words[1], &buffer);
            if (ret)
                return ret;
            if (drm_bo_size(svga_buffer_bo(buffer)) < surface->bytes)
                return -EINVAL;
        }
        return svga_batch_rebind(batch, &surface->backing, surface, &surface->offset, buffer, 0, &words[1]);
    }
    case SVGA_3D_CMD_UPDATE_GB_SURFACE:
    case SVGA_3D_CMD_READBACK_GB_SURFACE:
    case SVGA_3D_CMD_INVALIDATE_GB_SURFACE: {
        if (!FIXED(SVGA3dCmdUpdateGBSurface, p))
            return -EINVAL;
        struct svga_surface *surface;
        int ret = svga_batch_surface(batch, p->body, false, &surface);
        if (ret)
            return ret;
        if (!surface->gb)
            return -EINVAL;
        svga_batch_surface_cleaned(surface);
        return 0;
    }
    default: {
        bool update = p->command == SVGA_3D_CMD_UPDATE_GB_IMAGE;
        if (p->bytes != (update ? sizeof(SVGA3dCmdUpdateGBImage) : sizeof(SVGA3dCmdReadbackGBImage)))
            return -EINVAL;
        struct svga_surface *surface;
        SVGA3dSize size;
        int ret = svga_batch_image(batch, p->body, false, &surface, &size);
        if (ret)
            return ret;
        if (!surface->gb)
            return -EINVAL;
        if (update) {
            const SVGA3dBox box = BODY(SVGA3dCmdUpdateGBImage, p)->box;
            if (!box_fits(&size, box.x, box.y, box.z, box.w, box.h, box.d))
                return -EINVAL;
        }
        return 0;
    }
    }
}

static int gb_shader(struct svga_batch *batch, const struct svga_packet *p)
{
    if (!svga_render_gb() || !FIXED(SVGA3dCmdBindGBShader, p))
        return -EINVAL;
    uint32_t *words = p->body;
    struct svga_shader *shader = svga_shader_lookup(batch->file, words[0]);
    if (!shader)
        return -ENOENT;
    int ret = svga_batch_object(batch, &shader->object);
    if (ret)
        return ret;
    struct svga_buffer *buffer;
    ret = svga_batch_handle(batch, words[1], &buffer);
    if (ret)
        return ret;
    if ((words[2] & 3) || !svga_buffer_fits(drm_bo_size(svga_buffer_bo(buffer)), words[2], shader->bytes))
        return -EINVAL;
    words[0] = shader->object.id;
    return svga_batch_rebind(batch, &shader->backing, NULL, &shader->offset, buffer, words[2], &words[1]);
}

static int context_only(struct svga_batch *batch, const struct svga_packet *p,
                        uint32_t prefix, uint32_t stride)
{
    if (stride ? svga_array_count(p->bytes, prefix, stride, INT32_MAX) < 0 : p->bytes != prefix)
        return -EINVAL;
    struct svga_context *context;
    return legacy_context(batch, p->body, &context);
}

int svga_validate_legacy(struct svga_batch *batch, const struct svga_packet *p)
{
    switch (p->command) {
    case SVGA_3D_CMD_SURFACE_COPY: return surface_copy(batch, p);
    case SVGA_3D_CMD_SURFACE_STRETCHBLT: return stretch_copy(batch, p);
    case SVGA_3D_CMD_SURFACE_DMA: return surface_dma(batch, p);
    case SVGA_3D_CMD_INTRA_SURFACE_COPY: {
        if (!(svga.caps2 & SVGA_CAP2_INTRA_SURFACE_COPY) || !FIXED(SVGA3dCmdIntraSurfaceCopy, p))
            return -EINVAL;
        SVGA3dCmdIntraSurfaceCopy *cmd = p->body;
        SVGA3dSize size;
        struct svga_surface *surface;
        int ret = svga_batch_image(batch, &cmd->surface, false, &surface, &size);
        if (ret)
            return ret;
        if (!copy_fits(&size, &size, &cmd->box))
            return -EINVAL;
        svga_batch_surface_written(surface);
        return 0;
    }
    case SVGA_3D_CMD_SETRENDERTARGET: {
        if (!FIXED(SVGA3dCmdSetRenderTarget, p))
            return -EINVAL;
        SVGA3dCmdSetRenderTarget *cmd = p->body;
        if (cmd->type >= SVGA3D_RT_MAX)
            return -EINVAL;
        struct svga_context *context;
        int ret = legacy_context(batch, cmd, &context);
        if (ret)
            return ret;
        struct svga_surface *surface;
        SVGA3dSize size;
        ret = svga_batch_image(batch, &cmd->target, true, &surface, &size);
        if (!ret)
            ret = svga_context_bind_object(batch, context, SVGA_BIND_LEGACY_RT, cmd->type,
                                           surface ? &surface->object : NULL);
        return ret;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: return texture_state(batch, p);
    case SVGA_3D_CMD_DRAW_PRIMITIVES: return draw(batch, p);
    case SVGA_3D_CMD_SHADER_DEFINE:
    case SVGA_3D_CMD_SHADER_DESTROY:
    case SVGA_3D_CMD_SET_SHADER: return legacy_shader(batch, p);
    case SVGA_3D_CMD_SET_SHADER_CONST:
    case SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE: {
        if (p->command == SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE && !svga_render_gb())
            return -EINVAL;
        /* Both forms accept consecutive four-word registers in one packet. */
        int count = svga_array_count(p->bytes, sizeof(SVGA3dCmdSetGBShaderConstInline),
                                     sizeof(((SVGA3dCmdSetShaderConst *)0)->values), 8192);
        if (count <= 0)
            return -EINVAL;
        const SVGA3dCmdSetGBShaderConstInline *cmd = p->body;
        if (cmd->shaderType < SVGA3D_SHADERTYPE_MIN || cmd->shaderType >= SVGA3D_SHADERTYPE_PREDX_MAX)
            return -EINVAL;
        if (svga_render_gb())
            COMMAND_HEADER(p)->id = SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE;
        return context_only(batch, p, p->bytes, 0);
    }
    case SVGA_3D_CMD_BEGIN_QUERY:
    case SVGA_3D_CMD_END_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_QUERY:
    case SVGA_3D_CMD_BEGIN_GB_QUERY:
    case SVGA_3D_CMD_END_GB_QUERY:
    case SVGA_3D_CMD_WAIT_FOR_GB_QUERY: return query(batch, p);
    case SVGA_3D_CMD_BIND_GB_SURFACE:
    case SVGA_3D_CMD_UPDATE_GB_IMAGE:
    case SVGA_3D_CMD_UPDATE_GB_SURFACE:
    case SVGA_3D_CMD_READBACK_GB_IMAGE:
    case SVGA_3D_CMD_READBACK_GB_SURFACE:
    case SVGA_3D_CMD_INVALIDATE_GB_IMAGE:
    case SVGA_3D_CMD_INVALIDATE_GB_SURFACE: return gb_surface(batch, p);
    case SVGA_3D_CMD_BIND_GB_SHADER: return gb_shader(batch, p);
    /* These commands carry no user resource handles. vmwgfx admits
     * PRESENT_READBACK in both modes and the NOP commands only with GB. */
    case SVGA_3D_CMD_PRESENT_READBACK: return 0;
    case SVGA_3D_CMD_NOP_ERROR:
    case SVGA_3D_CMD_NOP: return svga_render_gb() ? 0 : -EINVAL;
    case SVGA_3D_CMD_CLEAR: return context_only(batch, p, sizeof(SVGA3dCmdClear), sizeof(SVGA3dRect));
    case SVGA_3D_CMD_SETRENDERSTATE: return context_only(batch, p, sizeof(SVGA3dCmdSetRenderState), sizeof(SVGA3dRenderState));
    case SVGA_3D_CMD_SETTRANSFORM: return context_only(batch, p, sizeof(SVGA3dCmdSetTransform), 0);
    case SVGA_3D_CMD_SETZRANGE: return context_only(batch, p, sizeof(SVGA3dCmdSetZRange), 0);
    case SVGA_3D_CMD_SETMATERIAL: return context_only(batch, p, sizeof(SVGA3dCmdSetMaterial), 0);
    case SVGA_3D_CMD_SETLIGHTDATA: return context_only(batch, p, sizeof(SVGA3dCmdSetLightData), 0);
    case SVGA_3D_CMD_SETLIGHTENABLED: return context_only(batch, p, sizeof(SVGA3dCmdSetLightEnabled), 0);
    case SVGA_3D_CMD_SETVIEWPORT: return context_only(batch, p, sizeof(SVGA3dCmdSetViewport), 0);
    case SVGA_3D_CMD_SETCLIPPLANE: return context_only(batch, p, sizeof(SVGA3dCmdSetClipPlane), 0);
    case SVGA_3D_CMD_SETSCISSORRECT: return context_only(batch, p, sizeof(SVGA3dCmdSetScissorRect), 0);
    default: return -ENOTTY;
    }
}
