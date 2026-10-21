#include "execbuf_parser.h"

#include <linux/errno.h>
#include <stddef.h>

#include "../../svga/svga_protocol.h"

struct drm_vmw_execbuf_packet_header {
    uint32_t command;
    uint32_t payload_bytes;
};

static bool drm_vmw_execbuf_size_is(uint32_t actual, uint32_t expected)
{
    return actual == expected;
}

static bool drm_vmw_execbuf_variable_size(uint32_t actual, uint32_t base,
                                           uint32_t element_size,
                                           bool at_least_one)
{
    if (!element_size || actual < base) return false;
    uint32_t tail = actual - base;
    if ((tail % element_size) != 0) return false;
    return !at_least_one || tail != 0;
}

static bool drm_vmw_execbuf_mul_add(uint32_t base, uint32_t count,
                                     uint32_t element_size,
                                     uint32_t *result)
{
    if (!result || (count && element_size > (UINT32_MAX - base) / count))
        return false;
    *result = base + count * element_size;
    return true;
}

static uint32_t drm_vmw_execbuf_load_u32(const uint8_t *bytes)
{
    uint32_t value;
    __builtin_memcpy(&value, bytes, sizeof(value));
    return value;
}

static bool drm_vmw_execbuf_bool_value_valid(uint32_t value)
{
    return value <= 1u;
}

static bool drm_vmw_execbuf_render_state_value_valid(
    const SVGA3dRenderState *state)
{
    if (!state) return false;
    uint32_t value = state->uintValue;
    switch (state->state) {
    case SVGA3D_RS_ZENABLE:
    case SVGA3D_RS_ZWRITEENABLE:
    case SVGA3D_RS_ALPHATESTENABLE:
    case SVGA3D_RS_DITHERENABLE:
    case SVGA3D_RS_BLENDENABLE:
    case SVGA3D_RS_FOGENABLE:
    case SVGA3D_RS_SPECULARENABLE:
    case SVGA3D_RS_STENCILENABLE:
    case SVGA3D_RS_LIGHTINGENABLE:
    case SVGA3D_RS_NORMALIZENORMALS:
    case SVGA3D_RS_POINTSPRITEENABLE:
    case SVGA3D_RS_POINTSCALEENABLE:
    case SVGA3D_RS_RANGEFOGENABLE:
    case SVGA3D_RS_VERTEXMATERIALENABLE:
    case SVGA3D_RS_LOCALVIEWER:
    case SVGA3D_RS_SCISSORTESTENABLE:
    case SVGA3D_RS_STENCILENABLE2SIDED:
    case SVGA3D_RS_ZVISIBLE:
    case SVGA3D_RS_LASTPIXEL:
    case SVGA3D_RS_CLIPPING:
    case SVGA3D_RS_MULTISAMPLEANTIALIAS:
    case SVGA3D_RS_INDEXEDVERTEXBLENDENABLE:
    case SVGA3D_RS_ANTIALIASEDLINEENABLE:
    case SVGA3D_RS_SEPARATEALPHABLENDENABLE:
    case SVGA3D_RS_LINEAA:
        return drm_vmw_execbuf_bool_value_valid(value);
    case SVGA3D_RS_FOGMODE: {
        SVGA3dFogMode mode = {.uintValue = value};
        return mode.function > SVGA3D_FOGFUNC_INVALID &&
               mode.function <= SVGA3D_FOGFUNC_PER_VERTEX &&
               mode.type > SVGA3D_FOGTYPE_INVALID &&
               mode.type < SVGA3D_FOGTYPE_MAX &&
               mode.base > SVGA3D_FOGBASE_INVALID &&
               mode.base < SVGA3D_FOGBASE_MAX;
    }
    case SVGA3D_RS_FILLMODE: {
        SVGA3dFillMode mode = {.uintValue = value};
        return mode.mode > SVGA3D_FILLMODE_INVALID &&
               mode.mode < SVGA3D_FILLMODE_MAX &&
               mode.face < SVGA3D_FACE_MAX;
    }
    case SVGA3D_RS_SHADEMODE:
        return value > SVGA3D_SHADEMODE_INVALID &&
               value < SVGA3D_SHADEMODE_MAX;
    case SVGA3D_RS_SRCBLEND:
    case SVGA3D_RS_DSTBLEND:
    case SVGA3D_RS_SRCBLENDALPHA:
    case SVGA3D_RS_DSTBLENDALPHA:
        return value > SVGA3D_BLENDOP_INVALID &&
               value < SVGA3D_BLENDOP_MAX;
    case SVGA3D_RS_BLENDEQUATION:
    case SVGA3D_RS_BLENDEQUATIONALPHA:
        return value > SVGA3D_BLENDEQ_INVALID &&
               value < SVGA3D_BLENDEQ_MAX;
    case SVGA3D_RS_CULLMODE:
        return value > SVGA3D_FACE_INVALID && value < SVGA3D_FACE_MAX;
    case SVGA3D_RS_ZFUNC:
    case SVGA3D_RS_ALPHAFUNC:
    case SVGA3D_RS_STENCILFUNC:
    case SVGA3D_RS_CCWSTENCILFUNC:
        return value > SVGA3D_CMP_INVALID && value < SVGA3D_CMP_MAX;
    case SVGA3D_RS_STENCILFAIL:
    case SVGA3D_RS_STENCILZFAIL:
    case SVGA3D_RS_STENCILPASS:
    case SVGA3D_RS_CCWSTENCILFAIL:
    case SVGA3D_RS_CCWSTENCILZFAIL:
    case SVGA3D_RS_CCWSTENCILPASS:
        return value > SVGA3D_STENCILOP_INVALID &&
               value < SVGA3D_STENCILOP_MAX;
    case SVGA3D_RS_FRONTWINDING:
        return value > SVGA3D_FRONTWINDING_INVALID &&
               value < SVGA3D_FRONTWINDING_MAX;
    case SVGA3D_RS_COORDINATETYPE:
        return value > SVGA3D_COORDINATE_INVALID &&
               value < SVGA3D_COORDINATE_MAX;
    case SVGA3D_RS_DIFFUSEMATERIALSOURCE:
    case SVGA3D_RS_SPECULARMATERIALSOURCE:
    case SVGA3D_RS_AMBIENTMATERIALSOURCE:
    case SVGA3D_RS_EMISSIVEMATERIALSOURCE:
        return value <= SVGA3D_VERTEXMATERIAL_SPECULAR;
    case SVGA3D_RS_CLIPPLANEENABLE:
        return (value & ~((1u << SVGA3D_MAX_CLIP_PLANES) - 1u)) == 0;
    case SVGA3D_RS_COLORWRITEENABLE:
    case SVGA3D_RS_COLORWRITEENABLE1:
    case SVGA3D_RS_COLORWRITEENABLE2:
    case SVGA3D_RS_COLORWRITEENABLE3:
        return (value & ~0xfu) == 0;
    case SVGA3D_RS_VERTEXBLEND:
        return value <= SVGA3D_VBLEND_3WEIGHT;
    case SVGA3D_RS_WRAP0:
    case SVGA3D_RS_WRAP1:
    case SVGA3D_RS_WRAP2:
    case SVGA3D_RS_WRAP3:
    case SVGA3D_RS_WRAP4:
    case SVGA3D_RS_WRAP5:
    case SVGA3D_RS_WRAP6:
    case SVGA3D_RS_WRAP7:
    case SVGA3D_RS_WRAP8:
    case SVGA3D_RS_WRAP9:
    case SVGA3D_RS_WRAP10:
    case SVGA3D_RS_WRAP11:
    case SVGA3D_RS_WRAP12:
    case SVGA3D_RS_WRAP13:
    case SVGA3D_RS_WRAP14:
    case SVGA3D_RS_WRAP15:
        return (value & ~SVGA3D_WRAPCOORD_ALL) == 0;
    case SVGA3D_RS_TRANSPARENCYANTIALIAS:
        return value < SVGA3D_TRANSPARENCYANTIALIAS_MAX;
    default:
        /* Float states and unconstrained integer/color/bit-pattern states
         * retain their protocol payload without a generic finite check. */
        return true;
    }
}

static bool drm_vmw_execbuf_texture_state_value_valid(
    const SVGA3dTextureState *state)
{
    if (!state) return false;
    uint32_t value = state->value;
    switch (state->name) {
    case SVGA3D_TS_COLOROP:
    case SVGA3D_TS_ALPHAOP:
        return value > SVGA3D_TC_INVALID && value < SVGA3D_TC_MAX;
    case SVGA3D_TS_COLORARG0:
    case SVGA3D_TS_COLORARG1:
    case SVGA3D_TS_COLORARG2:
    case SVGA3D_TS_ALPHAARG0:
    case SVGA3D_TS_ALPHAARG1:
    case SVGA3D_TS_ALPHAARG2: {
        uint32_t argument = value & 0xfu;
        uint32_t modifiers = value & ~0xfu;
        return argument > SVGA3D_TA_INVALID &&
               argument < SVGA3D_TA_MAX &&
               (modifiers & ~(SVGA3D_TM_ALPHA | SVGA3D_TM_ONE_MINUS)) == 0;
    }
    case SVGA3D_TS_ADDRESSU:
    case SVGA3D_TS_ADDRESSV:
    case SVGA3D_TS_ADDRESSW:
        return value > SVGA3D_TEX_ADDRESS_INVALID &&
               value < SVGA3D_TEX_ADDRESS_MAX;
    case SVGA3D_TS_MIPFILTER:
    case SVGA3D_TS_MAGFILTER:
    case SVGA3D_TS_MINFILTER:
        return value < SVGA3D_TEX_FILTER_MAX;
    case SVGA3D_TS_TEXTURETRANSFORMFLAGS: {
        const uint32_t valid = SVGA3D_TEX_TRANSFORM_S |
            SVGA3D_TEX_TRANSFORM_T | SVGA3D_TEX_TRANSFORM_R |
            SVGA3D_TEX_TRANSFORM_Q | SVGA3D_TEX_PROJECTED;
        return (value & ~valid) == 0;
    }
    case SVGA3D_TS_TEXCOORDGEN:
        return value < SVGA3D_TEXCOORD_GEN_MAX;
    default:
        /* Bindings, indices, mip levels, float states, and colors are
         * validated by their resource/arithmetic paths or kept verbatim. */
        return true;
    }
}

static bool drm_vmw_execbuf_fields_valid(uint32_t command,
                                          const uint8_t *body,
                                          uint32_t bytes)
{
    switch (command) {
    case SVGA_3D_CMD_SETTRANSFORM: {
        SVGA3dCmdSetTransform packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type > SVGA3D_TRANSFORM_INVALID &&
               packet.type < SVGA3D_TRANSFORM_MAX;
    }
    case SVGA_3D_CMD_SURFACE_STRETCHBLT: {
        SVGA3dCmdSurfaceStretchBlt packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.mode < SVGA3D_STRETCH_BLT_MAX;
    }
    case SVGA_3D_CMD_SETCLIPPLANE: {
        SVGA3dCmdSetClipPlane packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.index < SVGA3D_MAX_CLIP_PLANES;
    }
    case SVGA_3D_CMD_SETRENDERSTATE: {
        uint32_t count = (bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dRenderState);
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t name = drm_vmw_execbuf_load_u32(
                body + sizeof(uint32_t) +
                i * sizeof(SVGA3dRenderState));
            SVGA3dRenderState state;
            __builtin_memcpy(&state, body + sizeof(uint32_t) +
                i * sizeof(state), sizeof(state));
            if (!name || name >= SVGA3D_RS_MAX ||
                !drm_vmw_execbuf_render_state_value_valid(&state))
                return false;
        }
        return true;
    }
    case SVGA_3D_CMD_SETRENDERTARGET: {
        SVGA3dCmdSetRenderTarget packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        /* The negotiated VGPU9 profile exposes four color targets. Depth
         * and stencil occupy the preceding enum values. */
        return packet.type <= SVGA3D_RT_COLOR3;
    }
    case SVGA_3D_CMD_SETTEXTURESTATE: {
        uint32_t count = (bytes - sizeof(uint32_t)) /
                         sizeof(SVGA3dTextureState);
        for (uint32_t i = 0; i < count; ++i) {
            SVGA3dTextureState state;
            __builtin_memcpy(&state, body + sizeof(uint32_t) +
                i * sizeof(state), sizeof(state));
            if (state.stage >= 32u || !state.name ||
                state.name >= SVGA3D_TS_MAX ||
                !drm_vmw_execbuf_texture_state_value_valid(&state))
                return false;
        }
        return true;
    }
    case SVGA_3D_CMD_CLEAR: {
        SVGA3dCmdClear packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return (packet.clearFlag &
                ~(SVGA3D_CLEAR_COLOR | SVGA3D_CLEAR_DEPTH |
                  SVGA3D_CLEAR_STENCIL)) == 0;
    }
    case SVGA_3D_CMD_SHADER_DEFINE: {
        SVGA3dCmdDefineShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_SHADERTYPE_VS ||
               packet.type == SVGA3D_SHADERTYPE_PS;
    }
    case SVGA_3D_CMD_SETLIGHTDATA: {
        SVGA3dCmdSetLightData packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.data.type > SVGA3D_LIGHTTYPE_INVALID &&
               packet.data.type < SVGA3D_LIGHTTYPE_MAX &&
               drm_vmw_execbuf_bool_value_valid(packet.data.inWorldSpace);
    }
    case SVGA_3D_CMD_SETLIGHTENABLED: {
        SVGA3dCmdSetLightEnabled packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return drm_vmw_execbuf_bool_value_valid(packet.enabled);
    }
    case SVGA_3D_CMD_SETMATERIAL: {
        SVGA3dCmdSetMaterial packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.face > SVGA3D_FACE_INVALID &&
               packet.face < SVGA3D_FACE_MAX;
    }
    case SVGA_3D_CMD_SHADER_DESTROY: {
        SVGA3dCmdDestroyShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_SHADERTYPE_VS ||
               packet.type == SVGA3D_SHADERTYPE_PS;
    }
    case SVGA_3D_CMD_SET_SHADER: {
        SVGA3dCmdSetShader packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_SHADERTYPE_VS ||
               packet.type == SVGA3D_SHADERTYPE_PS;
    }
    case SVGA_3D_CMD_SET_SHADER_CONST: {
        SVGA3dCmdSetShaderConst packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        if ((packet.type != SVGA3D_SHADERTYPE_VS &&
             packet.type != SVGA3D_SHADERTYPE_PS) ||
            packet.ctype > SVGA3D_CONST_TYPE_BOOL)
            return false;
        uint32_t registers = (bytes - sizeof(packet)) /
                             sizeof(packet.values) + 1u;
        uint32_t limit = packet.ctype == SVGA3D_CONST_TYPE_FLOAT ? 256u : 16u;
        return packet.reg < limit && registers <= limit - packet.reg;
    }
    case SVGA_3D_CMD_BEGIN_QUERY: {
        SVGA3dCmdBeginQuery packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_QUERYTYPE_OCCLUSION;
    }
    case SVGA_3D_CMD_END_QUERY: {
        SVGA3dCmdEndQuery packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_QUERYTYPE_OCCLUSION;
    }
    case SVGA_3D_CMD_WAIT_FOR_QUERY: {
        SVGA3dCmdWaitForQuery packet;
        __builtin_memcpy(&packet, body, sizeof(packet));
        return packet.type == SVGA3D_QUERYTYPE_OCCLUSION;
    }
    case SVGA_3D_CMD_DRAW_PRIMITIVES: {
        SVGA3dCmdDrawPrimitives header;
        __builtin_memcpy(&header, body, sizeof(header));
        const SVGA3dVertexDecl *decls =
            (const SVGA3dVertexDecl *)(body + sizeof(header));
        const SVGA3dPrimitiveRange *ranges =
            (const SVGA3dPrimitiveRange *)(decls + header.numVertexDecls);
        for (uint32_t i = 0; i < header.numVertexDecls; ++i) {
            const SVGA3dVertexDecl *decl = &decls[i];
            if (decl->identity.type >= SVGA3D_DECLTYPE_MAX ||
                decl->identity.method > SVGA3D_DECLMETHOD_LOOKUPPRESAMPLED ||
                decl->identity.usage >= SVGA3D_DECLUSAGE_MAX)
                return false;
        }
        for (uint32_t i = 0; i < header.numRanges; ++i) {
            const SVGA3dPrimitiveRange *range = &ranges[i];
            if (!range->primType || range->primType >= SVGA3D_PRIMITIVE_MAX)
                return false;
            if (range->indexArray.surfaceId == SVGA3D_INVALID_ID) {
                if (range->indexWidth) return false;
            } else if ((range->indexWidth != 2u && range->indexWidth != 4u) ||
                       range->indexArray.stride != range->indexWidth) {
                return false;
            }
        }
        return true;
    }
    default:
        return true;
    }
}

static bool drm_vmw_execbuf_packet_shape(uint32_t command,
                                          const uint8_t *body,
                                          uint32_t bytes)
{
    switch (command) {
    /* Context and surface lifetimes are owned by DRM ioctls. */
    case SVGA_3D_CMD_SURFACE_DEFINE:
    case SVGA_3D_CMD_SURFACE_DEFINE_V2:
    case SVGA_3D_CMD_SURFACE_DESTROY:
    case SVGA_3D_CMD_CONTEXT_DEFINE:
    case SVGA_3D_CMD_CONTEXT_DESTROY:
        return false;
    case SVGA_3D_CMD_SURFACE_COPY:
        return drm_vmw_execbuf_variable_size(
            bytes, sizeof(SVGA3dSurfaceImageId) * 2u,
            sizeof(SVGA3dCopyBox), true);
    case SVGA_3D_CMD_SURFACE_STRETCHBLT:
        return drm_vmw_execbuf_size_is(bytes,
                                       sizeof(SVGA3dCmdSurfaceStretchBlt));
    case SVGA_3D_CMD_SURFACE_DMA:
        if (bytes < sizeof(SVGA3dCmdSurfaceDMA) +
                    sizeof(SVGA3dCopyBox) +
                    sizeof(SVGA3dCmdSurfaceDMASuffix))
            return false;
        {
            uint32_t boxes_bytes = bytes -
                sizeof(SVGA3dCmdSurfaceDMASuffix);
            if (!drm_vmw_execbuf_variable_size(
                    boxes_bytes, sizeof(SVGA3dCmdSurfaceDMA),
                    sizeof(SVGA3dCopyBox), true))
                return false;
            SVGA3dCmdSurfaceDMA dma;
            SVGA3dCmdSurfaceDMASuffix suffix;
            __builtin_memcpy(&dma, body, sizeof(dma));
            __builtin_memcpy(&suffix, body + boxes_bytes, sizeof(suffix));
            return (dma.transfer == SVGA3D_WRITE_HOST_VRAM ||
                    dma.transfer == SVGA3D_READ_HOST_VRAM) &&
                   suffix.suffixSize == sizeof(suffix) &&
                   suffix.flags.reserved == 0;
        }
    case SVGA_3D_CMD_SETTRANSFORM:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetTransform));
    case SVGA_3D_CMD_SETZRANGE:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetZRange));
    case SVGA_3D_CMD_SETRENDERSTATE:
        return drm_vmw_execbuf_variable_size(
            bytes, sizeof(uint32_t), sizeof(SVGA3dRenderState), false);
    case SVGA_3D_CMD_SETRENDERTARGET:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetRenderTarget));
    case SVGA_3D_CMD_SETTEXTURESTATE:
        return drm_vmw_execbuf_variable_size(
            bytes, sizeof(uint32_t), sizeof(SVGA3dTextureState), false);
    case SVGA_3D_CMD_SETMATERIAL:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetMaterial));
    case SVGA_3D_CMD_SETLIGHTDATA:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetLightData));
    case SVGA_3D_CMD_SETLIGHTENABLED:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetLightEnabled));
    case SVGA_3D_CMD_SETVIEWPORT:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetViewport));
    case SVGA_3D_CMD_SETCLIPPLANE:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetClipPlane));
    case SVGA_3D_CMD_CLEAR:
        return drm_vmw_execbuf_variable_size(
            bytes, sizeof(SVGA3dCmdClear), sizeof(SVGA3dRect), false);
    case SVGA_3D_CMD_SHADER_DEFINE:
        return bytes >= sizeof(SVGA3dCmdDefineShader) + sizeof(uint32_t) &&
               (bytes & 3u) == 0;
    case SVGA_3D_CMD_SHADER_DESTROY:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdDestroyShader));
    case SVGA_3D_CMD_SET_SHADER:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetShader));
    case SVGA_3D_CMD_SET_SHADER_CONST:
        return bytes >= sizeof(SVGA3dCmdSetShaderConst) &&
               ((bytes - sizeof(SVGA3dCmdSetShaderConst)) & 15u) == 0;
    case SVGA_3D_CMD_DRAW_PRIMITIVES: {
        if (bytes < sizeof(SVGA3dCmdDrawPrimitives)) return false;
        uint32_t decls = drm_vmw_execbuf_load_u32(body + sizeof(uint32_t));
        uint32_t ranges = drm_vmw_execbuf_load_u32(body + 2u * sizeof(uint32_t));
        if (decls > SVGA3D_MAX_VERTEX_ARRAYS ||
            ranges > SVGA3D_MAX_DRAW_PRIMITIVE_RANGES)
            return false;
        uint32_t expected;
        if (!drm_vmw_execbuf_mul_add(sizeof(SVGA3dCmdDrawPrimitives), decls,
                                     sizeof(SVGA3dVertexDecl), &expected) ||
            !drm_vmw_execbuf_mul_add(expected, ranges,
                                     sizeof(SVGA3dPrimitiveRange), &expected))
            return false;
        if (bytes == expected) return true;
        return drm_vmw_execbuf_mul_add(expected, decls,
                                       sizeof(SVGA3dVertexDivisor), &expected) &&
               bytes == expected;
    }
    case SVGA_3D_CMD_SETSCISSORRECT:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdSetScissorRect));
    case SVGA_3D_CMD_BEGIN_QUERY:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdBeginQuery));
    case SVGA_3D_CMD_END_QUERY:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdEndQuery));
    case SVGA_3D_CMD_WAIT_FOR_QUERY:
        return drm_vmw_execbuf_size_is(bytes, sizeof(SVGA3dCmdWaitForQuery));
    default:
        return false;
    }
}

int drm_vmwgfx_validate_stream(const void *stream, uint32_t bytes,
                               uint32_t *packet_count)
{
    return drm_vmwgfx_walk_stream(stream, bytes, packet_count, NULL, NULL);
}

int drm_vmwgfx_walk_stream(const void *stream, uint32_t bytes,
                           uint32_t *packet_count,
                           drm_vmwgfx_packet_visitor visitor,
                           void *context)
{
    if (!packet_count) return -LINUX_EINVAL;
    *packet_count = 0;
    if (bytes > DRM_VMW_EXECBUF_MAX_COMMAND_BYTES || (bytes & 3u) ||
        (bytes && !stream))
        return -LINUX_EINVAL;
    if (!bytes) return 0;

    /* A visitor may acquire typed references or stage translated state. Do
     * not expose any packet until the entire immutable stream is structurally
     * valid, so a malformed tail cannot leave a processed prefix behind. */
    if (visitor) {
        uint32_t validated_packets = 0;
        int status = drm_vmwgfx_walk_stream(stream, bytes,
            &validated_packets, NULL, NULL);
        if (status) return status;
    }

    const uint8_t *cursor = stream;
    uint32_t remaining = bytes;
    uint32_t count = 0;
    while (remaining) {
        if (remaining < sizeof(struct drm_vmw_execbuf_packet_header))
            return -LINUX_EINVAL;
        uint32_t command = drm_vmw_execbuf_load_u32(cursor);
        uint32_t payload_bytes = drm_vmw_execbuf_load_u32(
            cursor + sizeof(uint32_t));
        uint32_t packet_bytes;
        if ((payload_bytes & 3u) || payload_bytes > remaining - 8u ||
            !drm_vmw_execbuf_mul_add(8u, payload_bytes, 1u, &packet_bytes))
            return -LINUX_EINVAL;
        if (!drm_vmw_execbuf_packet_shape(command, cursor + 8u,
                                          payload_bytes) ||
            !drm_vmw_execbuf_fields_valid(command, cursor + 8u,
                                           payload_bytes))
            return -LINUX_EINVAL;
        if (visitor) {
            int status = visitor(command, cursor + 8u, payload_bytes,
                                 bytes - remaining, context);
            if (status) return status;
        }
        cursor += packet_bytes;
        remaining -= packet_bytes;
        if (count == UINT32_MAX) return -LINUX_EINVAL;
        ++count;
    }
    *packet_count = count;
    return 0;
}
