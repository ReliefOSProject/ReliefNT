/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "render.h"
#include <reliefnt/heap.h>

#define SVGA_MAX_BLOBS 256u
#define SVGA_MAX_BLOB_BYTES (64u * 1024u)

struct svga_blob {
    struct svga_blob *next;
    struct drm_file *owner;
    uint32_t id, refs, length;
    uint8_t payload[];
};

/* Blob IDs are in the same namespace as KMS object IDs. Both lookup and
 * reference transfer are serialized by DRM execution ownership. */
static struct svga_blob *blobs;
static unsigned blob_count;
static uint32_t next_blob = SVGA_BLOB_ID_BASE;

int svga_blob_alloc(struct drm_file *owner, uint32_t length, struct svga_blob **out)
{
    if (!length || length > SVGA_MAX_BLOB_BYTES)
        return -EINVAL;
    /* Leave room for every CRTC to create a mode from the legacy ioctl. */
    if (blob_count >= SVGA_MAX_BLOBS - (owner ? SVGA_MAX_OUTPUTS : 0) ||
        !next_blob)
        return -ENOSPC;
    struct svga_blob *blob = kernel_malloc(sizeof(*blob) + length);
    if (!blob)
        return -ENOMEM;
    *blob = (struct svga_blob){.next = blobs, .owner = owner, .id = next_blob++,
                               .refs = 1, .length = length};
    blobs = blob;
    ++blob_count;
    *out = blob;
    return 0;
}

void svga_blob_get(struct svga_blob *blob)
{
    if (blob)
        ++blob->refs;
}

void svga_blob_put(struct svga_blob *blob)
{
    if (!blob || --blob->refs)
        return;
    struct svga_blob **link = &blobs;
    while (*link != blob)
        link = &(*link)->next;
    *link = blob->next;
    --blob_count;
    kernel_free(blob);
}

uint32_t svga_blob_id(const struct svga_blob *blob)
{
    return blob ? blob->id : 0;
}

void *svga_blob_data(struct svga_blob *blob)
{
    return blob->payload;
}

static struct svga_blob *lookup(uint32_t id)
{
    for (struct svga_blob *blob = blobs; blob; blob = blob->next)
        if (blob->id == id)
            return blob;
    return NULL;
}

struct svga_blob *svga_blob_lookup(uint32_t id, uint32_t length)
{
    struct svga_blob *blob = lookup(id);
    return blob && blob->length == length ? blob : NULL;
}

struct svga_blob *svga_damage_lookup(uint32_t id)
{
    struct svga_blob *blob = lookup(id);
    return blob && blob->length % sizeof(struct drm_mode_rect) == 0 ? blob : NULL;
}

const struct drm_mode_rect *svga_damage_rects(const struct svga_blob *blob, unsigned *count)
{
    *count = blob->length / sizeof(struct drm_mode_rect);
    return (const struct drm_mode_rect *)blob->payload;
}

int svga_blob_destroy(struct drm_file *file, uint32_t id)
{
    struct svga_blob *blob = lookup(id);
    if (!blob)
        return -ENOENT;
    if (blob->owner != file)
        return -EPERM;
    blob->owner = NULL;
    svga_blob_put(blob);
    return 0;
}

void svga_blobs_release(struct drm_file *file)
{
    for (struct svga_blob *blob = blobs, *next; blob; blob = next) {
        next = blob->next;
        if (blob->owner == file) {
            blob->owner = NULL;
            svga_blob_put(blob);
        }
    }
}

int svga_property_blob_ioctl(struct task *task, struct drm_file *file, uint32_t command, void *data)
{
    if (command == DRM_IOCTL_MODE_CREATEPROPBLOB) {
        struct drm_mode_create_blob *arg = data;
        struct svga_blob *blob;
        int ret = svga_blob_alloc(file, arg->length, &blob);
        if (ret)
            return ret;
        ret = svga_import(task, blob->payload, arg->data, blob->length);
        if (ret) {
            svga_blob_put(blob);
            return ret;
        }
        arg->blob_id = blob->id;
        return 0;
    }
    if (command == DRM_IOCTL_MODE_DESTROYPROPBLOB)
        return svga_blob_destroy(file, ((const struct drm_mode_destroy_blob *)data)->blob_id);

    struct drm_mode_get_blob *arg = data;
    const struct svga_blob *blob = lookup(arg->blob_id);
    if (!blob)
        return -ENOENT;
    int ret = arg->length == blob->length ?
        drm_kms_copy_array(task, arg->data, arg->length, blob->payload, blob->length, 1) : 0;
    arg->length = blob->length;
    return ret;
}

struct atomic_property {
    const char *name;
    uint32_t flags, count;
    uint64_t values[2];
};

static const struct atomic_property properties[] = {
    [SVGA_PROP_ACTIVE - SVGA_PROP_ACTIVE] = {"ACTIVE", DRM_MODE_PROP_RANGE, 2, {0, 1}},
    [SVGA_PROP_MODE_ID - SVGA_PROP_ACTIVE] = {"MODE_ID", DRM_MODE_PROP_BLOB, 0, {0}},
    [SVGA_PROP_CONNECTOR_CRTC - SVGA_PROP_ACTIVE] = {"CRTC_ID", DRM_MODE_PROP_OBJECT, 1, {DRM_MODE_OBJECT_CRTC}},
    [SVGA_PROP_PLANE_CRTC - SVGA_PROP_ACTIVE] = {"CRTC_ID", DRM_MODE_PROP_OBJECT, 1, {DRM_MODE_OBJECT_CRTC}},
    [SVGA_PROP_FB_ID - SVGA_PROP_ACTIVE] = {"FB_ID", DRM_MODE_PROP_OBJECT, 1, {DRM_MODE_OBJECT_FB}},
    [SVGA_PROP_SRC_X - SVGA_PROP_ACTIVE] = {"SRC_X", DRM_MODE_PROP_RANGE, 2, {0, UINT32_MAX}},
    [SVGA_PROP_SRC_Y - SVGA_PROP_ACTIVE] = {"SRC_Y", DRM_MODE_PROP_RANGE, 2, {0, UINT32_MAX}},
    [SVGA_PROP_SRC_W - SVGA_PROP_ACTIVE] = {"SRC_W", DRM_MODE_PROP_RANGE, 2, {0, UINT32_MAX}},
    [SVGA_PROP_SRC_H - SVGA_PROP_ACTIVE] = {"SRC_H", DRM_MODE_PROP_RANGE, 2, {0, UINT32_MAX}},
    [SVGA_PROP_CRTC_X - SVGA_PROP_ACTIVE] = {"CRTC_X", DRM_MODE_PROP_SIGNED_RANGE, 2, {INT32_MIN, INT32_MAX}},
    [SVGA_PROP_CRTC_Y - SVGA_PROP_ACTIVE] = {"CRTC_Y", DRM_MODE_PROP_SIGNED_RANGE, 2, {INT32_MIN, INT32_MAX}},
    [SVGA_PROP_CRTC_W - SVGA_PROP_ACTIVE] = {"CRTC_W", DRM_MODE_PROP_RANGE, 2, {0, INT32_MAX}},
    [SVGA_PROP_CRTC_H - SVGA_PROP_ACTIVE] = {"CRTC_H", DRM_MODE_PROP_RANGE, 2, {0, INT32_MAX}},
    [SVGA_PROP_DAMAGE_CLIPS - SVGA_PROP_ACTIVE] = {"FB_DAMAGE_CLIPS", DRM_MODE_PROP_BLOB, 0, {0}},
    [SVGA_PROP_HOTSPOT_X - SVGA_PROP_ACTIVE] = {"HOTSPOT_X", DRM_MODE_PROP_SIGNED_RANGE, 2, {INT32_MIN, INT32_MAX}},
    [SVGA_PROP_HOTSPOT_Y - SVGA_PROP_ACTIVE] = {"HOTSPOT_Y", DRM_MODE_PROP_SIGNED_RANGE, 2, {INT32_MIN, INT32_MAX}},
};

int svga_atomic_property(struct task *task, struct drm_mode_get_property *arg)
{
    unsigned index = arg->prop_id - SVGA_PROP_ACTIVE;
    if (index >= sizeof(properties) / sizeof(properties[0]))
        return -ENOENT;
    const struct atomic_property *prop = &properties[index];
    int ret = drm_kms_copy_array(task, arg->values_ptr, arg->count_values,
                                 prop->values, prop->count, sizeof(prop->values[0]));
    arg->count_values = prop->count;
    arg->count_enum_blobs = 0;
    arg->flags = prop->flags | DRM_MODE_PROP_ATOMIC;
    __builtin_memset(arg->name, 0, sizeof(arg->name));
    __builtin_memcpy(arg->name, prop->name, __builtin_strlen(prop->name));
    return ret;
}

unsigned svga_atomic_properties(uint32_t type, unsigned unit, bool cursor,
                                uint32_t *ids, uint64_t *values)
{
    const struct svga_scanout *s = &svga_scanouts[unit];
    uint32_t crtc = SVGA_CRTC_ID + unit;
    unsigned count = 0;
#define PROP(id, value) do { ids[count] = SVGA_PROP_##id; values[count++] = (value); } while (0)
    switch (type) {
    case DRM_MODE_OBJECT_CRTC:
        PROP(ACTIVE, s->active);
        PROP(MODE_ID, svga_blob_id(s->mode_blob));
        break;
    case DRM_MODE_OBJECT_CONNECTOR:
        PROP(CONNECTOR_CRTC, s->connector_routed ? crtc : 0);
        break;
    case DRM_MODE_OBJECT_PLANE:
        if (cursor) {
            const struct svga_cursor_plane *c = &s->cursor;
            PROP(PLANE_CRTC, c->routed ? crtc : 0);
            PROP(FB_ID, c->fb ? c->fb->id : 0);
            PROP(SRC_X, c->src_x);
            PROP(SRC_Y, c->src_y);
            PROP(SRC_W, c->src_w);
            PROP(SRC_H, c->src_h);
            PROP(CRTC_X, (int64_t)c->x);
            PROP(CRTC_Y, (int64_t)c->y);
            PROP(CRTC_W, c->width);
            PROP(CRTC_H, c->height);
            PROP(HOTSPOT_X, (int64_t)c->hot_x);
            PROP(HOTSPOT_Y, (int64_t)c->hot_y);
        } else {
            PROP(PLANE_CRTC, s->plane_routed ? crtc : 0);
            PROP(FB_ID, s->fb ? s->fb->id : 0);
            PROP(SRC_X, s->x << 16);
            PROP(SRC_Y, s->y << 16);
            PROP(SRC_W, s->src_w);
            PROP(SRC_H, s->src_h);
            PROP(CRTC_X, (int64_t)s->dst_x);
            PROP(CRTC_Y, (int64_t)s->dst_y);
            PROP(CRTC_W, s->dst_w);
            PROP(CRTC_H, s->dst_h);
            PROP(DAMAGE_CLIPS, svga_blob_id(s->damage_blob));
        }
        break;
    }
#undef PROP
    return count;
}
