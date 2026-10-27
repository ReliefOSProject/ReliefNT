#include "memory.h"
#include "drm_internal.h"

#include <drm/drm_mode.h>
#include <linux/errno.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/paging.h>
#include <reliefnt/svga.h>
#include <reliefnt/usercopy.h>

#include "../svga/svga_protocol.h"

#define DRM_KMS_CRTC_ID 1u
#define DRM_KMS_CONNECTOR_ID 2u
#define DRM_KMS_ENCODER_ID 3u
#define DRM_KMS_FB_ID_MIN 0x01000000u
#define DRM_KMS_FB_ID_MAX 0x7fffffffu
#define DRM_FORMAT_XRGB8888 0x34325258u
#define DRM_FORMAT_ARGB8888 0x34325241u
#define DRM_KMS_PRESENT_JOBS_MAX 16u
#define DRM_VMW_RESOURCE_HANDLE_MIN 0x01000000u

struct drm_kms_fb {
    struct drm_kms_fb *next;
    struct drm_file *owner;
    struct drm_bo *bo;
    struct drm_vmwgfx_surface *surface;
    uint32_t references;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t offset;
    uint32_t format;
};

struct drm_kms_present_job {
    struct drm_kms_present_job *next;
    struct drm_kms_fb *fb;
    svga_handle fence;
    uint32_t x1, y1, x2, y2;
    bool submitted;
    bool cancelled;
};

struct drm_kms_pending_damage {
    struct drm_kms_fb *fb;
    uint32_t x1, y1, x2, y2;
    bool valid;
};

static uint32_t drm_kms_next_fb_id = DRM_KMS_FB_ID_MIN;
static struct drm_kms_fb *drm_kms_active_fb;
static struct drm_mode_modeinfo drm_kms_active_mode;
static bool drm_kms_scanout_enabled;
static bool drm_kms_framebuffer_owned;
static bool drm_kms_restore_pending;
static struct drm_kms_present_job *drm_kms_present_jobs;
static struct drm_kms_present_job *drm_kms_pending_job;
static uint32_t drm_kms_present_job_count;
static struct drm_kms_pending_damage drm_kms_pending_damage;

bool drm_device_kms_scanout_active(void)
{
    return __atomic_load_n(&drm_kms_scanout_enabled, __ATOMIC_ACQUIRE);
}

static bool drm_kms_framebuffer_ready(const struct framebuffer **out)
{
    const struct framebuffer *fb = framebuffer_get();
    if (!fb || !fb->available || fb->backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA ||
        fb->bpp != 32u || fb->bytes_per_pixel != 4u || !fb->width || !fb->height ||
        fb->width > UINT16_MAX || fb->height > UINT16_MAX ||
        fb->pitch < (uint64_t)fb->width * 4u)
        return false;
    if (out) *out = fb;
    return true;
}

static void drm_kms_append_decimal(char *buffer, uint32_t *length, uint32_t value)
{
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && count < sizeof(digits));
    while (count) buffer[(*length)++] = digits[--count];
}

static bool drm_kms_mode_current(struct drm_mode_modeinfo *mode)
{
    const struct framebuffer *fb;
    if (!mode || !drm_kms_framebuffer_ready(&fb)) return false;
    uint32_t hdisplay = fb->width;
    uint32_t vdisplay = fb->height;
    uint32_t hsync_start = hdisplay + 50u;
    uint32_t hsync_end = hsync_start + 50u;
    uint32_t htotal = hsync_end + 50u;
    uint32_t vsync_start = vdisplay + 3u;
    uint32_t vsync_end = vsync_start + 3u;
    uint32_t vtotal = vsync_end + 6u;
    uint64_t clock = (uint64_t)htotal * vtotal * 60u / 1000u;
    if (htotal > UINT16_MAX || vtotal > UINT16_MAX || clock > UINT32_MAX)
        return false;
    *mode = (struct drm_mode_modeinfo){
        .clock = (uint32_t)clock,
        .hdisplay = (uint16_t)hdisplay,
        .hsync_start = (uint16_t)hsync_start,
        .hsync_end = (uint16_t)hsync_end,
        .htotal = (uint16_t)htotal,
        .vdisplay = (uint16_t)vdisplay,
        .vsync_start = (uint16_t)vsync_start,
        .vsync_end = (uint16_t)vsync_end,
        .vtotal = (uint16_t)vtotal,
        .vrefresh = 60,
        .flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
        .type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
    };
    uint32_t length = 0;
    drm_kms_append_decimal(mode->name, &length, hdisplay);
    mode->name[length++] = 'x';
    drm_kms_append_decimal(mode->name, &length, vdisplay);
    mode->name[length] = '\0';
    return true;
}

static bool drm_kms_mode_equal(const struct drm_mode_modeinfo *left,
                               const struct drm_mode_modeinfo *right)
{
    return left->clock == right->clock &&
        left->hdisplay == right->hdisplay &&
        left->hsync_start == right->hsync_start &&
        left->hsync_end == right->hsync_end && left->htotal == right->htotal &&
        left->hskew == right->hskew && left->vdisplay == right->vdisplay &&
        left->vsync_start == right->vsync_start &&
        left->vsync_end == right->vsync_end && left->vtotal == right->vtotal &&
        left->vscan == right->vscan && left->flags == right->flags;
}

static struct drm_kms_fb *drm_kms_find_fb(struct drm_file *file, uint32_t id)
{
    for (struct drm_kms_fb *fb = file ? file->kms_fbs : NULL; fb; fb = fb->next)
        if (fb->id == id) return fb;
    return NULL;
}

static void drm_kms_fb_get(struct drm_kms_fb *fb)
{
    if (fb) ++fb->references;
}

/**
 * @brief Release a framebuffer only after all file, scanout and job references drain.
 * @param fb Owned framebuffer reference, or NULL.
 * Execution-owned task context under independent caller pin; final surface/BO/heap/module release occurs outside graphics locks.
 */
static void drm_kms_fb_put(struct drm_kms_fb *fb)
{
    if (!fb || !fb->references || --fb->references) return;
    if (fb->surface) drm_vmwgfx_surface_put(fb->surface);
    if (fb->bo) drm_bo_put(fb->bo);
    drm_async_free(fb);
}

static int drm_kms_map_svga_status(int status)
{
    switch (status) {
    case 0: return 0;
    case SVGA_EBUSY: return -LINUX_EBUSY;
    case SVGA_ENOMEM: return -LINUX_ENOMEM;
    case SVGA_ENODEV: return -LINUX_ENODEV;
    case SVGA_ENOTSUP: return -LINUX_EOPNOTSUPP;
    case SVGA_ETIMEDOUT: return -LINUX_ETIMEDOUT;
    case SVGA_EINVAL: return -LINUX_EINVAL;
    default: return -LINUX_EIO;
    }
}

static void drm_kms_job_set_rect(struct drm_kms_present_job *job,
                                 uint32_t x1, uint32_t y1,
                                 uint32_t x2, uint32_t y2)
{
    if (!job) return;
    job->x1 = x1;
    job->y1 = y1;
    job->x2 = x2;
    job->y2 = y2;
}

static void drm_kms_job_enqueue(struct drm_kms_present_job *job)
{
    job->next = drm_kms_present_jobs;
    drm_kms_present_jobs = job;
    ++drm_kms_present_job_count;
}

static void drm_kms_job_remove(struct drm_kms_present_job *job)
{
    struct drm_kms_present_job **link = &drm_kms_present_jobs;
    while (*link && *link != job) link = &(*link)->next;
    if (*link == job) {
        *link = job->next;
        job->next = NULL;
        if (drm_kms_present_job_count) --drm_kms_present_job_count;
    }
    if (drm_kms_pending_job == job) drm_kms_pending_job = NULL;
}

/**
 * @brief Release a hardware-complete or safely cancelled presentation job.
 * @param job Owned job or NULL; backend must no longer retain it.
 * Execution-owned task context under independent caller pin; drops retained framebuffer before the job module reference, outside queue locks.
 */
static void drm_kms_job_free(struct drm_kms_present_job *job)
{
    if (!job) return;
    drm_kms_job_remove(job);
    if (job->fb) drm_kms_fb_put(job->fb);
    drm_async_free(job);
}

/** @brief Submit the caller-pinned surface without render-batch allocations. */
static int drm_kms_job_submit(struct drm_kms_present_job *job)
{
    if (!job || job->submitted || job->cancelled)
        return -LINUX_EINVAL;
    const struct svga_present_rect rect = {
        .x = job->x1, .y = job->y1,
        .w = job->x2 - job->x1, .h = job->y2 - job->y1,
        .srcx = job->x1, .srcy = job->y1,
    };
    svga_handle surface = drm_vmwgfx_surface_backend_handle(job->fb->surface);
    int ret = drm_kms_map_svga_status(
        svga3d_present_try(surface, &rect, 1, &job->fence));
    if (!ret) job->submitted = true;
    return ret;
}

static void drm_kms_pending_damage_clear(void)
{
    if (drm_kms_pending_damage.valid && drm_kms_pending_damage.fb)
        drm_kms_fb_put(drm_kms_pending_damage.fb);
    drm_kms_pending_damage = (struct drm_kms_pending_damage){0};
}

static void drm_kms_cancel_pending(void)
{
    drm_kms_pending_damage_clear();
    if (drm_kms_pending_job) {
        drm_kms_pending_job->cancelled = true;
        drm_kms_pending_job = NULL;
    }
}

static void drm_kms_maybe_restore_framebuffer(void)
{
    if (!drm_kms_restore_pending || drm_kms_active_fb ||
        drm_kms_present_job_count || drm_kms_pending_damage.valid ||
        drm_kms_pending_job)
        return;
    if (drm_kms_framebuffer_owned) framebuffer_drm_scanout_release();
    drm_kms_framebuffer_owned = false;
    drm_kms_restore_pending = false;
}

static void drm_kms_job_discard(struct drm_kms_present_job *job)
{
    if (!job) return;
    drm_kms_job_free(job);
}

static void drm_kms_service_jobs(uint32_t budget)
{
    struct drm_kms_present_job *job = drm_kms_present_jobs;
    uint32_t visited = 0;
    while (job && visited < budget) {
        struct drm_kms_present_job *next = job->next;
        ++visited;
        if (job->cancelled && !job->submitted) {
            drm_kms_job_discard(job);
        } else if (job == drm_kms_pending_job && !job->submitted) {
            int ret = drm_kms_job_submit(job);
            if (!ret) {
                drm_kms_pending_job = NULL;
            } else if (ret != -LINUX_EBUSY) {
                if (!drm_kms_pending_damage.valid) {
                    drm_kms_fb_get(job->fb);
                    drm_kms_pending_damage = (struct drm_kms_pending_damage){
                        .fb = job->fb, .x1 = job->x1, .y1 = job->y1,
                        .x2 = job->x2, .y2 = job->y2, .valid = true,
                    };
                }
                job->cancelled = true;
                drm_kms_pending_job = NULL;
            }
        } else if (job->submitted) {
            bool signaled = false;
            int status = svga_fence_status(job->fence, &signaled);
            bool quiesced = status == SVGA_EIO &&
                            svga_fence_is_quiesced(job->fence);
            if ((!status && signaled) || quiesced)
                drm_kms_job_free(job);
        }
        job = next;
    }
}

/**
 * @brief Promote retained damage into independently pinned asynchronous presentation.
 * Execution-owned task context under file/service pin. Pending framebuffer ownership survives pressure; no graphics lock spans allocation or backend submission.
 */
static void drm_kms_service_pending_damage(void)
{
    if (drm_kms_pending_job) {
        if (!drm_kms_pending_job->cancelled) {
            int ret = drm_kms_job_submit(drm_kms_pending_job);
            if (!ret) {
                drm_kms_pending_job = NULL;
            } else if (ret != -LINUX_EBUSY) {
                if (!drm_kms_pending_damage.valid) {
                    drm_kms_fb_get(drm_kms_pending_job->fb);
                    drm_kms_pending_damage = (struct drm_kms_pending_damage){
                        .fb = drm_kms_pending_job->fb,
                        .x1 = drm_kms_pending_job->x1,
                        .y1 = drm_kms_pending_job->y1,
                        .x2 = drm_kms_pending_job->x2,
                        .y2 = drm_kms_pending_job->y2,
                        .valid = true,
                    };
                }
                drm_kms_pending_job->cancelled = true;
                drm_kms_pending_job = NULL;
            }
        }
        return;
    }
    if (!drm_kms_pending_damage.valid ||
        drm_kms_present_job_count >= DRM_KMS_PRESENT_JOBS_MAX)
        return;
    if (!drm_kms_active_fb || drm_kms_active_fb != drm_kms_pending_damage.fb) {
        drm_kms_pending_damage_clear();
        return;
    }
    struct drm_kms_present_job *job = drm_async_alloc(sizeof(*job));
    if (!job) return;
    __builtin_memset(job, 0, sizeof(*job));
    job->fb = drm_kms_pending_damage.fb;
    drm_kms_fb_get(job->fb);
    drm_kms_job_set_rect(job, drm_kms_pending_damage.x1,
                         drm_kms_pending_damage.y1,
                         drm_kms_pending_damage.x2,
                         drm_kms_pending_damage.y2);
    drm_kms_job_enqueue(job);
    int ret = drm_kms_job_submit(job);
    if (!ret) {
        drm_kms_pending_damage_clear();
    } else if (ret == -LINUX_EBUSY) {
        drm_kms_pending_job = job;
        drm_kms_pending_damage_clear();
    } else {
        job->cancelled = true;
    }
}

/**
 * @brief Construct independently pinned state for the requested surface presentation.
 * @param fb Borrowed retained framebuffer.
 * @param x1 Validated left edge.
 * @param y1 Validated top edge.
 * @param x2 Validated exclusive right edge.
 * @param y2 Validated exclusive bottom edge.
 * @return Original submission result or Linux errno; queued jobs retain backing and code until retirement.
 * Execution-owned task context under file/service pin, with no queue lock spanning heap or backend calls.
 */
static int drm_kms_submit_surface_present(struct drm_kms_fb *fb,
                                          uint32_t x1, uint32_t y1,
                                          uint32_t x2, uint32_t y2)
{
    if (!fb || !fb->surface || x1 >= x2 || y1 >= y2 ||
        x2 > fb->width || y2 > fb->height)
        return -LINUX_EINVAL;
    if (drm_kms_present_job_count >= DRM_KMS_PRESENT_JOBS_MAX)
        return -LINUX_EBUSY;
    struct drm_kms_present_job *job = drm_async_alloc(sizeof(*job));
    if (!job) return -LINUX_ENOMEM;
    __builtin_memset(job, 0, sizeof(*job));
    job->fb = fb;
    drm_kms_fb_get(fb);
    drm_kms_job_set_rect(job, x1, y1, x2, y2);
    drm_kms_job_enqueue(job);
    int ret = drm_kms_job_submit(job);
    if (ret) {
        drm_kms_job_discard(job);
        return ret;
    }
    return 0;
}

void drm_kms_process_retirements(uint32_t budget)
{
    if (!budget) budget = 1;
    if (budget > DRM_KMS_PRESENT_JOBS_MAX) budget = DRM_KMS_PRESENT_JOBS_MAX;
    drm_kms_service_jobs(budget);
    drm_kms_service_pending_damage();
    drm_kms_maybe_restore_framebuffer();
}

static int drm_kms_new_fb_id(uint32_t *id_out)
{
    if (drm_kms_next_fb_id < DRM_KMS_FB_ID_MIN ||
        drm_kms_next_fb_id > DRM_KMS_FB_ID_MAX) return -LINUX_ENOSPC;
    *id_out = drm_kms_next_fb_id++;
    return 0;
}

/**
 * @brief Check supported KMS-to-SVGA 32-bit channel-compatible scanout pairs.
 * @param drm_format KMS fourcc requested for the framebuffer.
 * @param surface_format SVGA3D backing format stored by the typed surface.
 * @return True for byte-compatible supported pairs; false for other layouts.
 * @note XRGB8888 ignores its fourth byte and can scan out A8R8G8B8 backing;
 *       ARGB8888 requires the backing's alpha channel to be present.
 */
static bool drm_kms_surface_format_compatible(uint32_t drm_format,
                                               uint32_t surface_format)
{
    if (drm_format == DRM_FORMAT_XRGB8888)
        return surface_format == SVGA3D_X8R8G8B8 ||
               surface_format == SVGA3D_A8R8G8B8;
    if (drm_format == DRM_FORMAT_ARGB8888)
        return surface_format == SVGA3D_A8R8G8B8;
    return false;
}

/**
 * @brief Validate and publish a framebuffer with original provider ownership.
 * @param file Retained owning DRM file.
 * @param width Requested pixel width.
 * @param height Requested pixel height.
 * @param pitch Requested byte stride.
 * @param format Linux DRM format.
 * @param handle Validated file-owned BO/surface selector.
 * @param offset Requested backing byte offset.
 * @param id_out Required created framebuffer ID output.
 * @return Zero or Linux validation/allocation error with balanced backing and module references.
 * Execution-owned task context under file pin; no graphics lock spans allocation or backing destruction.
 */
static int drm_kms_add_fb(struct drm_file *file, uint32_t width, uint32_t height,
                          uint32_t pitch, uint32_t format, uint32_t handle,
                          uint64_t offset, uint32_t *id_out)
{
    const struct framebuffer *display;
    struct drm_bo *bo = NULL;
    struct drm_vmwgfx_surface *surface = NULL;
    struct drm_vmwgfx_surface_metadata metadata;
    uint64_t row_bytes, last_row, required, size;
    if (!file || !id_out || !width || !height || !handle ||
        (format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_ARGB8888) ||
        !drm_kms_framebuffer_ready(&display) ||
        width > display->max_width || height > display->max_height ||
        width > UINT32_MAX / 4u)
        return -LINUX_EINVAL;
    row_bytes = (uint64_t)width * 4u;
    if (pitch < row_bytes || offset > UINT64_MAX - row_bytes) return -LINUX_EINVAL;
    last_row = (uint64_t)(height - 1u) * pitch;
    if (height > 1u && last_row / (height - 1u) != pitch) return -LINUX_EINVAL;
    if (last_row > UINT64_MAX - row_bytes) return -LINUX_EINVAL;
    required = last_row + row_bytes;
    if (offset > UINT64_MAX - required) return -LINUX_EINVAL;
    required += offset;
    if (handle >= DRM_VMW_RESOURCE_HANDLE_MIN) {
        if (offset) return -LINUX_EINVAL;
        surface = drm_vmwgfx_surface_lookup_get(file, handle);
        if (!surface) return -LINUX_EINVAL;
        int ret = drm_vmwgfx_surface_get_metadata(surface, &metadata);
        if (ret || !metadata.shareable || !metadata.scanout ||
            !drm_kms_surface_format_compatible(format, metadata.desc.format) ||
            (metadata.desc.flags & SVGA3D_SURFACE_CUBEMAP) ||
            metadata.mip_levels[0] != 1u || metadata.mip_levels[1] ||
            metadata.mip_levels[2] || metadata.mip_levels[3] ||
            metadata.mip_levels[4] || metadata.mip_levels[5] ||
            metadata.desc.depth != 1u || metadata.desc.width < width ||
            metadata.desc.height < height) {
            drm_vmwgfx_surface_put(surface);
            return -LINUX_EINVAL;
        }
    } else {
        if (format != DRM_FORMAT_XRGB8888) return -LINUX_EINVAL;
        bo = drm_bo_lookup_handle_get(file, handle);
        if (!bo) return -LINUX_EINVAL;
        size = drm_bo_size(bo);
        if (!size || required > size) {
            drm_bo_put(bo);
            return -LINUX_EINVAL;
        }
    }
    struct drm_kms_fb *fb = drm_async_alloc(sizeof(*fb));
    if (!fb) {
        if (surface) drm_vmwgfx_surface_put(surface);
        if (bo) drm_bo_put(bo);
        return -LINUX_ENOMEM;
    }
    uint32_t id;
    int ret = drm_kms_new_fb_id(&id);
    if (ret < 0) {
        drm_async_free(fb);
        if (surface) drm_vmwgfx_surface_put(surface);
        if (bo) drm_bo_put(bo);
        return ret;
    }
    *fb = (struct drm_kms_fb){
        .next = file->kms_fbs,
        .owner = file,
        .bo = bo,
        .surface = surface,
        .references = 1,
        .id = id,
        .width = width,
        .height = height,
        .pitch = pitch,
        .offset = offset,
        .format = format,
    };
    file->kms_fbs = fb;
    *id_out = id;
    return 0;
}

static void drm_kms_drop_active(void)
{
    struct drm_kms_fb *fb = drm_kms_active_fb;
    __atomic_store_n(&drm_kms_scanout_enabled, false, __ATOMIC_RELEASE);
    drm_kms_active_fb = NULL;
    __builtin_memset(&drm_kms_active_mode, 0, sizeof(drm_kms_active_mode));
    drm_kms_cancel_pending();
    drm_kms_restore_pending = true;
    if (fb) {
        if (fb->bo) drm_bo_put(fb->bo); /* independent CRTC scanout reference */
        drm_kms_fb_put(fb);
    }
    drm_kms_maybe_restore_framebuffer();
}

static int drm_kms_get_resources(struct task *task, struct drm_file *file,
                                 struct drm_mode_card_res *resources)
{
    const struct framebuffer *display;
    if (!drm_kms_framebuffer_ready(&display)) return -LINUX_ENODEV;
    uint32_t capacity_fbs = resources->count_fbs;
    uint32_t capacity_crtcs = resources->count_crtcs;
    uint32_t capacity_connectors = resources->count_connectors;
    uint32_t capacity_encoders = resources->count_encoders;
    uint64_t crtc_ptr = resources->crtc_id_ptr;
    uint64_t fb_ptr = resources->fb_id_ptr;
    uint64_t connector_ptr = resources->connector_id_ptr;
    uint64_t encoder_ptr = resources->encoder_id_ptr;
    uint32_t count_fbs = 0;
    for (struct drm_kms_fb *fb = file->kms_fbs; fb; fb = fb->next) ++count_fbs;
    resources->count_fbs = count_fbs;
    resources->count_crtcs = 1;
    resources->count_connectors = 1;
    resources->count_encoders = 1;
    resources->min_width = 1;
    resources->max_width = display->width;
    resources->min_height = 1;
    resources->max_height = display->height;
    uint32_t copied = 0;
    for (struct drm_kms_fb *fb = file->kms_fbs; fb && copied < capacity_fbs;
         fb = fb->next, ++copied) {
        if (user_copy_to_task(task, fb_ptr + (uint64_t)copied * sizeof(fb->id),
                              &fb->id, sizeof(fb->id)) < 0)
            return -LINUX_EFAULT;
    }
    uint32_t id;
    uint32_t copy_count = capacity_crtcs < 1u ? capacity_crtcs : 1u;
    if (copy_count) {
        id = DRM_KMS_CRTC_ID;
        if (user_copy_to_task(task, crtc_ptr, &id, sizeof(id)) < 0)
            return -LINUX_EFAULT;
    }
    copy_count = capacity_connectors < 1u ? capacity_connectors : 1u;
    if (copy_count) {
        id = DRM_KMS_CONNECTOR_ID;
        if (user_copy_to_task(task, connector_ptr, &id, sizeof(id)) < 0)
            return -LINUX_EFAULT;
    }
    copy_count = capacity_encoders < 1u ? capacity_encoders : 1u;
    if (copy_count) {
        id = DRM_KMS_ENCODER_ID;
        if (user_copy_to_task(task, encoder_ptr, &id, sizeof(id)) < 0)
            return -LINUX_EFAULT;
    }
    return 0;
}

static int drm_kms_get_connector(struct task *task,
                                 struct drm_mode_get_connector *connector)
{
    struct drm_mode_modeinfo mode;
    if (connector->connector_id != DRM_KMS_CONNECTOR_ID)
        return -LINUX_ENOENT;
    if (!drm_kms_mode_current(&mode)) return -LINUX_ENODEV;
    uint32_t mode_capacity = connector->count_modes;
    uint32_t encoder_capacity = connector->count_encoders;
    uint64_t mode_ptr = connector->modes_ptr;
    uint64_t encoder_ptr = connector->encoders_ptr;
    uint64_t props_ptr = connector->props_ptr;
    uint64_t prop_values_ptr = connector->prop_values_ptr;
    *connector = (struct drm_mode_get_connector){
        .encoders_ptr = encoder_ptr,
        .modes_ptr = mode_ptr,
        .props_ptr = props_ptr,
        .prop_values_ptr = prop_values_ptr,
        .count_modes = 1,
        .count_encoders = 1,
        .encoder_id = DRM_KMS_ENCODER_ID,
        .connector_id = DRM_KMS_CONNECTOR_ID,
        .connector_type = DRM_MODE_CONNECTOR_VIRTUAL,
        .connector_type_id = 1,
        .connection = 1,
        .subpixel = 0,
    };
    if (mode_capacity) {
        if (user_copy_to_task(task, mode_ptr, &mode, sizeof(mode)) < 0)
            return -LINUX_EFAULT;
    }
    if (encoder_capacity) {
        uint32_t id = DRM_KMS_ENCODER_ID;
        if (user_copy_to_task(task, encoder_ptr, &id, sizeof(id)) < 0)
            return -LINUX_EFAULT;
    }
    return 0;
}

static int drm_kms_get_encoder(struct drm_mode_get_encoder *encoder)
{
    if (encoder->encoder_id != DRM_KMS_ENCODER_ID) return -LINUX_ENOENT;
    *encoder = (struct drm_mode_get_encoder){
        .encoder_id = DRM_KMS_ENCODER_ID,
        .encoder_type = DRM_MODE_ENCODER_VIRTUAL,
        .crtc_id = drm_kms_active_fb ? DRM_KMS_CRTC_ID : 0,
        .possible_crtcs = 1,
        .possible_clones = 0,
    };
    return 0;
}

static int drm_kms_get_crtc(struct drm_mode_crtc *crtc)
{
    if (crtc->crtc_id != DRM_KMS_CRTC_ID) return -LINUX_ENOENT;
    uint32_t id = crtc->crtc_id;
    *crtc = (struct drm_mode_crtc){.crtc_id = id, .gamma_size = 0};
    if (drm_kms_active_fb) {
        crtc->fb_id = drm_kms_active_fb->id;
        crtc->mode_valid = 1;
        crtc->mode = drm_kms_active_mode;
    }
    return 0;
}

static int drm_kms_set_crtc(struct task *task, struct drm_file *file,
                            struct drm_mode_crtc *crtc)
{
    if (crtc->crtc_id != DRM_KMS_CRTC_ID) return -LINUX_ENOENT;
    if (crtc->mode_valid > 1u) return -LINUX_EINVAL;
    if (!crtc->mode_valid) {
        if (crtc->fb_id || crtc->count_connectors) return -LINUX_EINVAL;
        drm_kms_drop_active();
        return 0;
    }
    struct drm_mode_modeinfo mode;
    if (!drm_kms_mode_current(&mode)) return -LINUX_ENODEV;
    if (!crtc->fb_id || crtc->x || crtc->y || crtc->count_connectors != 1u)
        return -LINUX_EINVAL;
    uint32_t connector_id;
    if (user_copy_from_task(task, &connector_id, crtc->set_connectors_ptr,
                            sizeof(connector_id)) < 0)
        return -LINUX_EFAULT;
    if (connector_id != DRM_KMS_CONNECTOR_ID ||
        !drm_kms_mode_equal(&mode, &crtc->mode)) return -LINUX_EINVAL;
    /* Framebuffers are per-file, while the CRTC is device-global. A caller
     * may only scan out an FB from its own DRM open-file description. */
    struct drm_kms_fb *fb = drm_kms_find_fb(file, crtc->fb_id);
    const struct framebuffer *display;
    if (!fb) return -LINUX_EINVAL;
    if (!drm_kms_framebuffer_ready(&display) || fb->width != display->width ||
        fb->height != display->height ||
        (fb->surface
             ? (fb->format != DRM_FORMAT_XRGB8888 &&
                fb->format != DRM_FORMAT_ARGB8888)
             : fb->format != DRM_FORMAT_XRGB8888))
        return -LINUX_EINVAL;
    bool began_scanout = false;
    if (!drm_kms_framebuffer_owned) {
        int begin_ret = framebuffer_drm_scanout_begin();
        if (begin_ret < 0) return -LINUX_ENODEV;
        drm_kms_framebuffer_owned = true;
        began_scanout = true;
    }
    int ret;
    if (fb->surface) {
        ret = drm_kms_submit_surface_present(fb, 0, 0,
                                             fb->width, fb->height);
    } else {
        uint64_t physical = drm_bo_page_physical(fb->bo, 0);
        uint8_t *pixels = physical ? paging_kernel_direct_map(physical) : NULL;
        if (!pixels) ret = -LINUX_EFAULT;
        else ret = framebuffer_drm_present_region(
            pixels + fb->offset, fb->pitch, fb->format,
            0, 0, fb->width, fb->height);
    }
    if (ret < 0) {
        if (began_scanout) {
            framebuffer_drm_scanout_release();
            drm_kms_framebuffer_owned = false;
        }
        return ret;
    }
    drm_kms_cancel_pending();
    drm_kms_restore_pending = false;
    if (drm_kms_active_fb != fb) {
        drm_kms_fb_get(fb);
        if (fb->bo) drm_bo_get(fb->bo); /* independent CRTC scanout reference */
        struct drm_kms_fb *old = drm_kms_active_fb;
        drm_kms_active_fb = fb;
        __atomic_store_n(&drm_kms_scanout_enabled, true, __ATOMIC_RELEASE);
        if (old) {
            if (old->bo) drm_bo_put(old->bo);
            drm_kms_fb_put(old);
        }
    }
    drm_kms_active_mode = mode;
    return 0;
}

static int drm_kms_addfb2(struct drm_file *file, struct drm_mode_fb_cmd2 *arg,
                          uint32_t *created_fb_out)
{
    if (arg->fb_id || arg->flags ||
        (arg->pixel_format != DRM_FORMAT_XRGB8888 &&
         arg->pixel_format != DRM_FORMAT_ARGB8888) ||
        !arg->handles[0] || !arg->pitches[0] || arg->modifier[0] ||
        arg->handles[1] || arg->handles[2] || arg->handles[3] ||
        arg->pitches[1] || arg->pitches[2] || arg->pitches[3] ||
        arg->offsets[1] || arg->offsets[2] || arg->offsets[3] ||
        arg->modifier[1] || arg->modifier[2] || arg->modifier[3])
        return -LINUX_EINVAL;
    uint32_t id;
    int ret = drm_kms_add_fb(file, arg->width, arg->height, arg->pitches[0],
                             arg->pixel_format, arg->handles[0], arg->offsets[0],
                             &id);
    if (ret < 0) return ret;
    arg->fb_id = id;
    if (created_fb_out) *created_fb_out = id;
    return 0;
}

static int drm_kms_addfb(struct drm_file *file, struct drm_mode_fb_cmd *arg,
                         uint32_t *created_fb_out)
{
    if (arg->fb_id || arg->depth != 24u || arg->bpp != 32u)
        return -LINUX_EINVAL;
    uint32_t id;
    int ret = drm_kms_add_fb(file, arg->width, arg->height, arg->pitch,
                             DRM_FORMAT_XRGB8888, arg->handle, 0, &id);
    if (ret < 0) return ret;
    arg->fb_id = id;
    if (created_fb_out) *created_fb_out = id;
    return 0;
}

static int drm_kms_rmfb(struct drm_file *file, uint32_t id)
{
    struct drm_kms_fb **link = &file->kms_fbs;
    while (*link && (*link)->id != id) link = &(*link)->next;
    if (!*link) return -LINUX_ENOENT;
    struct drm_kms_fb *fb = *link;
    *link = fb->next;
    fb->next = NULL;
    if ((drm_kms_pending_damage.valid &&
         drm_kms_pending_damage.fb == fb) ||
        (drm_kms_pending_job && drm_kms_pending_job->fb == fb))
        drm_kms_cancel_pending();
    if (drm_kms_active_fb == fb) drm_kms_drop_active();
    drm_kms_fb_put(fb);
    return 0;
}

static int drm_kms_dirtyfb(struct task *task, struct drm_file *file,
                           struct drm_mode_fb_dirty_cmd *arg)
{
    struct drm_kms_fb *fb = drm_kms_find_fb(file, arg->fb_id);
    if (!fb) return -LINUX_ENOENT;
    if (arg->flags || arg->num_clips > DRM_MODE_FB_DIRTY_MAX_CLIPS)
        return -LINUX_EINVAL;
    const struct framebuffer *display;
    if (!drm_kms_framebuffer_ready(&display)) return -LINUX_ENODEV;
    if (fb->width != display->width || fb->height != display->height)
        return -LINUX_EINVAL;
    struct drm_clip_rect clips[DRM_MODE_FB_DIRTY_MAX_CLIPS];
    if (arg->num_clips && user_copy_from_task(task, clips, arg->clips_ptr,
            (uint64_t)arg->num_clips * sizeof(clips[0])) < 0)
        return -LINUX_EFAULT;
    uint32_t x1 = fb->width, y1 = fb->height, x2 = 0, y2 = 0;
    if (!arg->num_clips) {
        x1 = 0;
        y1 = 0;
        x2 = fb->width;
        y2 = fb->height;
    } else {
        for (uint32_t i = 0; i < arg->num_clips; ++i) {
            if (clips[i].x1 >= clips[i].x2 || clips[i].y1 >= clips[i].y2 ||
                clips[i].x2 > fb->width || clips[i].y2 > fb->height)
                return -LINUX_EINVAL;
            if (clips[i].x1 < x1) x1 = clips[i].x1;
            if (clips[i].y1 < y1) y1 = clips[i].y1;
            if (clips[i].x2 > x2) x2 = clips[i].x2;
            if (clips[i].y2 > y2) y2 = clips[i].y2;
        }
    }
    if (drm_kms_active_fb != fb) return 0;
    if (fb->surface) {
        if (drm_kms_pending_job &&
            !drm_kms_pending_job->submitted &&
            drm_kms_pending_job->fb == fb) {
            struct drm_kms_present_job *job = drm_kms_pending_job;
            if (x1 > job->x1) x1 = job->x1;
            if (y1 > job->y1) y1 = job->y1;
            if (x2 < job->x2) x2 = job->x2;
            if (y2 < job->y2) y2 = job->y2;
            drm_kms_job_set_rect(job, x1, y1, x2, y2);
            drm_kms_service_pending_damage();
            return 0;
        }
        if (drm_kms_pending_damage.valid &&
            drm_kms_pending_damage.fb == fb) {
            if (x1 > drm_kms_pending_damage.x1) x1 = drm_kms_pending_damage.x1;
            if (y1 > drm_kms_pending_damage.y1) y1 = drm_kms_pending_damage.y1;
            if (x2 < drm_kms_pending_damage.x2) x2 = drm_kms_pending_damage.x2;
            if (y2 < drm_kms_pending_damage.y2) y2 = drm_kms_pending_damage.y2;
            drm_kms_pending_damage.x1 = x1;
            drm_kms_pending_damage.y1 = y1;
            drm_kms_pending_damage.x2 = x2;
            drm_kms_pending_damage.y2 = y2;
        } else {
            drm_kms_pending_damage_clear();
            drm_kms_fb_get(fb);
            drm_kms_pending_damage = (struct drm_kms_pending_damage){
                .fb = fb, .x1 = x1, .y1 = y1,
                .x2 = x2, .y2 = y2, .valid = true,
            };
        }
        drm_kms_service_pending_damage();
        return 0;
    }
    uint64_t physical = drm_bo_page_physical(fb->bo, 0);
    uint8_t *pixels = physical ? paging_kernel_direct_map(physical) : NULL;
    if (!pixels) return -LINUX_EFAULT;
    pixels += fb->offset;
    return framebuffer_drm_present_region(pixels, fb->pitch, fb->format,
                                          x1, y1, x2, y2);
}

static int drm_kms_get_object_properties(struct drm_mode_obj_get_properties *arg)
{
    bool valid = (arg->obj_type == DRM_MODE_OBJECT_CONNECTOR &&
                  arg->obj_id == DRM_KMS_CONNECTOR_ID) ||
                 (arg->obj_type == DRM_MODE_OBJECT_CRTC &&
                  arg->obj_id == DRM_KMS_CRTC_ID) ||
                 (arg->obj_type == DRM_MODE_OBJECT_ENCODER &&
                  arg->obj_id == DRM_KMS_ENCODER_ID);
    if (!valid) return -LINUX_ENOENT;
    arg->count_props = 0;
    return 0;
}

int drm_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                  void *data, uint32_t *created_fb_out)
{
    if (created_fb_out) *created_fb_out = 0;
    if (!file || !data) return -LINUX_EFAULT;
    switch (command) {
    case DRM_IOCTL_MODE_GETRESOURCES:
        return drm_kms_get_resources(task, file, data);
    case DRM_IOCTL_MODE_GETCONNECTOR:
        return drm_kms_get_connector(task, data);
    case DRM_IOCTL_MODE_GETENCODER:
        return drm_kms_get_encoder(data);
    case DRM_IOCTL_MODE_GETCRTC:
        return drm_kms_get_crtc(data);
    case DRM_IOCTL_MODE_SETCRTC:
        return drm_kms_set_crtc(task, file, data);
    case DRM_IOCTL_MODE_ADDFB:
        return drm_kms_addfb(file, data, created_fb_out);
    case DRM_IOCTL_MODE_ADDFB2:
        return drm_kms_addfb2(file, data, created_fb_out);
    case DRM_IOCTL_MODE_RMFB:
        return drm_kms_rmfb(file, *(uint32_t *)data);
    case DRM_IOCTL_MODE_DIRTYFB:
        return drm_kms_dirtyfb(task, file, data);
    case DRM_IOCTL_MODE_OBJ_GETPROPERTIES:
        return drm_kms_get_object_properties(data);
    case DRM_IOCTL_MODE_GETPROPERTY:
    case DRM_IOCTL_MODE_SETPROPERTY:
    case DRM_IOCTL_MODE_OBJ_SETPROPERTY:
    case DRM_IOCTL_MODE_GETPROPBLOB:
        return -LINUX_ENOENT;
    default:
        return -LINUX_EOPNOTSUPP;
    }
}

void drm_kms_remove_fb(struct drm_file *file, uint32_t fb_id)
{
    if (file && fb_id) (void)drm_kms_rmfb(file, fb_id);
}

void drm_kms_drop_master(struct drm_file *file)
{
    if (drm_kms_active_fb && drm_kms_active_fb->owner == file)
        drm_kms_drop_active();
    else if ((drm_kms_pending_damage.valid &&
              drm_kms_pending_damage.fb->owner == file) ||
             (drm_kms_pending_job && drm_kms_pending_job->fb->owner == file))
        drm_kms_cancel_pending();
}

void drm_kms_vt_release(void)
{
    if (drm_kms_active_fb) drm_kms_drop_active();
    else {
        drm_kms_cancel_pending();
        drm_kms_restore_pending = true;
        drm_kms_maybe_restore_framebuffer();
    }
}

void drm_device_kms_vt_release(void)
{
    drm_kms_vt_release();
}

void drm_kms_file_release(struct drm_file *file)
{
    if (!file) return;
    drm_kms_drop_master(file);
    while (file->kms_fbs) {
        struct drm_kms_fb *fb = file->kms_fbs;
        file->kms_fbs = fb->next;
        fb->next = NULL;
        drm_kms_fb_put(fb);
    }
}
