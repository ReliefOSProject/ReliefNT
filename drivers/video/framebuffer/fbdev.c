/* Linux fbdev variable-screen-info ABI for the supported ReliefOS backends. */
#include <linux/errno.h>
#include <linux/fb.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/multiboot2.h>

static bool fbdev_bitfield_matches(const struct fb_bitfield *field,
                                   uint32_t offset, uint32_t length)
{
    return field->offset == offset && field->length == length &&
           field->msb_right == 0;
}

static bool fbdev_format_matches(const struct fb_var_screeninfo *var,
                                 uint32_t red_offset, uint32_t red_length,
                                 uint32_t green_offset, uint32_t green_length,
                                 uint32_t blue_offset, uint32_t blue_length)
{
    return fbdev_bitfield_matches(&var->red, red_offset, red_length) &&
           fbdev_bitfield_matches(&var->green, green_offset, green_length) &&
           fbdev_bitfield_matches(&var->blue, blue_offset, blue_length) &&
           fbdev_bitfield_matches(&var->transp, 0, 0);
}

static bool fbdev_matches_current_format(const struct fb_var_screeninfo *var,
                                         const struct framebuffer *fb)
{
    return fbdev_format_matches(var,
                                fb->red_field_position, fb->red_mask_size,
                                fb->green_field_position, fb->green_mask_size,
                                fb->blue_field_position, fb->blue_mask_size);
}

static bool fbdev_matches_xrgb8888(const struct fb_var_screeninfo *var)
{
    return fbdev_format_matches(var, 16, 8, 8, 8, 0, 8);
}

static void fbdev_copy_current_format(struct fb_var_screeninfo *var,
                                      const struct framebuffer *fb)
{
    var->bits_per_pixel = fb->bpp;
    var->red = (struct fb_bitfield){fb->red_field_position,
                                    fb->red_mask_size, 0};
    var->green = (struct fb_bitfield){fb->green_field_position,
                                      fb->green_mask_size, 0};
    var->blue = (struct fb_bitfield){fb->blue_field_position,
                                     fb->blue_mask_size, 0};
    var->transp = (struct fb_bitfield){0};
}

static void fbdev_copy_xrgb8888_format(struct fb_var_screeninfo *var)
{
    var->bits_per_pixel = 32;
    var->red = (struct fb_bitfield){16, 8, 0};
    var->green = (struct fb_bitfield){8, 8, 0};
    var->blue = (struct fb_bitfield){0, 8, 0};
    var->transp = (struct fb_bitfield){0};
}

static bool fbdev_has_only_supported_fields(const struct fb_var_screeninfo *var)
{
    return var->grayscale == 0 && var->nonstd == 0 && var->accel_flags == 0 &&
           var->rotate == 0 && var->colorspace == 0 &&
           var->reserved[0] == 0 && var->reserved[1] == 0 &&
           var->reserved[2] == 0 && var->reserved[3] == 0;
}

static bool fbdev_geometry_valid(const struct fb_var_screeninfo *var)
{
    if (!var->xres || !var->yres || var->xres_virtual != var->xres ||
        var->yres_virtual != var->yres || var->xoffset || var->yoffset)
        return false;
    return true;
}

static bool fbdev_mode_geometry_supported(const struct fb_var_screeninfo *var,
                                          const struct framebuffer *fb)
{
    if (!fb->max_width || !fb->max_height || !fb->max_bytes ||
        var->xres > fb->max_width || var->yres > fb->max_height)
        return false;
    /* Compare in pixels to avoid overflowing when untrusted 32-bit geometry
     * is multiplied by the four-byte XRGB storage size. */
    return (uint64_t)var->xres * var->yres <= fb->max_bytes / 4u;
}

static bool fbdev_mode_setting_supported(const struct framebuffer *fb)
{
    return (fb->capabilities & FRAMEBUFFER_CAP_MODE_SET) != 0 &&
           (fb->backend == FRAMEBUFFER_BACKEND_BOCHS_VBE ||
            fb->backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA);
}

static void fbdev_normalize_geometry(struct fb_var_screeninfo *var,
                                     uint32_t width, uint32_t height)
{
    var->xres = width;
    var->yres = height;
    var->xres_virtual = width;
    var->yres_virtual = height;
    var->xoffset = 0;
    var->yoffset = 0;
}

/**
 * @brief Validate one Linux fbdev mode request and apply only FB_ACTIVATE_NOW.
 * @param var User-independent request copy; updated only after validation and
 *            successful application, with the actual or normalized layout.
 * @return Zero on success, -EINVAL for unsupported mode/flags, or -ENODEV
 *         when no usable framebuffer is available.
 */
int framebuffer_fbdev_put_var(struct fb_var_screeninfo *var)
{
    struct fb_var_screeninfo normalized;
    const struct framebuffer *fb = framebuffer_get();
    uint32_t activation;
    bool current_layout;
    bool test_only;

    if (!var) return -LINUX_EINVAL;
    if (!fb || !fb->available) return -LINUX_ENODEV;

    normalized = *var;
    activation = normalized.activate & ~FB_ACTIVATE_KD_TEXT;
    if (activation & FB_ACTIVATE_INV_MODE) return -LINUX_EINVAL;
    /* Linux treats FORCE as a modifier; the activation mask still decides
     * whether this request is applied or validation-only. */
    if (activation & ~(FB_ACTIVATE_MASK | FB_ACTIVATE_FORCE))
        return -LINUX_EINVAL;
    switch (activation & FB_ACTIVATE_MASK) {
    case FB_ACTIVATE_NOW:
        test_only = false;
        break;
    case FB_ACTIVATE_NXTOPEN:
    case FB_ACTIVATE_TEST:
        test_only = true;
        break;
    default:
        return -LINUX_EINVAL;
    }
    if (!fbdev_has_only_supported_fields(&normalized) ||
        !fbdev_geometry_valid(&normalized))
        return -LINUX_EINVAL;

    current_layout = normalized.xres == fb->width &&
                     normalized.yres == fb->height &&
                     normalized.bits_per_pixel == fb->bpp &&
                     fbdev_matches_current_format(&normalized, fb) &&
                     ((fb->bpp == 24 && fb->bytes_per_pixel == 3) ||
                      (fb->bpp == 32 && fb->bytes_per_pixel == 4));

    /* The existing firmware-provided 24bpp surface is usable at its current
     * geometry, but the mode backends always switch to XRGB8888. */
    if (normalized.bits_per_pixel == 24) {
        if (fb->type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB || fb->bpp != 24 ||
            !current_layout) return -LINUX_EINVAL;
        fbdev_copy_current_format(&normalized, fb);
        fbdev_normalize_geometry(&normalized, fb->width, fb->height);
        normalized.activate = activation;
        *var = normalized;
        return 0;
    }

    if (normalized.bits_per_pixel != 32) return -LINUX_EINVAL;
    if (current_layout && fb->type == MULTIBOOT2_FRAMEBUFFER_TYPE_RGB &&
        (test_only || !(activation & FB_ACTIVATE_FORCE))) {
        fbdev_copy_current_format(&normalized, fb);
        fbdev_normalize_geometry(&normalized, fb->width, fb->height);
        normalized.activate = activation;
        *var = normalized;
        return 0;
    }
    if (!fbdev_matches_xrgb8888(&normalized) ||
        !fbdev_mode_geometry_supported(&normalized, fb) ||
        !fbdev_mode_setting_supported(fb))
        return -LINUX_EINVAL;

    if (!test_only) {
        if (framebuffer_set_mode(normalized.xres, normalized.yres) != 0)
            return -LINUX_EINVAL;
        fb = framebuffer_get();
        /* A DRM KMS lease permits an identical-size request to return success
         * while retaining its 24bpp state. Never report the requested 32bpp
         * format unless the framebuffer actually adopted it. */
        if (!fb || !fb->available || fb->width != normalized.xres ||
            fb->height != normalized.yres || fb->bpp != 32 ||
            fb->type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB ||
            fb->bytes_per_pixel != 4 || fb->pitch < (uint64_t)fb->width * 4u ||
            fb->red_field_position != 16 || fb->red_mask_size != 8 ||
            fb->green_field_position != 8 || fb->green_mask_size != 8 ||
            fb->blue_field_position != 0 || fb->blue_mask_size != 8)
            return -LINUX_EINVAL;
        fbdev_copy_current_format(&normalized, fb);
        fbdev_normalize_geometry(&normalized, fb->width, fb->height);
    } else {
        fbdev_copy_xrgb8888_format(&normalized);
    }
    normalized.activate = activation;
    *var = normalized;
    return 0;
}
