#include "internal.h"

static uint64_t framebuffer_channel_max(uint8_t mask_size);

static uint32_t framebuffer_component_to_native(uint8_t component,
                                                uint8_t field_position,
                                                uint8_t mask_size);

static uint8_t framebuffer_component_from_native(uint32_t pixel,
                                                 uint8_t field_position,
                                                 uint8_t mask_size);


int framebuffer_color_format_valid(const struct framebuffer *target)
{
    if (!target || target->type != MULTIBOOT2_FRAMEBUFFER_TYPE_RGB ||
        !target->red_mask_size || !target->green_mask_size ||
        !target->blue_mask_size || target->red_mask_size > 32u ||
        target->green_mask_size > 32u || target->blue_mask_size > 32u) {
        return 0;
    }
    return target->red_field_position <= 32u - target->red_mask_size &&
           target->green_field_position <= 32u - target->green_mask_size &&
           target->blue_field_position <= 32u - target->blue_mask_size;
}

static uint64_t framebuffer_channel_max(uint8_t mask_size)
{
    return mask_size == 32u ? 0xffffffffULL : ((1ULL << mask_size) - 1ULL);
}

static uint32_t framebuffer_component_to_native(uint8_t component,
                                                uint8_t field_position,
                                                uint8_t mask_size)
{
    uint64_t max_value = framebuffer_channel_max(mask_size);
    uint64_t scaled = ((uint64_t)component * max_value + 127ULL) / 255ULL;
    return (uint32_t)(scaled << field_position);
}

static uint8_t framebuffer_component_from_native(uint32_t pixel,
                                                 uint8_t field_position,
                                                 uint8_t mask_size)
{
    uint64_t max_value = framebuffer_channel_max(mask_size);
    uint64_t raw = ((uint64_t)pixel >> field_position) & max_value;
    return (uint8_t)((raw * 255ULL + max_value / 2ULL) / max_value);
}

/**
 * @brief Convert logical RGB to the native channel layout of a framebuffer.
 * @param target Framebuffer format that determines the native channel layout.
 * @param color Logical RGB color to convert.
 * @return Native-format pixel value.
 */
uint32_t framebuffer_native_color_for(const struct framebuffer *target,
                                             uint32_t color)
{
    uint8_t red_position = 16u;
    uint8_t red_size = 8u;
    uint8_t green_position = 8u;
    uint8_t green_size = 8u;
    uint8_t blue_position = 0u;
    uint8_t blue_size = 8u;

    if (framebuffer_color_format_valid(target)) {
        red_position = target->red_field_position;
        red_size = target->red_mask_size;
        green_position = target->green_field_position;
        green_size = target->green_mask_size;
        blue_position = target->blue_field_position;
        blue_size = target->blue_mask_size;
    }

    return framebuffer_component_to_native((uint8_t)(color >> 16),
                                           red_position, red_size) |
           framebuffer_component_to_native((uint8_t)(color >> 8),
                                           green_position, green_size) |
           framebuffer_component_to_native((uint8_t)color,
                                           blue_position, blue_size);
}

uint32_t framebuffer_native_color(uint32_t color)
{
    return framebuffer_native_color_for(&fb, color);
}

uint32_t framebuffer_logical_color(uint32_t pixel)
{
    uint8_t red_position = 16u;
    uint8_t red_size = 8u;
    uint8_t green_position = 8u;
    uint8_t green_size = 8u;
    uint8_t blue_position = 0u;
    uint8_t blue_size = 8u;

    if (framebuffer_color_format_valid(&fb)) {
        red_position = fb.red_field_position;
        red_size = fb.red_mask_size;
        green_position = fb.green_field_position;
        green_size = fb.green_mask_size;
        blue_position = fb.blue_field_position;
        blue_size = fb.blue_mask_size;
    }

    return ((uint32_t)framebuffer_component_from_native(pixel, red_position, red_size) << 16) |
           ((uint32_t)framebuffer_component_from_native(pixel, green_position, green_size) << 8) |
           (uint32_t)framebuffer_component_from_native(pixel, blue_position, blue_size);
}

int framebuffer_gop_mask(uint32_t mask, uint8_t *position, uint8_t *size)
{
    uint8_t shift = 0;
    uint8_t width = 0;

    if (!mask || !position || !size) {
        return 0;
    }
    while (!(mask & 1u)) {
        mask >>= 1;
        ++shift;
    }
    while (mask & 1u) {
        mask >>= 1;
        ++width;
    }
    if (mask != 0 || !width || shift > 32u - width) {
        return 0;
    }
    *position = shift;
    *size = width;
    return 1;
}

void framebuffer_set_default_format(void)
{
    fb.type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
    fb.red_field_position = 16u;
    fb.red_mask_size = 8u;
    fb.green_field_position = 8u;
    fb.green_mask_size = 8u;
    fb.blue_field_position = 0u;
    fb.blue_mask_size = 8u;
}

uint8_t framebuffer_bytes_per_pixel(uint32_t pitch, uint32_t width,
                                           uint8_t bpp)
{
    uint32_t expected = ((uint32_t)bpp + 7u) / 8u;

    if (!width) {
        return 0;
    }
    if ((expected == 3u || expected == 4u) &&
        (uint64_t)width * expected <= pitch) {
        return (uint8_t)expected;
    }

    /* Some QEMU VGA modes report 32bpp but expose a packed 24-bit scanline. */
    if (pitch % width == 0u) {
        uint32_t actual = pitch / width;
        if (actual == 3u || actual == 4u) {
            return (uint8_t)actual;
        }
    }
    return 0;
}
