#include "device.h"

static int require_ready(void)
{
    return svga.available && svga.fifo_ready ? 0 : SVGA_ENODEV;
}

static int valid_sid(uint32_t id)
{
    return svga_surface_usable(id);
}

static int valid_cid(uint32_t id)
{
    return svga_context_usable(id);
}

static void context_init_bindings(struct svga_context *context)
{
    for (uint32_t i = 0; i < SVGA3D_RT_MAX; ++i)
        context->targets[i] = SVGA3D_INVALID_ID;
    for (uint32_t i = 0; i < 2; ++i)
        context->shaders[i] = UINT32_MAX;
    context->query_attachment = UINT32_MAX;
}

/* GB shader/query objects need separate translation. Only expose the v9
 * commands whose fixed-pipeline encoding is shared by the two backends. */
static bool gb_raw_supported(uint32_t command)
{
    switch (command) {
    case SVGA_3D_CMD_SURFACE_COPY:
    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
    case SVGA_3D_CMD_SETTRANSFORM:
    case SVGA_3D_CMD_SETZRANGE:
    case SVGA_3D_CMD_SETRENDERSTATE:
    case SVGA_3D_CMD_SETRENDERTARGET:
    case SVGA_3D_CMD_SETTEXTURESTATE:
    case SVGA_3D_CMD_SETMATERIAL:
    case SVGA_3D_CMD_SETLIGHTDATA:
    case SVGA_3D_CMD_SETLIGHTENABLED:
    case SVGA_3D_CMD_SETVIEWPORT:
    case SVGA_3D_CMD_SETCLIPPLANE:
    case SVGA_3D_CMD_CLEAR:
    case SVGA_3D_CMD_PRESENT:
    case SVGA_3D_CMD_DRAW_PRIMITIVES:
    case SVGA_3D_CMD_SETSCISSORRECT:
        return true;
    default: return false;
    }
}

static int surface_referenced(uint32_t sid)
{
    for (uint32_t cid = 0; cid < SVGA_CONTEXTS; ++cid) {
        if (!svga.contexts[cid].handle) continue;
        for (uint32_t target = 0; target < SVGA3D_RT_MAX; ++target)
            if (svga.contexts[cid].targets[target] == sid) return 1;
        for (uint32_t stage = 0; stage < 32; ++stage)
            if (svga.contexts[cid].textures[stage] == sid) return 1;
    }
    return 0;
}

static int managed_command(uint32_t command)
{
    switch (command) {
    case SVGA_3D_CMD_SURFACE_DEFINE:
    case SVGA_3D_CMD_SURFACE_DEFINE_V2:
    case SVGA_3D_CMD_SURFACE_DESTROY:
    case SVGA_3D_CMD_SURFACE_DMA:
    case SVGA_3D_CMD_CONTEXT_DEFINE:
    case SVGA_3D_CMD_CONTEXT_DESTROY:
        return 1;
    default:
        return 0;
    }
}

int svga_format(uint32_t format, uint32_t *bw, uint32_t *bh,
                uint32_t *bytes, uint32_t *cap)
{
    if (!bw || !bh || !bytes || !cap) return SVGA_EINVAL;
    *bw = *bh = 1; *bytes = 4; *cap = 0;
    switch (format) {
    case SVGA3D_X8R8G8B8:
        *cap = SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8;
        return 0;
    case SVGA3D_A8R8G8B8:
        *cap = SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8;
        return 0;
    case SVGA3D_ALPHA8:
        *bytes = 1;
        *cap = SVGA3D_DEVCAP_SURFACEFMT_ALPHA8;
        return 0;
    case SVGA3D_Z_D16:
        *bytes = 2; *cap = SVGA3D_DEVCAP_SURFACEFMT_Z_D16; return 0;
    case SVGA3D_Z_D24S8:
        *cap = SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8; return 0;
    case SVGA3D_BUFFER:
        *bytes = 1;
        *cap = SVGA3D_DEVCAP_3D;
        return 0;
    default:
        return SVGA_ENOTSUP;
    }
}

static int format_operations_locked(uint32_t format, uint32_t *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    uint32_t cap, implemented;
    switch (format) {
    case SVGA3D_X8R8G8B8:
        cap = SVGA3D_DEVCAP_SURFACEFMT_X8R8G8B8;
        implemented = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                      SVGA3DFORMAT_OP_CUBETEXTURE |
                      SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                      SVGA3DFORMAT_OP_DISPLAYMODE | SVGA3DFORMAT_OP_SRGBREAD;
        break;
    case SVGA3D_A8R8G8B8:
        cap = SVGA3D_DEVCAP_SURFACEFMT_A8R8G8B8;
        implemented = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_VOLUMETEXTURE |
                      SVGA3DFORMAT_OP_CUBETEXTURE |
                      SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET |
                      SVGA3DFORMAT_OP_SRGBREAD;
        break;
    case SVGA3D_ALPHA8:
        cap = SVGA3D_DEVCAP_SURFACEFMT_ALPHA8;
        implemented = SVGA3DFORMAT_OP_TEXTURE;
        break;
    case SVGA3D_Z_D16:
        cap = SVGA3D_DEVCAP_SURFACEFMT_Z_D16;
        implemented = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL;
        break;
    case SVGA3D_Z_D24S8:
        cap = SVGA3D_DEVCAP_SURFACEFMT_Z_D24S8;
        implemented = SVGA3DFORMAT_OP_TEXTURE | SVGA3DFORMAT_OP_ZSTENCIL;
        break;
    case SVGA3D_BUFFER:
        return 0;
    default:
        return SVGA_ENOTSUP;
    }
    if (cap >= SVGA3D_DEVCAP_MAX || !svga.cap_valid[cap]) return SVGA_ENOTSUP;
    /* Positive operations are intersected with local support, but the three
     * negative format restrictions must survive for Mesa's format policy. */
    *out = (svga.cap_values[cap] & implemented) |
           (svga.cap_values[cap] &
            (SVGA3DFORMAT_OP_NOFILTER |
             SVGA3DFORMAT_OP_NOALPHABLEND |
             SVGA3DFORMAT_OP_NOTEXCOORDWRAPNORMIP));
    return 0;
}

int svga3d_format_operations(uint32_t format, uint32_t *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = format_operations_locked(format, out);
    svga_unlock(flags);
    return ret;
}

static uint64_t surface_size(const struct svga_surface_desc *d,
                             uint32_t bw, uint32_t bh, uint32_t bpp)
{
    uint64_t width = ((uint64_t)d->width + bw - 1u) / bw;
    uint64_t height = ((uint64_t)d->height + bh - 1u) / bh;
    uint64_t depth = d->depth ? d->depth : 1u;
    uint64_t levels = d->mip_levels ? d->mip_levels : 1u;
    uint64_t total = 0;
    for (uint64_t level = 0; level < levels; ++level) {
        uint64_t area;
        uint64_t volume;
        if (width && height > UINT64_MAX / width) return 0;
        area = width * height;
        if (area && depth > UINT64_MAX / area) return 0;
        volume = area * depth;
        if (volume && bpp > UINT64_MAX / volume) return 0;
        if (total > UINT64_MAX - volume * bpp) return 0;
        total += volume * bpp;
        if (width > 1) width >>= 1;
        if (height > 1) height >>= 1;
        if (depth > 1) depth >>= 1;
    }
    uint64_t faces = (d->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u;
    if (total > UINT64_MAX / faces) return 0;
    return total * faces;
}

static int surface_validate_locked(const struct svga_surface_desc *desc,
                                   uint32_t *bw, uint32_t *bh,
                                   uint32_t *bpp, uint64_t *bytes)
{
    if (!desc || !bw || !bh || !bpp || !bytes || !desc->width || !desc->height)
        return SVGA_EINVAL;
    int ret = require_ready();
    uint32_t cap;
    if (ret || (ret = svga_format(desc->format, bw, bh, bpp, &cap))) return ret;
    /* Stream-output buffers need a DXSO state tail that this VGPU9 backend
     * does not allocate or support. Reject the role before surface sizing. */
    if (desc->flags & SVGA3D_SURFACE_BIND_STREAM_OUTPUT) return SVGA_ENOTSUP;
    if (desc->mip_levels > SVGA_MAX_MIPS || desc->depth > 256u)
        return SVGA_EINVAL;
    uint32_t levels = desc->mip_levels ? desc->mip_levels : 1u;
    uint32_t depth = desc->depth ? desc->depth : 1u;
    if ((desc->flags & SVGA3D_SURFACE_CUBEMAP) &&
        (desc->width != desc->height || depth != 1u))
        return SVGA_EINVAL;
    uint32_t largest = desc->width > desc->height ? desc->width : desc->height;
    if (depth > largest) largest = depth;
    uint32_t max_levels = 1;
    while (largest > 1u) { largest >>= 1; ++max_levels; }
    if (levels > max_levels) return SVGA_EINVAL;

    if (desc->format != SVGA3D_BUFFER) {
        uint32_t max_width = svga.cap_valid[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH] ?
                             svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH] : UINT32_MAX;
        uint32_t max_height = svga.cap_valid[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT] ?
                              svga.cap_values[SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT] : UINT32_MAX;
        uint32_t max_depth = svga.cap_valid[SVGA3D_DEVCAP_MAX_VOLUME_EXTENT] ?
                             svga.cap_values[SVGA3D_DEVCAP_MAX_VOLUME_EXTENT] : UINT32_MAX;
        if (desc->width > max_width || desc->height > max_height ||
            desc->depth > max_depth)
            return SVGA_EINVAL;
    }
    if (!svga.cap_valid[cap] || !svga.cap_values[cap]) return SVGA_ENOTSUP;
    if (desc->format == SVGA3D_BUFFER) {
        /* Mesa's legacy buffer resources use these bind bits to identify
         * their buffer roles. Shader-resource reads also apply to buffers;
         * image-only render/depth uses do not. */
        if ((desc->flags & (SVGA3D_SURFACE_HINT_RENDERTARGET |
                            SVGA3D_SURFACE_HINT_TEXTURE |
                            SVGA3D_SURFACE_HINT_DEPTHSTENCIL |
                            SVGA3D_SURFACE_CUBEMAP |
                            SVGA3D_SURFACE_BIND_RENDER_TARGET |
                            SVGA3D_SURFACE_BIND_DEPTH_STENCIL)) || depth > 1u)
            return SVGA_ENOTSUP;
    } else {
        const uint32_t buffer_bindings =
            SVGA3D_SURFACE_BIND_VERTEX_BUFFER |
            SVGA3D_SURFACE_BIND_INDEX_BUFFER |
            SVGA3D_SURFACE_BIND_CONSTANT_BUFFER;
        if (desc->flags & buffer_bindings) return SVGA_ENOTSUP;
        uint32_t operations;
        ret = format_operations_locked(desc->format, &operations);
        if (ret) return ret;
        uint32_t required = 0;
        if (desc->flags & (SVGA3D_SURFACE_HINT_RENDERTARGET |
                           SVGA3D_SURFACE_BIND_RENDER_TARGET))
            required |= SVGA3DFORMAT_OP_OFFSCREEN_RENDERTARGET;
        if (desc->flags & (SVGA3D_SURFACE_HINT_TEXTURE |
                           SVGA3D_SURFACE_BIND_SHADER_RESOURCE))
            required |= SVGA3DFORMAT_OP_TEXTURE;
        if (desc->flags & (SVGA3D_SURFACE_HINT_DEPTHSTENCIL |
                           SVGA3D_SURFACE_BIND_DEPTH_STENCIL))
            required |= SVGA3DFORMAT_OP_ZSTENCIL;
        if (desc->flags & SVGA3D_SURFACE_CUBEMAP)
            required |= SVGA3DFORMAT_OP_CUBETEXTURE;
        if (depth > 1u) required |= SVGA3DFORMAT_OP_VOLUMETEXTURE;
        if ((operations & required) != required) return SVGA_ENOTSUP;
    }
    *bytes = surface_size(desc, *bw, *bh, *bpp);
    if (!*bytes || *bytes > svga.surface_limit_bytes ||
        *bytes > SVGA_MAX_SURFACE_BYTES || *bytes > UINT32_MAX)
        return SVGA_EINVAL;
    if (svga.surface_bytes > svga.surface_limit_bytes - *bytes) return SVGA_ENOMEM;
    return 0;
}

static int lifecycle_packet(uint32_t *words, uint32_t capacity_words,
                            uint32_t *used_words, uint32_t id,
                            const void *payload, uint32_t bytes)
{
    if (!words || !used_words || (bytes & 3u) || (bytes && !payload))
        return SVGA_EINVAL;
    uint32_t packet_words = 2u + bytes / 4u;
    if (*used_words > capacity_words || packet_words > capacity_words - *used_words)
        return SVGA_ENOMEM;
    uint32_t *dst = words + *used_words;
    dst[0] = id;
    dst[1] = bytes;
    for (uint32_t i = 0; i < bytes / 4; ++i)
        __builtin_memcpy(&dst[2 + i], (const uint8_t *)payload + i * 4, 4);
    *used_words += packet_words;
    return 0;
}

int svga_context_create_locked(svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    int ret = require_ready();
    if (ret) goto done;
    uint32_t id;
    for (id = 1; id < svga.context_limit && svga.contexts[id].handle; ++id) {}
    if (id >= svga.context_limit) { ret = SVGA_ENOMEM; goto done; }
    svga_handle handle = svga_new_handle(SVGA_KIND_CONTEXT, id);
    if (!handle) { ret = SVGA_ENOMEM; goto done; }
    if (svga.gb_active) {
        svga.contexts[id].handle = handle;
        context_init_bindings(&svga.contexts[id]);
        ret = svga_gb_context_create_locked(id);
        if (ret) svga.contexts[id].gb.retiring = true;
        if (!ret || svga_context_destroy_locked(id)) *out = handle;
        goto done;
    }
    SVGA3dCmdDefineContext cmd = {id};
    ret = svga_command_locked(SVGA_3D_CMD_CONTEXT_DEFINE, &cmd, sizeof(cmd));
    if (!ret) {
        svga.contexts[id].handle = handle;
        context_init_bindings(&svga.contexts[id]);
        *out = handle;
    }
done:
    return ret;
}

int svga3d_context_create(svga_handle *out)
{
    uint64_t flags = svga_lock();
    int ret = svga_context_create_locked(out);
    svga_unlock(flags);
    return ret;
}

int svga_context_destroy_locked(uint32_t id)
{
    if (id >= SVGA_CONTEXTS || !svga.contexts[id].handle) return SVGA_EINVAL;
    if (svga.contexts[id].batch_state_owner) return SVGA_EBUSY;
    if (svga.contexts[id].gb.retire_fence) {
        struct svga_context *context = &svga.contexts[id];
        if (!svga_fence_reached_locked(context->gb.retire_fence))
            return SVGA_EBUSY;
        for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i)
            if (svga.shaders[i].defined &&
                svga.shaders[i].context == context->handle)
                return SVGA_EBUSY;
        if (context->gb.mode_lease) {
            int ret = svga_mode_lease_release_locked();
            if (ret) return ret;
        }
        svga_gb_backing_free_locked(&context->gb.mob);
        *context = (struct svga_context){0};
        return 0;
    }
    if (svga_query_context_busy_locked(id)) return SVGA_EBUSY;
    if (svga.gb_active) svga.contexts[id].gb.retiring = true;
    int ret = svga_sync_locked();
    if (ret) return ret;
    if (svga.gb_active) {
        for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
            struct svga_shader *shader = &svga.shaders[i];
            if (!shader->defined || shader->context != svga.contexts[id].handle)
                continue;
            if (shader->reserved) return SVGA_EBUSY;
            if (shader->retiring) {
                if (!svga_fence_reached_locked(shader->retire_fence))
                    return SVGA_EBUSY;
                /* The reaper owns detaching and freeing shader backing in
                 * task context, after it drops the IRQ-saving device lock. */
                return SVGA_EBUSY;
            }
            uint32_t words[16] = {0}, used = 0;
            uint32_t type_index = shader->type - SVGA3D_SHADERTYPE_VS;
            if (type_index < 2 && svga.contexts[id].shaders[type_index] == i) {
                SVGA3dCmdSetShader unbind_context = {
                    id, (SVGA3dShaderType)shader->type, SVGA3D_INVALID_ID};
                ret = lifecycle_packet(words, 16, &used,
                    SVGA_3D_CMD_SET_SHADER, &unbind_context,
                    sizeof(unbind_context));
                if (ret) return ret;
            }
            SVGA3dCmdBindGBShader unbind_shader = {
                shader->host_id, SVGA3D_INVALID_ID, 0};
            SVGA3dCmdDestroyGBShader destroy_shader = {shader->host_id};
            ret = lifecycle_packet(words, 16, &used,
                SVGA_3D_CMD_BIND_GB_SHADER, &unbind_shader,
                sizeof(unbind_shader));
            if (!ret) ret = lifecycle_packet(words, 16, &used,
                SVGA_3D_CMD_DESTROY_GB_SHADER, &destroy_shader,
                sizeof(destroy_shader));
            if (!ret) ret = lifecycle_packet(words, 16, &used,
                SVGA_3D_CMD_DESTROY_GB_MOB, &shader->mobid,
                sizeof(shader->mobid));
            if (ret) return ret;
            svga_handle fence = 0;
            ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
            if (ret) return ret;
            shader->retiring = true;
            shader->retire_fence = fence;
            if (type_index < 2)
                svga.contexts[id].shaders[type_index] = UINT32_MAX;
            return SVGA_EBUSY;
        }
        ret = svga_gb_destroy_locked(&svga.contexts[id].gb, id, true);
        if (!ret && svga.contexts[id].gb.mode_lease)
            ret = svga_mode_lease_release_locked();
        if (!ret) svga.contexts[id] = (struct svga_context){0};
        return ret;
    }
    SVGA3dCmdDestroyContext cmd = {id};
    ret = svga_command_locked(SVGA_3D_CMD_CONTEXT_DESTROY, &cmd, sizeof(cmd));
    if (!ret && svga.contexts[id].gb.mode_lease)
        ret = svga_mode_lease_release_locked();
    if (!ret) svga.contexts[id] = (struct svga_context){0};
    return ret;
}

int svga3d_context_destroy(svga_handle context)
{
    for (uint32_t attempt = 0; attempt <= SVGA_GB_SHADER_SLOTS; ++attempt) {
        uint64_t flags = svga_lock();
        int id = svga_find_context(context);
        int ret = id < 0 ? SVGA_EINVAL :
                  svga_context_destroy_locked((uint32_t)id);
        svga_unlock(flags);
        if (ret != SVGA_EBUSY) return ret;

        uint32_t reaped = 0;
        (void)svga_backend_reap_retired(SVGA_GB_SHADER_SLOTS, &reaped);
        if (!reaped) return SVGA_EBUSY;
    }
    return SVGA_EBUSY;
}

int svga3d_context_try_create(svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int ret = require_ready();
    uint32_t id;
    for (id = 1; !ret && id < svga.context_limit && svga.contexts[id].handle; ++id) {}
    if (ret) goto done;
    if (id >= svga.context_limit) { ret = SVGA_ENOMEM; goto done; }
    svga_handle handle = svga_new_handle(SVGA_KIND_CONTEXT, id);
    if (!handle) { ret = SVGA_ENOMEM; goto done; }

    uint32_t words[32] = {0}, used = 0;
    struct svga_mob mob = {0};
    if (svga.gb_active) {
        ret = svga_gb_backing_alloc_locked(&mob, SVGA_GB_CONTEXT_BYTES);
        if (ret) goto done;
        SVGA3dCmdDefineGBMob define_mob = {id, mob.depth,
            svga_gb_backing_root(&mob), mob.bytes};
        SVGA3dCmdBindGBContext bind = {id, id, 0};
        ret = lifecycle_packet(words, 32, &used, SVGA_3D_CMD_DEFINE_GB_MOB,
                               &define_mob, sizeof(define_mob));
        if (!ret) ret = lifecycle_packet(words, 32, &used,
            SVGA_3D_CMD_DEFINE_GB_CONTEXT, &id, sizeof(id));
        if (!ret) ret = lifecycle_packet(words, 32, &used,
            SVGA_3D_CMD_BIND_GB_CONTEXT, &bind, sizeof(bind));
    } else {
        SVGA3dCmdDefineContext define = {id};
        ret = lifecycle_packet(words, 32, &used, SVGA_3D_CMD_CONTEXT_DEFINE,
                               &define, sizeof(define));
    }
    if (ret) {
        if (mob.phys) svga_gb_backing_free_locked(&mob);
        goto done;
    }
    ret = svga_mode_lease_acquire_locked();
    if (ret) {
        if (mob.phys) svga_gb_backing_free_locked(&mob);
        goto done;
    }
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
    if (ret) {
        (void)svga_mode_lease_release_locked();
        if (mob.phys) svga_gb_backing_free_locked(&mob);
        goto done;
    }
    if (mob.phys) mob.defined = true;
    struct svga_context context = {.handle = handle, .ready_fence = fence,
        .gb = {.mob = mob, .defined = svga.gb_active, .bound = svga.gb_active,
               .mode_lease = true}};
    context_init_bindings(&context);
    svga.contexts[id] = context;
    *out = handle;
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

int svga3d_context_try_destroy(svga_handle handle)
{
    uint32_t reaped = 0;
    (void)svga_backend_reap_retired(SVGA_GB_SHADER_SLOTS, &reaped);
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int id = svga_find_context(handle), ret = SVGA_EINVAL;
    if (id < 0) goto done;
    struct svga_context *context = &svga.contexts[id];
    if (context->batch_state_owner) { ret = SVGA_EBUSY; goto done; }
    ret = svga_query_prepare_context_retire_locked((uint32_t)id);
    if (ret) goto done;
    if (context->gb.retiring) {
        if (!svga_fence_reached_locked(context->gb.retire_fence)) {
            ret = SVGA_EBUSY;
            goto done;
        }
        for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
            struct svga_shader *shader = &svga.shaders[i];
            if (!shader->defined || shader->context != handle) continue;
            ret = SVGA_EBUSY;
            goto done;
        }
        if (context->gb.mode_lease) {
            ret = svga_mode_lease_release_locked();
            if (ret) goto done;
        }
        svga_gb_backing_free_locked(&context->gb.mob);
        *context = (struct svga_context){0};
        ret = 0;
        goto done;
    }
    ret = require_ready();
    if (ret) goto done;

    uint32_t words[SVGA_GB_SHADER_SLOTS * 16u + 32u] = {0}, used = 0;
    if (svga.gb_active) {
        for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
            struct svga_shader *shader = &svga.shaders[i];
            if (!shader->defined || shader->context != handle) continue;
            if (shader->retiring) {
                if (!svga_fence_reached_locked(shader->retire_fence)) {
                    ret = SVGA_EBUSY;
                    goto done;
                }
                ret = SVGA_EBUSY;
                goto done;
            }
            uint32_t type_index = shader->type - SVGA3D_SHADERTYPE_VS;
            if (type_index < 2 && context->shaders[type_index] == i) {
                SVGA3dCmdSetShader unbind_context = {(uint32_t)id,
                    (SVGA3dShaderType)shader->type, SVGA3D_INVALID_ID};
                ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
                    SVGA_3D_CMD_SET_SHADER, &unbind_context, sizeof(unbind_context));
                if (ret) goto done;
            }
            SVGA3dCmdBindGBShader unbind_shader = {shader->host_id,
                                                    SVGA3D_INVALID_ID, 0};
            SVGA3dCmdDestroyGBShader destroy_shader = {shader->host_id};
            ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
                SVGA_3D_CMD_BIND_GB_SHADER, &unbind_shader, sizeof(unbind_shader));
            if (!ret) ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
                SVGA_3D_CMD_DESTROY_GB_SHADER, &destroy_shader, sizeof(destroy_shader));
            if (!ret) ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
                SVGA_3D_CMD_DESTROY_GB_MOB, &shader->mobid, sizeof(shader->mobid));
            if (ret) goto done;
        }
    }
    if (context->gb.bound) {
        SVGA3dCmdBindGBContext unbind = {(uint32_t)id, SVGA3D_INVALID_ID, 0};
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
            SVGA_3D_CMD_BIND_GB_CONTEXT, &unbind, sizeof(unbind));
        if (ret) goto done;
    }
    if (context->gb.defined) {
        uint32_t object = (uint32_t)id;
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
            SVGA_3D_CMD_DESTROY_GB_CONTEXT, &object, sizeof(object));
        if (ret) goto done;
    }
    if (context->gb.mob.defined) {
        uint32_t mobid = (uint32_t)id;
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
            SVGA_3D_CMD_DESTROY_GB_MOB, &mobid, sizeof(mobid));
        if (ret) goto done;
    }
    if (!svga.gb_active) {
        SVGA3dCmdDestroyContext destroy = {(uint32_t)id};
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])), &used,
            SVGA_3D_CMD_CONTEXT_DESTROY, &destroy, sizeof(destroy));
        if (ret) goto done;
    }
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
    if (ret) goto done;
    context->gb.retiring = true;
    context->gb.retire_fence = fence;
    for (uint32_t i = 0; i < SVGA_GB_SHADER_SLOTS; ++i) {
        struct svga_shader *shader = &svga.shaders[i];
        if (shader->defined && shader->context == handle) {
            shader->retiring = true;
            shader->retire_fence = fence;
        }
    }
    ret = SVGA_EBUSY;
done:
    svga_unlock(flags);
    return ret;
}

int svga_surface_create_locked(const struct svga_surface_desc *desc,
                               svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    uint32_t bw, bh, bpp;
    uint64_t bytes;
    int ret = surface_validate_locked(desc, &bw, &bh, &bpp, &bytes);
    if (ret) return ret;
    uint32_t id;
    for (id = 1; id < svga.surface_limit && svga.surfaces[id].handle; ++id) {}
    if (id >= svga.surface_limit) { ret = SVGA_ENOMEM; goto done; }
    svga_handle handle = svga_new_handle(SVGA_KIND_SURFACE, id);
    if (!handle) { ret = SVGA_ENOMEM; goto done; }
    if (svga.gb_active) {
        svga.surfaces[id] = (struct svga_surface){.handle = handle, .desc = *desc,
            .block_w = bw, .block_h = bh, .block_bytes = bpp,
            .faces = (desc->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u, .bytes = bytes};
        svga.surface_bytes += bytes;
        ret = svga_gb_surface_create_locked(id);
        if (ret) svga.surfaces[id].gb.retiring = true;
        if (!ret || svga_surface_destroy_locked(id)) *out = handle;
        goto done;
    }
    uint8_t command[sizeof(SVGA3dCmdDefineSurface) +
                    SVGA3D_MAX_SURFACE_FACES * SVGA_MAX_MIPS * sizeof(SVGA3dSize)] = {0};
    SVGA3dCmdDefineSurface *cmd = (SVGA3dCmdDefineSurface *)command;
    uint32_t levels = desc->mip_levels ? desc->mip_levels : 1u;
    uint32_t faces = (desc->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u;
    cmd->sid = id;
    cmd->surfaceFlags = (SVGA3dSurfaceFlags)desc->flags;
    cmd->format = (SVGA3dSurfaceFormat)desc->format;
    for (uint32_t f = 0; f < faces; ++f) cmd->face[f].numMipLevels = levels;
    SVGA3dSize *sizes = (SVGA3dSize *)(command + sizeof(SVGA3dCmdDefineSurface));
    for (uint32_t f = 0; f < faces; ++f) {
        uint32_t width = desc->width, height = desc->height;
        uint32_t depth = desc->depth ? desc->depth : 1u;
        for (uint32_t level = 0; level < levels; ++level) {
            sizes[f * levels + level] = (SVGA3dSize){width, height, depth};
            if (width > 1) width >>= 1;
            if (height > 1) height >>= 1;
            if (depth > 1) depth >>= 1;
        }
    }
    ret = svga_command_locked(SVGA_3D_CMD_SURFACE_DEFINE, command,
                               sizeof(SVGA3dCmdDefineSurface) +
                               faces * levels * sizeof(*sizes));
    if (!ret) {
        svga.surfaces[id] = (struct svga_surface){.handle = handle, .desc = *desc,
            .block_w = bw, .block_h = bh, .block_bytes = bpp,
            .faces = (desc->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u, .bytes = bytes};
        svga.surface_bytes += bytes;
        *out = handle;
    }
done:
    return ret;
}

int svga3d_surface_create(const struct svga_surface_desc *desc, svga_handle *out)
{
    uint64_t flags = svga_lock();
    int ret = svga_surface_create_locked(desc, out);
    svga_unlock(flags);
    return ret;
}

int svga_surface_destroy_locked(uint32_t id)
{
    if (id >= SVGA_SURFACES || !svga.surfaces[id].handle) return SVGA_EINVAL;
    if (svga.surfaces[id].retiring) return SVGA_EBUSY;
    if (surface_referenced(id)) return SVGA_EBUSY;
    if (svga.gb_active) svga.surfaces[id].gb.retiring = true;
    int ret = svga_sync_locked();
    if (ret) return ret;
    if (svga.gb_active) ret = svga_gb_destroy_locked(&svga.surfaces[id].gb, id, false);
    else {
        SVGA3dCmdDestroySurface cmd = {id};
        ret = svga_command_locked(SVGA_3D_CMD_SURFACE_DESTROY, &cmd, sizeof(cmd));
    }
    if (!ret && svga.surfaces[id].gb.mode_lease)
        ret = svga_mode_lease_release_locked();
    if (!ret) {
        if (svga.surface_bytes >= svga.surfaces[id].bytes) svga.surface_bytes -= svga.surfaces[id].bytes;
        svga.surfaces[id] = (struct svga_surface){0};
    }
    return ret;
}
int svga3d_surface_destroy(svga_handle surface)
{
    uint64_t flags = svga_lock();
    int id = svga_find_surface(surface);
    int ret = id < 0 ? SVGA_EINVAL : svga_surface_destroy_locked((uint32_t)id);
    svga_unlock(flags);
    return ret;
}

int svga3d_surface_try_create(const struct svga_surface_desc *desc,
                              svga_handle *out)
{
    if (!out) return SVGA_EINVAL;
    *out = 0;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    uint32_t bw, bh, bpp;
    uint64_t bytes;
    int ret = surface_validate_locked(desc, &bw, &bh, &bpp, &bytes);
    if (ret) goto done;
    uint32_t id;
    for (id = 1; id < svga.surface_limit && svga.surfaces[id].handle; ++id) {}
    if (id >= svga.surface_limit) { ret = SVGA_ENOMEM; goto done; }
    svga_handle handle = svga_new_handle(SVGA_KIND_SURFACE, id);
    if (!handle) { ret = SVGA_ENOMEM; goto done; }

    uint32_t words[(sizeof(SVGA3dCmdDefineSurface) +
                    SVGA3D_MAX_SURFACE_FACES * SVGA_MAX_MIPS * sizeof(SVGA3dSize)) / 4u + 16u] = {0};
    uint32_t used = 0;
    struct svga_mob mob = {0};
    if (svga.gb_active) {
        ret = svga_gb_backing_alloc_locked(&mob, (uint32_t)bytes);
        if (ret) goto done;
        uint32_t mobid = SVGA_CONTEXTS + id;
        SVGA3dCmdDefineGBMob define_mob = {mobid, mob.depth,
            svga_gb_backing_root(&mob), mob.bytes};
        SVGA3dCmdDefineGBSurface define_surface = {.sid = id,
            .surfaceFlags = desc->flags, .format = desc->format,
            .numMipLevels = desc->mip_levels ? desc->mip_levels : 1u,
            .autogenFilter = SVGA3D_TEX_FILTER_NONE,
            .size = {desc->width, desc->height, desc->depth ? desc->depth : 1u}};
        SVGA3dCmdBindGBSurface bind = {id, mobid};
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])),
            &used, SVGA_3D_CMD_DEFINE_GB_MOB, &define_mob, sizeof(define_mob));
        if (!ret) ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])),
            &used, SVGA_3D_CMD_DEFINE_GB_SURFACE, &define_surface, sizeof(define_surface));
        if (!ret) ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])),
            &used, SVGA_3D_CMD_BIND_GB_SURFACE, &bind, sizeof(bind));
        if (!ret) ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])),
            &used, SVGA_3D_CMD_UPDATE_GB_SURFACE, &id, sizeof(id));
    } else {
        uint8_t body[sizeof(SVGA3dCmdDefineSurface) +
                     SVGA3D_MAX_SURFACE_FACES * SVGA_MAX_MIPS * sizeof(SVGA3dSize)] = {0};
        SVGA3dCmdDefineSurface *define = (SVGA3dCmdDefineSurface *)body;
        uint32_t levels = desc->mip_levels ? desc->mip_levels : 1u;
        uint32_t faces = (desc->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u;
        define->sid = id;
        define->surfaceFlags = (SVGA3dSurfaceFlags)desc->flags;
        define->format = (SVGA3dSurfaceFormat)desc->format;
        for (uint32_t f = 0; f < faces; ++f) define->face[f].numMipLevels = levels;
        SVGA3dSize *sizes = (SVGA3dSize *)(body + sizeof(*define));
        for (uint32_t f = 0; f < faces; ++f) {
            uint32_t width = desc->width, height = desc->height;
            uint32_t depth = desc->depth ? desc->depth : 1u;
            for (uint32_t level = 0; level < levels; ++level) {
                sizes[f * levels + level] = (SVGA3dSize){width, height, depth};
                if (width > 1) width >>= 1;
                if (height > 1) height >>= 1;
                if (depth > 1) depth >>= 1;
            }
        }
        uint32_t body_bytes = sizeof(*define) + faces * levels * sizeof(*sizes);
        ret = lifecycle_packet(words, (uint32_t)(sizeof(words) / sizeof(words[0])),
            &used, SVGA_3D_CMD_SURFACE_DEFINE, body, body_bytes);
    }
    if (!ret) ret = svga_mode_lease_acquire_locked();
    if (ret) {
        if (mob.phys) svga_gb_backing_free_locked(&mob);
        goto done;
    }
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
    if (ret) {
        (void)svga_mode_lease_release_locked();
        if (mob.phys) svga_gb_backing_free_locked(&mob);
        goto done;
    }
    if (mob.phys) mob.defined = true;
    svga.surfaces[id] = (struct svga_surface){.handle = handle, .desc = *desc,
        .block_w = bw, .block_h = bh, .block_bytes = bpp,
        .faces = (desc->flags & SVGA3D_SURFACE_CUBEMAP) ? 6u : 1u,
        .bytes = bytes, .ready_fence = fence,
        .gb = {.mob = mob, .defined = svga.gb_active, .bound = svga.gb_active,
               .mode_lease = true}};
    svga.surface_bytes += bytes;
    *out = handle;
    ret = 0;
done:
    svga_unlock(flags);
    return ret;
}

int svga3d_surface_try_destroy(svga_handle handle)
{
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return SVGA_EBUSY;
    int id = svga_find_surface(handle), ret = SVGA_EINVAL;
    if (id < 0) goto done;
    struct svga_surface *surface = &svga.surfaces[id];
    if (surface->retiring) {
        if (!svga_fence_reached_locked(surface->retire_fence)) {
            ret = SVGA_EBUSY;
            goto done;
        }
        if (surface->gb.mode_lease) {
            ret = svga_mode_lease_release_locked();
            if (ret) goto done;
        }
        svga_gb_backing_free_locked(&surface->gb.mob);
        if (svga.surface_bytes >= surface->bytes) svga.surface_bytes -= surface->bytes;
        *surface = (struct svga_surface){0};
        ret = 0;
        goto done;
    }
    ret = require_ready();
    if (ret) goto done;
    if (surface_referenced((uint32_t)id)) {
        ret = SVGA_EBUSY;
        goto done;
    }

    uint32_t words[32] = {0}, used = 0;
    if (svga.gb_active) {
        if (surface->gb.bound) {
            ret = lifecycle_packet(words, 32, &used,
                SVGA_3D_CMD_INVALIDATE_GB_SURFACE, &id, sizeof(id));
            SVGA3dCmdBindGBSurface unbind = {(uint32_t)id, SVGA3D_INVALID_ID};
            if (!ret) ret = lifecycle_packet(words, 32, &used,
                SVGA_3D_CMD_BIND_GB_SURFACE, &unbind, sizeof(unbind));
        }
        if (!ret && surface->gb.defined)
            ret = lifecycle_packet(words, 32, &used,
                SVGA_3D_CMD_DESTROY_GB_SURFACE, &id, sizeof(id));
        if (!ret && surface->gb.mob.defined) {
            uint32_t mobid = SVGA_CONTEXTS + (uint32_t)id;
            ret = lifecycle_packet(words, 32, &used,
                SVGA_3D_CMD_DESTROY_GB_MOB, &mobid, sizeof(mobid));
        }
    } else {
        SVGA3dCmdDestroySurface destroy = {(uint32_t)id};
        ret = lifecycle_packet(words, 32, &used,
            SVGA_3D_CMD_SURFACE_DESTROY, &destroy, sizeof(destroy));
    }
    if (ret) goto done;
    svga_handle fence = 0;
    ret = svga_fifo_batch_try_locked(words, used * 4u, &fence);
    if (ret) goto done;
    surface->retiring = true;
    surface->gb.retiring = true;
    surface->retire_fence = fence;
    ret = SVGA_EBUSY;
done:
    svga_unlock(flags);
    return ret;
}

static int gb_submit_binding(uint32_t command, const void *payload, uint32_t bytes)
{
    uint32_t cid;
    if (bytes < sizeof(cid)) return SVGA_EINVAL;
    __builtin_memcpy(&cid, payload, sizeof(cid));
    if (!valid_cid(cid)) return SVGA_EINVAL;
    if (command == SVGA_3D_CMD_SETRENDERTARGET) {
        SVGA3dCmdSetRenderTarget cmd;
        if (bytes != sizeof(cmd)) return SVGA_EINVAL;
        __builtin_memcpy(&cmd, payload, sizeof(cmd));
        if ((uint32_t)cmd.type >= SVGA3D_RT_MAX) return SVGA_EINVAL;
        if (cmd.target.sid != SVGA3D_INVALID_ID) {
            if (!valid_sid(cmd.target.sid)) return SVGA_EINVAL;
            const struct svga_surface *s = &svga.surfaces[cmd.target.sid];
            if (cmd.target.face >= s->faces ||
                cmd.target.mipmap >= (s->desc.mip_levels ? s->desc.mip_levels : 1u))
                return SVGA_EINVAL;
        }
        int ret = svga_command_locked(command, payload, bytes);
        if (!ret) svga.contexts[cid].targets[cmd.type] = cmd.target.sid;
        return ret;
    }
    if ((bytes - sizeof(cid)) % sizeof(SVGA3dTextureState)) return SVGA_EINVAL;
    uint32_t count = (bytes - sizeof(cid)) / sizeof(SVGA3dTextureState);
    const uint8_t *states = (const uint8_t *)payload + sizeof(cid);
    for (uint32_t i = 0; i < count; ++i) {
        SVGA3dTextureState state;
        __builtin_memcpy(&state, states + i * sizeof(state), sizeof(state));
        if (state.stage >= 32 || (state.name == SVGA3D_TS_BIND_TEXTURE &&
            state.value != SVGA3D_INVALID_ID && !valid_sid(state.value))) return SVGA_EINVAL;
    }
    int ret = svga_command_locked(command, payload, bytes);
    if (!ret) {
        for (uint32_t i = 0; i < count; ++i) {
            SVGA3dTextureState state;
            __builtin_memcpy(&state, states + i * sizeof(state), sizeof(state));
            if (state.name == SVGA3D_TS_BIND_TEXTURE)
                svga.contexts[cid].textures[state.stage] = state.value;
        }
    }
    return ret;
}

int svga3d_submit(uint32_t generation, uint32_t command,
                  const void *payload, uint32_t bytes)
{
    if (!payload || (bytes & 3u) || command < SVGA_3D_CMD_BASE ||
        command >= SVGA_3D_CMD_MAX || managed_command(command))
        return SVGA_EINVAL;
    uint64_t flags = svga_lock();
    int ret = generation != svga.generation ? SVGA_EIO : require_ready();
    if (!ret && svga.gb_active) {
        if (!gb_raw_supported(command)) ret = SVGA_ENOTSUP;
        /* Raw ring-0 packets may contain several resource IDs. Require all
         * pending retirements to finish before accepting another packet. */
        for (uint32_t i = 0; !ret && i < SVGA_CONTEXTS; ++i)
            if (svga.contexts[i].gb.retiring) ret = SVGA_EBUSY;
        for (uint32_t i = 0; !ret && i < SVGA_SURFACES; ++i)
            if (svga.surfaces[i].gb.retiring) ret = SVGA_EBUSY;
    }
    if (!ret) {
        if (svga.gb_active && (command == SVGA_3D_CMD_SETRENDERTARGET ||
                              command == SVGA_3D_CMD_SETTEXTURESTATE))
            ret = gb_submit_binding(command, payload, bytes);
        else ret = svga_command_locked(command, payload, bytes);
    }
    svga_unlock(flags);
    return ret;
}

int svga3d_set_render_target(svga_handle context, uint32_t target_type,
                             svga_handle surface)
{
    uint64_t flags = svga_lock();
    int cid = svga_find_context(context), sid = svga_find_surface(surface);
    int ret = (!valid_cid((uint32_t)cid) || !valid_sid((uint32_t)sid) ||
               target_type >= SVGA3D_RT_MAX) ? SVGA_EINVAL : require_ready();
    if (!ret) {
        SVGA3dCmdSetRenderTarget cmd = {(uint32_t)cid, (SVGA3dRenderTargetType)target_type,
                                        {(uint32_t)sid, 0, 0}};
        ret = svga_command_locked(SVGA_3D_CMD_SETRENDERTARGET, &cmd, sizeof(cmd));
        if (!ret) svga.contexts[cid].targets[target_type] = (uint32_t)sid;
    }
    svga_unlock(flags);
    return ret;
}

int svga3d_clear(svga_handle context, uint32_t flags, uint32_t color,
                 float depth, uint32_t stencil)
{
    uint64_t lock_flags = svga_lock();
    int cid = svga_find_context(context);
    int ret = !valid_cid((uint32_t)cid) ? SVGA_EINVAL : require_ready();
    if (!ret) {
        uint32_t sid = svga.contexts[cid].targets[(flags & SVGA3D_CLEAR_COLOR) ?
            SVGA3D_RT_COLOR0 : SVGA3D_RT_DEPTH];
        if (!flags || (flags & ~(SVGA3D_CLEAR_COLOR | SVGA3D_CLEAR_DEPTH | SVGA3D_CLEAR_STENCIL)) ||
            !valid_sid(sid)) ret = SVGA_EINVAL;
        else {
            SVGA3dCmdClear cmd = {(uint32_t)cid, (SVGA3dClearFlag)flags, color, depth, stencil};
            SVGA3dRect rect = {0, 0, svga.surfaces[sid].desc.width, svga.surfaces[sid].desc.height};
            struct svga_span spans[] = {{&cmd, sizeof(cmd)}, {&rect, sizeof(rect)}};
            ret = svga_fifo_packet_locked(SVGA_3D_CMD_CLEAR, true, spans, 2);
        }
    }
    svga_unlock(lock_flags);
    return ret;
}

static int checked_gmr(int id, uint32_t offset, uint32_t bytes)
{
    if (id < 0 || (uint32_t)id >= SVGA_GMRS || !svga.gmrs[id].handle ||
        svga.gmrs[id].retiring || offset > svga.gmrs[id].bytes || bytes > svga.gmrs[id].bytes - offset)
        return SVGA_EINVAL;
    return 0;
}

int svga_surface_dma_locked(svga_handle surface, uint32_t face, uint32_t mip,
                       svga_handle buffer, uint32_t offset, uint32_t pitch,
                       uint32_t direction, const struct svga_dma_box *boxes,
                       uint32_t count)
{
    if (!boxes || !count || count > 64 || !pitch ||
        (direction != SVGA3D_WRITE_HOST_VRAM && direction != SVGA3D_READ_HOST_VRAM))
        return SVGA_EINVAL;
    int sid = svga_find_surface(surface), gid = svga_find_gmr(buffer);
    int ret = (!valid_sid((uint32_t)sid) || gid < 0 || face >= svga.surfaces[sid].faces ||
               mip >= (svga.surfaces[sid].desc.mip_levels ? svga.surfaces[sid].desc.mip_levels : 1u)) ? SVGA_EINVAL : require_ready();
    uint64_t bytes = 0;
    if (!ret) {
        uint32_t level_width = svga.surfaces[sid].desc.width;
        uint32_t level_height = svga.surfaces[sid].desc.height;
        uint32_t level_depth = svga.surfaces[sid].desc.depth ? svga.surfaces[sid].desc.depth : 1u;
        for (uint32_t level = 0; level < mip; ++level) {
            if (level_width > 1) level_width >>= 1;
            if (level_height > 1) level_height >>= 1;
            if (level_depth > 1) level_depth >>= 1;
        }
        level_width = (level_width + svga.surfaces[sid].block_w - 1u) /
                      svga.surfaces[sid].block_w;
        level_height = (level_height + svga.surfaces[sid].block_h - 1u) /
                       svga.surfaces[sid].block_h;
        for (uint32_t i = 0; i < count; ++i) {
            if (!boxes[i].w || !boxes[i].h || !boxes[i].d ||
                boxes[i].x > level_width || boxes[i].w > level_width - boxes[i].x ||
                boxes[i].y > level_height || boxes[i].h > level_height - boxes[i].y ||
                boxes[i].z > level_depth || boxes[i].d > level_depth - boxes[i].z ||
                (uint64_t)pitch * boxes[i].h > UINT32_MAX) { ret = SVGA_EINVAL; break; }
            uint64_t row_bytes = (uint64_t)boxes[i].w * svga.surfaces[sid].block_bytes;
            uint64_t source_x = (uint64_t)boxes[i].srcx * svga.surfaces[sid].block_bytes;
            if (source_x > pitch || row_bytes > pitch - source_x) {
                ret = SVGA_EINVAL;
                break;
            }
            uint64_t slice_stride = (uint64_t)pitch * level_height;
            uint64_t slice_index = (uint64_t)boxes[i].srcz + boxes[i].d - 1u;
            uint64_t row_index = (uint64_t)boxes[i].srcy + boxes[i].h - 1u;
            if ((slice_stride && slice_index > UINT32_MAX / slice_stride) ||
                (pitch && row_index > UINT32_MAX / pitch)) {
                ret = SVGA_EINVAL;
                break;
            }
            uint64_t last_slice = slice_index * slice_stride;
            uint64_t last_row = row_index * pitch;
            uint64_t end = last_slice + last_row +
                           source_x + row_bytes;
            if (end < last_slice || end < last_row || end < source_x || end > UINT32_MAX) {
                ret = SVGA_EINVAL;
                break;
            }
            if (end > bytes) bytes = end;
        }
        if (!ret) ret = checked_gmr(gid, offset, (uint32_t)bytes);
    }
    if (!ret) {
        SVGA3dCmdSurfaceDMA cmd = {0};
        cmd.guest.ptr.gmrId = (uint32_t)gid;
        cmd.guest.ptr.offset = offset;
        cmd.guest.pitch = pitch;
        cmd.host.sid = (uint32_t)sid;
        cmd.host.face = face;
        cmd.host.mipmap = mip;
        cmd.transfer = (SVGA3dTransferType)direction;
        struct svga_span spans[] = {{&cmd, sizeof(cmd)}, {boxes, count * sizeof(*boxes)}};
        ret = svga_fifo_packet_locked(SVGA_3D_CMD_SURFACE_DMA, true, spans, 2);
    }
    return ret;
}

int svga3d_surface_dma(svga_handle surface, uint32_t face, uint32_t mip,
                       svga_handle buffer, uint32_t offset, uint32_t pitch,
                       uint32_t direction, const struct svga_dma_box *boxes, uint32_t count)
{
    uint64_t flags = svga_lock();
    int ret = svga_surface_dma_locked(surface, face, mip, buffer, offset, pitch,
                                      direction, boxes, count);
    svga_unlock(flags);
    return ret;
}

int svga3d_present(svga_handle surface, const struct svga_present_rect *rects,
                   uint32_t count)
{
    if (!rects || !count || count > 64) return SVGA_EINVAL;
    uint64_t flags = svga_lock();
    int sid = svga_find_surface(surface);
    int ret = !valid_sid((uint32_t)sid) ? SVGA_EINVAL : require_ready();
    if (!ret) {
        for (uint32_t i = 0; i < count; ++i) {
            if (!rects[i].w || !rects[i].h || rects[i].x > svga.surfaces[sid].desc.width - 1u ||
                rects[i].y > svga.surfaces[sid].desc.height - 1u ||
                rects[i].w > svga.surfaces[sid].desc.width - rects[i].x ||
                rects[i].h > svga.surfaces[sid].desc.height - rects[i].y) { ret = SVGA_EINVAL; break; }
        }
    }
    if (!ret) {
        SVGA3dCmdPresent cmd = {(uint32_t)sid};
        struct svga_span spans[] = {{&cmd, sizeof(cmd)}, {rects, count * sizeof(*rects)}};
        ret = svga_fifo_packet_locked(SVGA_3D_CMD_PRESENT, true, spans, 2);
        if (!ret) ret = svga_sync_locked();
    }
    svga_unlock(flags);
    return ret;
}
