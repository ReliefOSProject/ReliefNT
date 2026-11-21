/* SPDX-License-Identifier: Apache-2.0 */
#include "exec.h"
#include "kms.h"
#include <reliefnt/heap.h>

#define OBJECT_BUCKETS 2048u
#define BITMAP_WORDS(count) (((count) + 63u) / 64u)

static struct svga_object *objects[OBJECT_BUCKETS];
struct svga_surface_ref {
    struct svga_surface_ref *hash_next, *next, **previous;
    struct svga_surface *surface;
    struct svga_render_file *file;
    uint32_t count;
};

static struct svga_surface_ref *imports[OBJECT_BUCKETS];
static uint64_t context_ids[BITMAP_WORDS(SVGA3D_HB_MAX_CONTEXT_IDS)];
static uint64_t surface_ids[BITMAP_WORDS(SVGA3D_HB_MAX_SURFACE_IDS)];
static uint64_t shader_ids[BITMAP_WORDS(SVGA3D_MAX_SIMULTANEOUS_SHADERS)];
static uint32_t next_handle = SVGA_RENDER_HANDLE_BASE;

static struct object_ids {
    uint64_t *bits;
    uint32_t limit;
} const ids[] = {
    [SVGA_CONTEXT] = {context_ids, SVGA3D_HB_MAX_CONTEXT_IDS},
    [SVGA_SURFACE] = {surface_ids, SVGA3D_HB_MAX_SURFACE_IDS},
    [SVGA_SHADER] = {shader_ids, SVGA3D_MAX_SIMULTANEOUS_SHADERS},
    [SVGA_FENCE] = {NULL, 0},
};

static uint32_t alloc_id(enum svga_object_type type)
{
    const struct object_ids *pool = &ids[type];
    if (!pool->bits)
        return 0;
    for (unsigned word = 0; word < BITMAP_WORDS(pool->limit); ++word) {
        /* Internal display surfaces use the first SVGA_MAX_OUTPUTS IDs. */
        unsigned reserved = type == SVGA_SURFACE ? SVGA_MAX_OUTPUTS : 1;
        uint64_t available = ~pool->bits[word];
        if (!word)
            available &= UINT64_MAX << reserved;
        if (!available)
            continue;
        unsigned bit = __builtin_ctzll(available);
        unsigned id = word * 64 + bit;
        if (id >= pool->limit)
            break;
        pool->bits[word] |= UINT64_C(1) << bit;
        return id;
    }
    return SVGA3D_INVALID_ID;
}

static void free_id(const struct svga_object *object)
{
    uint64_t *bits = ids[object->type].bits;
    if (bits)
        bits[object->id / 64] &= ~(UINT64_C(1) << (object->id % 64));
}

static struct svga_object **object_link(uint32_t handle)
{
    struct svga_object **link = &objects[handle % OBJECT_BUCKETS];
    while (*link && (*link)->handle != handle)
        link = &(*link)->hash_next;
    return link;
}

static struct svga_surface_ref **import_link(const struct svga_render_file *file, uint32_t handle)
{
    struct svga_surface_ref **link = &imports[handle % OBJECT_BUCKETS];
    while (*link && ((*link)->file != file || (*link)->surface->object.handle != handle))
        link = &(*link)->hash_next;
    return link;
}

struct svga_render_file *svga_render_file_get(struct drm_file *file)
{
    if (!file->svga_render) {
        struct svga_render_file *state = kernel_malloc(sizeof(*state));
        if (!state)
            return NULL;
        *state = (struct svga_render_file){0};
        file->svga_render = state;
    }
    return file->svga_render;
}

struct svga_object *svga_object_create(struct drm_file *file, enum svga_object_type type,
                                       size_t bytes)
{
    struct svga_render_file *state = svga_render_file_get(file);
    if (!state || state->object_count == SVGA_RENDER_MAX_OBJECTS)
        return NULL;
    struct svga_object *object = kernel_malloc(bytes);
    if (!object)
        return NULL;
    uint32_t id = alloc_id(type);
    if (id == SVGA3D_INVALID_ID) {
        kernel_free(object);
        return NULL;
    }
    uint32_t handle;
    do {
        handle = next_handle++;
        /* The original context and surface ABI uses signed 32-bit IDs. */
        if (next_handle > INT32_MAX)
            next_handle = SVGA_RENDER_HANDLE_BASE;
    } while (*object_link(handle));
    __builtin_memset(object, 0, bytes);
    *object = (struct svga_object){
        .hash_next = objects[handle % OBJECT_BUCKETS], .owner = state,
        .owner_next = state->objects, .owner_previous = &state->objects,
        .type = type, .handle = handle, .id = id, .references = 1, .handle_refs = 1,
    };
    objects[handle % OBJECT_BUCKETS] = object;
    if (state->objects)
        state->objects->owner_previous = &object->owner_next;
    state->objects = object;
    ++state->object_count;
    return object;
}

struct svga_object *svga_object_lookup(const struct drm_file *file, enum svga_object_type type,
                                       uint32_t handle)
{
    if (handle < SVGA_RENDER_HANDLE_BASE || !file->svga_render)
        return NULL;
    struct svga_object *object = *object_link(handle);
    if (!object || object->type != type)
        return NULL;
    if (object->owner == file->svga_render ||
        (type == SVGA_SURFACE && *import_link(file->svga_render, handle)))
        return object;
    return NULL;
}

/* An explicit PRIME lease can cross masters. A numeric legacy handle can
 * only add a lease within the authenticating primary master's namespace. */
struct svga_surface *svga_surface_lookup_reference(struct drm_file *file, uint32_t handle)
{
    struct svga_object *object = svga_object_lookup(file, SVGA_SURFACE, handle);
    if (object)
        return (struct svga_surface *)object;
    if (file->is_render || !file->authenticated || !file->master)
        return NULL;
    object = *object_link(handle);
    if (!object || object->type != SVGA_SURFACE)
        return NULL;
    struct svga_surface *surface = (struct svga_surface *)object;
    return surface->creator_master == file->master ? surface : NULL;
}

int svga_surface_ref(struct drm_file *file, struct svga_surface *surface)
{
    struct svga_render_file *state = svga_render_file_get(file);
    if (!state)
        return -ENOMEM;
    if (surface->object.owner == state) {
        if (surface->object.handle_refs == UINT32_MAX)
            return -EOVERFLOW;
        ++surface->object.handle_refs;
        return 0;
    }
    struct svga_surface_ref **link = import_link(state, surface->object.handle);
    if (*link) {
        if ((*link)->count == UINT32_MAX)
            return -EOVERFLOW;
        ++(*link)->count;
        return 0;
    }
    if (state->object_count == SVGA_RENDER_MAX_OBJECTS)
        return -ENOSPC;
    struct svga_surface_ref *ref = kernel_malloc(sizeof(*ref));
    if (!ref)
        return -ENOMEM;
    *ref = (struct svga_surface_ref){.surface = surface, .file = state, .count = 1,
                                    .next = state->imports, .previous = &state->imports};
    if (state->imports)
        state->imports->previous = &ref->next;
    state->imports = ref;
    *link = ref;
    ++state->object_count;
    svga_object_get(&surface->object);
    return 0;
}

static void unlink_import(struct svga_surface_ref **link)
{
    struct svga_surface_ref *ref = *link;
    *link = ref->hash_next;
    *ref->previous = ref->next;
    if (ref->next)
        ref->next->previous = ref->previous;
    --ref->file->object_count;
    svga_object_put(&ref->surface->object);
    kernel_free(ref);
}

void svga_object_get(struct svga_object *object)
{
    ++object->references;
}

static void destroy_id(struct svga_object *object, uint32_t command)
{
    const uint32_t words[] = {command, sizeof(uint32_t), object->id};
    if (object->published && svga_submit(words, sizeof(words)))
        svga.fifo.fault = true;
}

void svga_object_put(struct svga_object *object)
{
    if (--object->references)
        return;
    switch (object->type) {
    case SVGA_CONTEXT: {
        struct svga_context *ctx = (struct svga_context *)object;
        static const uint32_t commands[] = {
            [SVGA_CONTEXT_HOST] = SVGA_3D_CMD_CONTEXT_DESTROY,
            [SVGA_CONTEXT_GB] = SVGA_3D_CMD_DESTROY_GB_CONTEXT,
            [SVGA_CONTEXT_DX] = SVGA_3D_CMD_DX_DESTROY_CONTEXT,
        };
        svga_query_release(ctx);
        destroy_id(object, commands[ctx->type]);
        svga_context_release_backing(ctx);
        break;
    }
    case SVGA_SURFACE: {
        struct svga_surface *surface = (struct svga_surface *)object;
        destroy_id(object, surface->gb ? SVGA_3D_CMD_DESTROY_GB_SURFACE : SVGA_3D_CMD_SURFACE_DESTROY);
        svga_region_release(surface->backing);
        struct drm_bo *bo = surface->bo;
        svga_surface_set_bo(surface, NULL);
        drm_bo_put(bo);
        drm_master_put(surface->creator_master);
        break;
    }
    case SVGA_SHADER: {
        struct svga_shader *shader = (struct svga_shader *)object;
        destroy_id(object, SVGA_3D_CMD_DESTROY_GB_SHADER);
        svga_region_release(shader->backing);
        break;
    }
    case SVGA_FENCE:
        break;
    }
    /* The queue orders definitions issued with a recycled ID after destroy. */
    if (object->handle)
        *object_link(object->handle) = object->hash_next;
    free_id(object);
    kernel_free(object);
}

static void unlink_object(struct svga_object *object)
{
    *object->owner_previous = object->owner_next;
    if (object->owner_next)
        object->owner_next->owner_previous = object->owner_previous;
    --object->owner->object_count;
    object->owner = NULL;
    svga_object_put(object);
}

int svga_object_unref(struct drm_file *file, enum svga_object_type type, uint32_t handle)
{
    struct svga_object *object = svga_object_lookup(file, type, handle);
    if (!object)
        return -EINVAL;
    if (object->owner != file->svga_render) {
        struct svga_surface_ref **link = import_link(file->svga_render, handle);
        if (!--(*link)->count)
            unlink_import(link);
        return 0;
    }
    if (!--object->handle_refs)
        unlink_object(object);
    return 0;
}

void svga_render_file_release(struct drm_file *file)
{
    struct svga_render_file *state = file->svga_render;
    if (!state)
        return;
    svga_cpu_grabs_release(state);
    svga_fence_events_release(state);
    /* Contexts can retain views, shader code and surfaces. Release those
     * references before walking the rest of the namespace. */
    for (struct svga_object *object = state->objects, *next; object; object = next) {
        next = object->owner_next;
        if (object->type == SVGA_CONTEXT)
            unlink_object(object);
    }
    while (state->objects)
        unlink_object(state->objects);
    while (state->imports)
        unlink_import(import_link(state, state->imports->surface->object.handle));
    file->svga_render = NULL;
    kernel_free(state);
}

int svga_context_create(struct drm_file *file, bool dx, struct drm_vmw_context_arg *reply)
{
    if (dx && !svga_render_shader_model())
        return -EINVAL;
    struct svga_context *ctx = (struct svga_context *)svga_object_create(file, SVGA_CONTEXT, sizeof(*ctx));
    if (!ctx)
        return -ENOMEM;
    ctx->type = dx ? SVGA_CONTEXT_DX : svga_render_gb() ? SVGA_CONTEXT_GB : SVGA_CONTEXT_HOST;
    int ret = svga_context_publish(ctx);
    if (ret) {
        unlink_object(&ctx->object);
        return ret;
    }
    reply->cid = ctx->object.handle;
    return 0;
}

struct svga_shader *svga_shader_lookup(struct drm_file *file, uint32_t handle)
{
    return (struct svga_shader *)svga_object_lookup(file, SVGA_SHADER, handle);
}

static int shader_publish(struct svga_shader *shader)
{
    const uint32_t words[] = {
        SVGA_3D_CMD_DEFINE_GB_SHADER, sizeof(SVGA3dCmdDefineGBShader),
        shader->object.id, shader->type, shader->bytes,
        SVGA_3D_CMD_BIND_GB_SHADER, sizeof(SVGA3dCmdBindGBShader),
        shader->object.id, shader->backing ? svga_region_id(shader->backing) : SVGA3D_INVALID_ID,
        shader->offset,
    };
    uint32_t bytes = shader->backing ? sizeof(words) : sizeof(SVGA3dCmdHeader) + sizeof(SVGA3dCmdDefineGBShader);
    int ret = svga_submit(words, bytes);
    if (!ret)
        shader->object.published = true;
    return ret;
}

/* Legacy clients define their shaders inline and name them per context.
 * GB hosts need device-wide IDs and a MOB, as in vmw_compat_shader_add. */
int svga_shader_inline(uint32_t type, const void *code, uint32_t bytes, struct svga_shader **out)
{
    struct svga_shader *shader = kernel_malloc(sizeof(*shader));
    if (!shader)
        return -ENOMEM;
    uint32_t id = alloc_id(SVGA_SHADER);
    if (id == SVGA3D_INVALID_ID) {
        kernel_free(shader);
        return -ENOSPC;
    }
    *shader = (struct svga_shader){
        .object = {.type = SVGA_SHADER, .id = id, .references = 1},
        .type = type, .bytes = bytes,
    };
    int ret = svga_region_create(SVGA_REGION_MOB, bytes, &shader->backing);
    if (!ret) {
        __builtin_memcpy(svga_region_pixels(shader->backing), code, bytes);
        ret = shader_publish(shader);
    }
    if (ret)
        svga_object_put(&shader->object);
    else
        *out = shader;
    return ret;
}

int svga_shader_create(struct drm_file *file, struct drm_vmw_shader_create_arg *arg)
{
    if (!svga_render_gb())
        return -EOPNOTSUPP;
    if (!arg->size || (arg->size & 3) || arg->size > UINT32_MAX ||
        (arg->shader_type != drm_vmw_shader_type_vs && arg->shader_type != drm_vmw_shader_type_ps))
        return -EINVAL;

    /* A shader may get its bytecode later through BIND_GB_SHADER. */
    struct drm_bo *bo = NULL;
    if (arg->buffer_handle != SVGA3D_INVALID_ID) {
        bo = drm_bo_lookup_handle_get(file, arg->buffer_handle);
        if (!bo)
            return -ENOENT;
        uint64_t size = drm_bo_size(bo);
        if (arg->offset > UINT32_MAX || arg->offset > size || arg->size > size - arg->offset) {
            drm_bo_put(bo);
            return -EINVAL;
        }
    }
    struct svga_shader *shader = (struct svga_shader *)svga_object_create(file, SVGA_SHADER, sizeof(*shader));
    if (!shader) {
        drm_bo_put(bo);
        return -ENOMEM;
    }
    shader->type = arg->shader_type == drm_vmw_shader_type_vs ? SVGA3D_SHADERTYPE_VS : SVGA3D_SHADERTYPE_PS;
    shader->bytes = arg->size;
    int ret = 0;
    if (bo) {
        shader->offset = arg->offset;
        ret = svga_region_bind(SVGA_REGION_MOB, bo, &shader->backing);
        drm_bo_put(bo);
    }
    if (!ret)
        ret = shader_publish(shader);
    if (ret) {
        unlink_object(&shader->object);
        return ret;
    }
    arg->shader_handle = shader->object.handle;
    return 0;
}

void svga_render_rollback(struct drm_file *file, uint32_t command, const void *data, uint32_t object)
{
    if (object) {
        (void)svga_object_unref(file, SVGA_SURFACE, object);
        return;
    }
    switch (command) {
    case VMW_CREATE_CONTEXT:
        (void)svga_object_unref(file, SVGA_CONTEXT, ((const struct drm_vmw_context_arg *)data)->cid);
        break;
    case VMW_CREATE_EXTENDED_CONTEXT:
        (void)svga_object_unref(file, SVGA_CONTEXT, ((const union drm_vmw_extended_context_arg *)data)->rep.cid);
        break;
    case VMW_CREATE_SURFACE:
        (void)svga_object_unref(file, SVGA_SURFACE, ((const union drm_vmw_surface_create_arg *)data)->rep.sid);
        break;
    case VMW_GB_SURFACE_CREATE:
        (void)svga_object_unref(file, SVGA_SURFACE, ((const union drm_vmw_gb_surface_create_arg *)data)->rep.handle);
        break;
    case VMW_GB_SURFACE_CREATE_EXT:
        (void)svga_object_unref(file, SVGA_SURFACE, ((const union drm_vmw_gb_surface_create_ext_arg *)data)->rep.handle);
        break;
    case VMW_CREATE_SHADER:
        (void)svga_object_unref(file, SVGA_SHADER, ((const struct drm_vmw_shader_create_arg *)data)->shader_handle);
        break;
    }
}
