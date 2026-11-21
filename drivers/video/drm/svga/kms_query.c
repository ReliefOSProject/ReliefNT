/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/graphics_service.h>
#include <reliefnt/text_stream.h>

static void make_mode(struct drm_mode_modeinfo *mode, uint32_t width,
                      uint32_t height, bool preferred)
{
    *mode = (struct drm_mode_modeinfo){
        .hdisplay = width, .hsync_start = width + 48, .hsync_end = width + 80,
        .htotal = width + 160, .vdisplay = height, .vsync_start = height + 3,
        .vsync_end = height + 8, .vtotal = height + 45, .vrefresh = 60,
        .clock = (width + 160) * (height + 45) * 60 / 1000,
        .flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
        .type = DRM_MODE_TYPE_DRIVER | (preferred ? DRM_MODE_TYPE_PREFERRED : 0),
    };
    struct text_stream name = {.buffer = mode->name, .capacity = sizeof(mode->name) - 1};
    text_unsigned(&name, width);
    text_string(&name, "x");
    text_unsigned(&name, height);
}

static const uint16_t common_sizes[][2] = {
    {1920, 1080}, {1600, 900}, {1280, 1024}, {1280, 720}, {1024, 768},
    {800, 600}, {640, 480},
};
#define MODE_COUNT (1 + sizeof(common_sizes) / sizeof(common_sizes[0]))

static unsigned modes_get(unsigned unit, struct drm_mode_modeinfo modes[MODE_COUNT])
{
    if (!svga_output_connected(unit))
        return 0;
    unsigned count = 0;
    const struct drm_vmw_rect *preferred = &svga_layout[unit];
    if (svga_mode_fits(preferred->w, preferred->h))
        make_mode(&modes[count++], preferred->w, preferred->h, true);
    for (unsigned i = 0; i < MODE_COUNT - 1; ++i) {
        uint32_t w = common_sizes[i][0], h = common_sizes[i][1];
        if (!svga_mode_fits(w, h) ||
            (count && w == modes[0].hdisplay && h == modes[0].vdisplay))
            continue;
        make_mode(&modes[count], w, h, count == 0);
        ++count;
    }
    return count;
}

int svga_connector_info(uint32_t index, struct graphics_connector_info *out)
{
    if (index >= svga_num_outputs)
        return -ENOENT;
    struct drm_mode_modeinfo modes[MODE_COUNT];
    unsigned count = modes_get(index, modes);
    *out = (struct graphics_connector_info){
        .connected = svga_output_connected(index),
        .enabled = svga_kms_active() && svga_scanout_visible(&svga_scanouts[index]),
    };
    for (unsigned i = 0; i < count && i < sizeof(out->widths) / sizeof(out->widths[0]); ++i) {
        out->widths[i] = modes[i].hdisplay;
        out->heights[i] = modes[i].vdisplay;
        ++out->mode_count;
    }
    return 0;
}

static int resources(struct task *task, struct drm_file *file, struct drm_mode_card_res *arg)
{
    uint32_t crtc[SVGA_MAX_OUTPUTS], encoder[SVGA_MAX_OUTPUTS], connector[SVGA_MAX_OUTPUTS];
    for (unsigned i = 0; i < svga_num_outputs; ++i) {
        crtc[i] = SVGA_CRTC_ID + i;
        encoder[i] = SVGA_ENCODER_ID + i;
        connector[i] = SVGA_CONNECTOR_ID + i;
    }
    uint32_t fbs[SVGA_MAX_FBS];
    unsigned count = 0;
    for (struct svga_fb *fb = file->svga ? file->svga->fbs : NULL; fb; fb = fb->next)
        fbs[count++] = fb->id;
    int ret = drm_kms_copy_array(task, arg->fb_id_ptr, arg->count_fbs, fbs, count, sizeof(fbs[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->crtc_id_ptr, arg->count_crtcs, crtc, svga_num_outputs, sizeof(crtc[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->encoder_id_ptr, arg->count_encoders, encoder, svga_num_outputs, sizeof(encoder[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->connector_id_ptr, arg->count_connectors, connector, svga_num_outputs, sizeof(connector[0]));
    arg->count_crtcs = arg->count_encoders = arg->count_connectors = svga_num_outputs;
    arg->count_fbs = count;
    arg->min_width = arg->min_height = 1;
    arg->max_width = svga.max_width;
    arg->max_height = svga.max_height;
    return ret;
}

static int connector_properties(struct task *task, const struct drm_file *file, unsigned unit,
                                uint64_t props_ptr, uint64_t values_ptr, uint32_t *count)
{
    uint32_t props[4] = {SVGA_SUGGESTED_X_ID, SVGA_SUGGESTED_Y_ID, SVGA_HOTPLUG_MODE_ID};
    uint64_t values[4] = {svga_layout[unit].x, svga_layout[unit].y, 1};
    unsigned size = 3;
    if (file->atomic_modeset)
        size += svga_atomic_properties(DRM_MODE_OBJECT_CONNECTOR, unit, false, props + size, values + size);
    int ret = drm_kms_copy_array(task, props_ptr, *count, props, size, sizeof(props[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, values_ptr, *count, values, size, sizeof(values[0]));
    *count = size;
    return ret;
}

static int connector(struct task *task, const struct drm_file *file, struct drm_mode_get_connector *arg)
{
    int unit = svga_output_index(arg->connector_id, SVGA_CONNECTOR_ID);
    if (unit < 0)
        return -ENOENT;
    struct drm_mode_modeinfo modes[MODE_COUNT];
    uint32_t encoder = SVGA_ENCODER_ID + unit;
    unsigned count = modes_get(unit, modes);
    int ret = drm_kms_copy_array(task, arg->modes_ptr, arg->count_modes, modes, count, sizeof(modes[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->encoders_ptr, arg->count_encoders, &encoder, 1, sizeof(encoder));
    if (!ret)
        ret = connector_properties(task, file, unit, arg->props_ptr, arg->prop_values_ptr, &arg->count_props);
    arg->count_modes = count;
    arg->count_encoders = 1;
    arg->encoder_id = svga_scanouts[unit].connector_routed ? encoder : 0;
    arg->connector_type = DRM_MODE_CONNECTOR_VIRTUAL;
    arg->connector_type_id = unit + 1;
    arg->connection = svga_output_connected(unit) ? 1 : 2;
    arg->subpixel = 1;   /* unknown */
    arg->mm_width = arg->mm_height = 0;
    return ret;
}

static int connector_property(struct task *task, struct drm_mode_get_property *arg)
{
    const char *name;
    uint64_t values[] = {0, INT32_MAX};
    switch (arg->prop_id) {
    case SVGA_SUGGESTED_X_ID: name = "suggested X"; break;
    case SVGA_SUGGESTED_Y_ID: name = "suggested Y"; break;
    case SVGA_HOTPLUG_MODE_ID: name = "hotplug_mode_update"; values[1] = 1; break;
    default: return -ENOENT;
    }
    int ret = drm_kms_copy_array(task, arg->values_ptr, arg->count_values, values, 2, sizeof(values[0]));
    arg->count_values = 2;
    arg->count_enum_blobs = 0;
    arg->flags = DRM_MODE_PROP_RANGE | DRM_MODE_PROP_IMMUTABLE;
    __builtin_memset(arg->name, 0, sizeof(arg->name));
    __builtin_memcpy(arg->name, name, __builtin_strlen(name));
    return ret;
}

static int plane_property(struct task *task, struct drm_mode_get_property *arg)
{
    if (arg->prop_id != SVGA_PLANE_TYPE_ID)
        return -ENOENT;
    struct drm_mode_property_enum kinds[] = {
        {.value = 0, .name = "Overlay"}, {.value = 1, .name = "Primary"},
        {.value = 2, .name = "Cursor"},
    };
    const uint64_t values[] = {0, 1, 2};
    int ret = drm_kms_copy_array(task, arg->values_ptr, arg->count_values, values, 3, sizeof(values[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->enum_blob_ptr, arg->count_enum_blobs, kinds, 3, sizeof(kinds[0]));
    arg->count_values = arg->count_enum_blobs = 3;
    arg->flags = DRM_MODE_PROP_ENUM | DRM_MODE_PROP_IMMUTABLE;
    __builtin_memset(arg->name, 0, sizeof(arg->name));
    __builtin_memcpy(arg->name, "type", 5);
    return ret;
}

static int plane_index(const struct drm_file *file, uint32_t id, bool *cursor)
{
    int unit = svga_output_index(id, SVGA_PLANE_ID);
    *cursor = false;
    if (unit < 0 && svga_cursor_plane_available(file)) {
        unit = svga_output_index(id, SVGA_CURSOR_PLANE_ID);
        *cursor = unit >= 0;
    }
    return unit;
}

static int object_properties(struct task *task, const struct drm_file *file,
                             struct drm_mode_obj_get_properties *arg)
{
    unsigned count = 0;
    uint32_t props[1 + SVGA_PROP_END - SVGA_PROP_ACTIVE];
    uint64_t values[1 + SVGA_PROP_END - SVGA_PROP_ACTIVE];
    bool cursor = false;
    int unit;
    switch (arg->obj_type) {
    case DRM_MODE_OBJECT_CONNECTOR:
        unit = svga_output_index(arg->obj_id, SVGA_CONNECTOR_ID);
        if (unit < 0)
            return -ENOENT;
        return connector_properties(task, file, unit, arg->props_ptr, arg->prop_values_ptr, &arg->count_props);
    case DRM_MODE_OBJECT_ENCODER:
        unit = svga_output_index(arg->obj_id, SVGA_ENCODER_ID);
        break;
    case DRM_MODE_OBJECT_CRTC:
        unit = svga_output_index(arg->obj_id, SVGA_CRTC_ID);
        break;
    case DRM_MODE_OBJECT_PLANE:
        unit = plane_index(file, arg->obj_id, &cursor);
        props[0] = SVGA_PLANE_TYPE_ID;
        values[0] = cursor ? 2 : 1;
        count = 1;
        break;
    default:
        return -ENOENT;
    }
    if (unit < 0)
        return -ENOENT;
    if (file->atomic_modeset)
        count += svga_atomic_properties(arg->obj_type, unit, cursor, props + count, values + count);
    int ret = drm_kms_copy_array(task, arg->props_ptr, arg->count_props, props, count, sizeof(props[0]));
    if (!ret)
        ret = drm_kms_copy_array(task, arg->prop_values_ptr, arg->count_props, values, count, sizeof(values[0]));
    arg->count_props = count;
    return ret;
}

int svga_kms_query(struct task *task, struct drm_file *file, uint32_t command, void *data)
{
    switch (command) {
    case DRM_IOCTL_MODE_GETRESOURCES:
        return resources(task, file, data);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return connector(task, file, data);
    case DRM_IOCTL_MODE_GETENCODER: {
        struct drm_mode_get_encoder *arg = data;
        int unit = svga_output_index(arg->encoder_id, SVGA_ENCODER_ID);
        if (unit < 0)
            return -ENOENT;
        arg->encoder_type = DRM_MODE_ENCODER_VIRTUAL;
        arg->crtc_id = svga_scanouts[unit].connector_routed ? SVGA_CRTC_ID + unit : 0;
        arg->possible_crtcs = arg->possible_clones = 1u << unit;
        return 0;
    }
    case DRM_IOCTL_MODE_GETCRTC: {
        struct drm_mode_crtc *arg = data;
        int unit = svga_output_index(arg->crtc_id, SVGA_CRTC_ID);
        if (unit < 0)
            return -ENOENT;
        const struct svga_scanout *scanout = &svga_scanouts[unit];
        arg->mode_valid = scanout->mode_blob != NULL;
        arg->fb_id = scanout->fb ? scanout->fb->id : 0;
        arg->x = scanout->x;
        arg->y = scanout->y;
        arg->gamma_size = 0;
        arg->mode = scanout->mode;
        return 0;
    }
    case DRM_IOCTL_MODE_GETPLANERESOURCES: {
        struct drm_mode_get_plane_res *arg = data;
        uint32_t planes[2 * SVGA_MAX_OUTPUTS];
        uint32_t count = file->universal_planes ? svga_num_outputs : 0;
        for (unsigned i = 0; i < count; ++i)
            planes[i] = SVGA_PLANE_ID + i;
        if (count && svga_cursor_plane_available(file)) {
            for (unsigned i = 0; i < svga_num_outputs; ++i)
                planes[count++] = SVGA_CURSOR_PLANE_ID + i;
        }
        int ret = drm_kms_copy_array(task, arg->plane_id_ptr, arg->count_planes, planes, count, sizeof(planes[0]));
        arg->count_planes = count;
        return ret;
    }
    case DRM_IOCTL_MODE_GETPLANE: {
        struct drm_mode_get_plane *arg = data;
        bool cursor;
        int unit = plane_index(file, arg->plane_id, &cursor);
        if (unit < 0)
            return -ENOENT;
        const uint32_t formats[] = {SVGA_XRGB8888, SVGA_ARGB8888};
        uint32_t count = cursor ? 1 : 2;
        int ret = drm_kms_copy_array(task, arg->format_type_ptr, arg->count_format_types,
                                     cursor ? formats + 1 : formats, count, sizeof(formats[0]));
        arg->count_format_types = count;
        const struct svga_scanout *scanout = &svga_scanouts[unit];
        const struct svga_fb *fb = cursor ? scanout->cursor.fb : scanout->fb;
        arg->crtc_id = fb ? SVGA_CRTC_ID + unit : 0;
        arg->fb_id = fb ? fb->id : 0;
        arg->possible_crtcs = 1u << unit;
        arg->gamma_size = 0;
        return ret;
    }
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return object_properties(task, file, data);
    case DRM_IOCTL_MODE_GETPROPERTY: {
        const struct drm_mode_get_property *arg = data;
        if (arg->prop_id >= SVGA_PROP_ACTIVE)
            return file->atomic_modeset ? svga_atomic_property(task, data) : -ENOENT;
        return arg->prop_id == SVGA_PLANE_TYPE_ID ? plane_property(task, data) : connector_property(task, data);
    }
    default:
        return -EOPNOTSUPP;
    }
}
