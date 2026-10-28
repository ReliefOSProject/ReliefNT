/* SPDX-License-Identifier: MIT
 * VMware SVGA GB wire definitions, adapted from Linux vmwgfx/device_include/
 * svga3d_cmd.h, svga3d_types.h and svga_reg.h (retrieved 2026-09-05).
 * Copyright 2012-2023 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef RELIEFOS_SVGA_GB_PROTOCOL_H
#define RELIEFOS_SVGA_GB_PROTOCOL_H

#define SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB 51u
#define SVGA_REG_MOB_MAX_SIZE 57u
#define SVGA_REG_CAP2 59u
#define SVGA_REG_GBOBJECT_MEM_SIZE_KB 76u
#define SVGA_CAP_CAP2_REGISTER 0x80000000u
#define SVGA_CAP2_GB_MEMSIZE_2 0x00000008u
#define SVGA_3D_CMD_SET_OTABLE_BASE 1091u
#define SVGA_3D_CMD_DEFINE_GB_MOB 1093u
#define SVGA_3D_CMD_DESTROY_GB_MOB 1094u
#define SVGA_3D_CMD_DEFINE_GB_SURFACE 1097u
#define SVGA_3D_CMD_DESTROY_GB_SURFACE 1098u
#define SVGA_3D_CMD_BIND_GB_SURFACE 1099u
#define SVGA_3D_CMD_UPDATE_GB_SURFACE 1102u
#define SVGA_3D_CMD_INVALIDATE_GB_SURFACE 1106u
#define SVGA_3D_CMD_DEFINE_GB_CONTEXT 1107u
#define SVGA_3D_CMD_DESTROY_GB_CONTEXT 1108u
#define SVGA_3D_CMD_BIND_GB_CONTEXT 1109u
#define SVGA_3D_CMD_DEFINE_GB_SHADER 1112u
#define SVGA_3D_CMD_DESTROY_GB_SHADER 1113u
#define SVGA_3D_CMD_BIND_GB_SHADER 1114u
#define SVGA_3D_CMD_BEGIN_GB_QUERY 1116u
#define SVGA_3D_CMD_END_GB_QUERY 1117u
#define SVGA_3D_CMD_WAIT_FOR_GB_QUERY 1118u
#define SVGA_3D_CMD_SET_GB_SHADERCONSTS_INLINE 1130u
#define SVGA_OTABLE_MOB 0u
#define SVGA_OTABLE_SURFACE 1u
#define SVGA_OTABLE_CONTEXT 2u
#define SVGA_OTABLE_SHADER 3u
#define SVGA_OTABLE_SCREENTARGET 4u
#define SVGA_OTABLE_DX9_MAX 5u
#define SVGA3D_MOBFMT_INVALID UINT32_MAX
#define SVGA3D_MOBFMT_PT_0 0u
#define SVGA3D_MOBFMT_PT_1 1u
#define SVGA3D_MOBFMT_PT_2 2u

/* sizeof(SVGAGBContextData), verified against the upstream packed layout.
 * The host initializes this opaque state with BindGBContext.validContents=0. */
#define SVGA_GB_CONTEXT_BYTES 16384u
/* Packed OTable entry strides, not sizes of the DEFINE command payloads. */
#define SVGA_GB_MOB_ENTRY_BYTES 16u
#define SVGA_GB_SURFACE_ENTRY_BYTES 64u
#define SVGA_GB_CONTEXT_ENTRY_BYTES 8u
#define SVGA_GB_SHADER_ENTRY_BYTES 16u
#define SVGA_GB_SCREENTARGET_ENTRY_BYTES 64u
#define SVGA_GB_QUERY_RESULT_BYTES 12u
#define SVGA_GB_QUERY_TYPE_OCCLUSION 0u
#define SVGA_GB_SHADER_TYPE_VS 1u
#define SVGA_GB_SHADER_TYPE_PS 2u
#define SVGA_GB_CONST_FLOAT 0u
#define SVGA_GB_CONST_INT 1u
#define SVGA_GB_CONST_BOOL 2u

typedef struct {
    uint32_t type, baseAddress, sizeInBytes, validSizeInBytes, ptDepth;
} SVGA3dCmdSetOTableBase;
typedef struct {
    uint32_t mobid, ptDepth, base, sizeInBytes;
} SVGA3dCmdDefineGBMob;
typedef struct {
    uint32_t sid, surfaceFlags, format, numMipLevels, multisampleCount, autogenFilter;
    SVGA3dSize size;
} SVGA3dCmdDefineGBSurface;
typedef struct { uint32_t sid, mobid; } SVGA3dCmdBindGBSurface;
typedef struct { uint32_t cid, mobid, validContents; } SVGA3dCmdBindGBContext;
typedef struct { uint32_t shid, type, sizeInBytes; } SVGA3dCmdDefineGBShader;
typedef struct { uint32_t shid, mobid, offsetInBytes; } SVGA3dCmdBindGBShader;
typedef struct { uint32_t shid; } SVGA3dCmdDestroyGBShader;
typedef struct { uint32_t cid, type; } SVGA3dCmdBeginGBQuery;
typedef struct { uint32_t cid, type, mobid, offset; } SVGA3dCmdEndGBQuery;
typedef struct { uint32_t cid, type, mobid, offset; } SVGA3dCmdWaitForGBQuery;

_Static_assert(sizeof(SVGA3dCmdSetOTableBase) == 20, "OTable wire size");
_Static_assert(sizeof(SVGA3dCmdDefineGBMob) == 16, "MOB wire size");
_Static_assert(sizeof(SVGA3dCmdDefineGBSurface) == 36, "GB surface wire size");
_Static_assert(sizeof(SVGA3dCmdBindGBSurface) == 8, "GB surface binding wire size");
_Static_assert(sizeof(SVGA3dCmdBindGBContext) == 12, "GB context binding wire size");
_Static_assert(sizeof(SVGA3dCmdDefineGBShader) == 12, "GB shader definition wire size");
_Static_assert(sizeof(SVGA3dCmdBindGBShader) == 12, "GB shader binding wire size");
_Static_assert(sizeof(SVGA3dCmdDestroyGBShader) == 4, "GB shader destroy wire size");
_Static_assert(sizeof(SVGA3dCmdBeginGBQuery) == 8, "GB query begin wire size");
_Static_assert(sizeof(SVGA3dCmdEndGBQuery) == 16, "GB query end wire size");
_Static_assert(sizeof(SVGA3dCmdWaitForGBQuery) == 16, "GB query wait wire size");
_Static_assert(sizeof(SVGA3dQueryResult) == SVGA_GB_QUERY_RESULT_BYTES, "GB query result size");
#endif
