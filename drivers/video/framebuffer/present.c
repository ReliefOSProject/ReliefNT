#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include "internal.h"

static struct framebuffer_damage svga_damage;
static struct framebuffer_damage framebuffer_damage;
static struct kernel_spinlock framebuffer_damage_lock = KERNEL_SPINLOCK_INIT;
static uint64_t framebuffer_damage_generation;

/** @brief Read the permanent CPU framebuffer's explicit publication generation.
 * @return Atomic damage snapshot; no lock, allocation or optional code.
 */
uint64_t framebuffer_generation(void)
{ return __atomic_load_n(&framebuffer_damage_generation,__ATOMIC_ACQUIRE); }

/** @brief Transfer accumulated pixel bounds to the normal scanout consumer. */
bool framebuffer_take_damage(uint64_t *generation, struct framebuffer_damage *damage)
{
    if (*generation == framebuffer_generation()) return false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&framebuffer_damage_lock, &flags);
    bool pending = framebuffer_damage.x1 < framebuffer_damage.x2;
    if (pending) {
        *damage = framebuffer_damage;
        *generation = __atomic_load_n(&framebuffer_damage_generation, __ATOMIC_RELAXED);
        framebuffer_damage = (struct framebuffer_damage){0};
    }
    kernel_spin_unlock_irqrestore(&framebuffer_damage_lock, flags);
    return pending;
}

/** @brief Record a clipped CPU update without holding a hardware lock. */
static void framebuffer_record_damage(struct framebuffer_damage damage)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&framebuffer_damage_lock, &flags);
    framebuffer_damage_union(&framebuffer_damage, damage);
    uint64_t generation = __atomic_load_n(&framebuffer_damage_generation, __ATOMIC_RELAXED);
    __atomic_store_n(&framebuffer_damage_generation, generation + 1, __ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&framebuffer_damage_lock, flags);
}

bool framebuffer_drm_owned(void)
{
    return __atomic_load_n(&framebuffer_drm_scanout_owned, __ATOMIC_ACQUIRE);
}

/** @brief Publish a fenced pixel region or retain it in the boot scanout.
 * The SVGA lock protects the rectangle; the pending flag lets idle ticks
 * avoid that lock. Callers have already ordered their own aperture writes.
 */
static void framebuffer_svga_present_locked(struct framebuffer_damage damage)
{
    if (__atomic_load_n(&framebuffer_svga_damage_pending, __ATOMIC_RELAXED))
        framebuffer_damage_union(&damage, svga_damage);
    int rc = svga_update_try_locked(damage.x1, damage.y1,
        damage.x2 - damage.x1, damage.y2 - damage.y1);
    if (rc) svga_damage = damage;
    __atomic_store_n(&framebuffer_svga_damage_pending, rc != 0, __ATOMIC_RELAXED);
}

void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    bool svga_backend = fb.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    uint64_t flags = svga_backend ? svga_lock() : 0;
    if (fb.available && width && height && x < fb.width && y < fb.height) {
        if (width > fb.width - x) width = fb.width - x;
        if (height > fb.height - y) height = fb.height - y;
        struct framebuffer_damage damage = {x, y, x + width, y + height};
        if (svga_backend) {
            if (!framebuffer_drm_owned()) {
                vmware_svga_memory_fence();
                framebuffer_svga_present_locked(damage);
            }
        } else {
            framebuffer_record_damage(damage);
        }
    }
    if (svga_backend) svga_unlock(flags);
}

void framebuffer_present(void)
{
    framebuffer_present_region(0, 0, UINT32_MAX, UINT32_MAX);
}

void framebuffer_display_tick(bool graphical)
{
    if (fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA ||
        (!graphical && !__atomic_load_n(&framebuffer_svga_damage_pending, __ATOMIC_RELAXED)))
        return;
    uint64_t flags;
    if (!svga_try_lock_irqsave(&flags)) return;
    if (fb.available) {
        if (graphical && !framebuffer_drm_owned() && !svga.traces_enabled) {
            /* mmap writers cannot declare their damage on legacy hardware. */
            vmware_svga_memory_fence();
            if (!svga_update_try_locked(0, 0, fb.width, fb.height))
                framebuffer_svga_clear_damage();
        } else if (__atomic_load_n(&framebuffer_svga_damage_pending, __ATOMIC_RELAXED)) {
            framebuffer_svga_present_locked(svga_damage);
        }
    }
    svga_unlock(flags);
}

/**
 * @brief Transfer a validated SVGA scanout from console tracing to DRM presentation.
 * @return Zero, including already-owned scanout, or legacy internal -1 for unsupported framebuffer state.
 * Execution-owned task context; takes the SVGA lock internally and disables traces only on success. No heap allocation or provider self-reference.
 */
int framebuffer_drm_scanout_begin(void)
{
    if (!fb.available || fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA ||
        !vmware_svga.present || fb.bpp != 32u || fb.bytes_per_pixel != 4u ||
        !fb.width || !fb.height || fb.pitch < (uint64_t)fb.width * 4u)
        return -1;
    uint64_t flags = svga_lock();
    if (framebuffer_drm_owned()) {
        svga_unlock(flags);
        return 0;
    }
    __atomic_store_n(&framebuffer_drm_scanout_owned, true, __ATOMIC_RELEASE);
    framebuffer_svga_clear_damage();
    if (svga.bound && svga.traces_enabled) {
        svga.ops.write(SVGA_REG_TRACES, 0);
        svga.traces_enabled = false;
    }
    svga_unlock(flags);
    return 0;
}

/**
 * @brief Return the permanent SVGA scanout to ordinary framebuffer tracing.
 * Execution-owned task context after dependent jobs drain. Clears pending DRM damage and restores tracing under the SVGA lock; does not unbind the boot console or free provider-owned backing.
 */
void framebuffer_drm_scanout_release(void)
{
    uint64_t flags = svga_lock();
    __atomic_store_n(&framebuffer_drm_scanout_owned, false, __ATOMIC_RELEASE);
    framebuffer_svga_clear_damage();
    if (svga.fifo_ready && svga.bound)
        (void)svga_restore_traces_locked();
    svga_unlock(flags);
}

/**
 * @brief Copy validated XRGB8888 damage into the retained DRM-owned scanout.
 * @param pixels Readable kernel source covering every requested row.
 * @param pitch Source byte stride, at least the display width times four.
 * @param format Supported XRGB8888 format code.
 * @param x1 Inclusive left pixel.
 * @param y1 Inclusive top pixel.
 * @param x2 Exclusive right pixel, within display width.
 * @param y2 Exclusive bottom pixel, within display height.
 * @return Zero after copying/queuing damage, or legacy internal -1 for invalid or unavailable scanout.
 * Execution-owned task context; source remains borrowed only during the copy. Coalesces FIFO pressure into permanent damage state under the SVGA lock; no module/data pointer is queued.
 */
int framebuffer_drm_present_region(const void *pixels, uint32_t pitch,
                                   uint32_t format, uint32_t x1, uint32_t y1,
                                   uint32_t x2, uint32_t y2)
{
    const uint32_t drm_format_xrgb8888 = 0x34325258u;
    if (!pixels || !framebuffer_drm_owned() ||
        fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA || !fb.available ||
        format != drm_format_xrgb8888 || fb.bpp != 32u ||
        fb.bytes_per_pixel != 4u || pitch < (uint64_t)fb.width * 4u ||
        x1 >= x2 || y1 >= y2 || x2 > fb.width || y2 > fb.height)
        return -1;
    uint64_t last_source = (uint64_t)(y2 - 1u) * pitch + (uint64_t)x2 * 4u;
    if (y2 > 1u && (last_source - (uint64_t)x2 * 4u) / (y2 - 1u) != pitch)
        return -1;
    const uint8_t *source = pixels;
    for (uint32_t y = y1; y < y2; ++y) {
        uint8_t *destination = framebuffer_pixel_ptr(x1, y);
        const uint8_t *row = source + (uint64_t)y * pitch + (uint64_t)x1 * 4u;
        framebuffer_copy_io32(destination, row, x2 - x1);
    }
    /* Drain even when FIFO pressure defers publication to another CPU/tick. */
    vmware_svga_memory_fence();

    uint64_t flags = svga_lock();
    if (!framebuffer_drm_owned()) {
        svga_unlock(flags);
        return -1;
    }
    framebuffer_svga_present_locked((struct framebuffer_damage){x1, y1, x2, y2});
    svga_unlock(flags);
    /* FIFO-full damage has already been copied into GFB and is kernel-owned
     * until a nonblocking display tick publishes its coalesced rectangle. */
    return 0;
}

EXPORT_SYMBOL(framebuffer_drm_present_region);
EXPORT_SYMBOL(framebuffer_drm_scanout_begin);
EXPORT_SYMBOL(framebuffer_drm_scanout_release);
EXPORT_SYMBOL(framebuffer_generation);
EXPORT_SYMBOL(framebuffer_take_damage);
