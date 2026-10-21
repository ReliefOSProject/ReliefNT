/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/boot_logo.h>
#include <reliefnt/framebuffer.h>
#include <reliefos/psf_font.h>
#include "boot_logo_data.h"

#define LOGO_SOURCE_SIZE 80u
#define LOGO_MIN_SIZE 16u
#define LOGO_MARGIN 16u
#define LOGO_TEXT_ROWS 4u

/**
 * @brief Paint embedded logos with bounds, spacing and native pixel conversion.
 * @param target Valid framebuffer surface owned by the BSP, or NULL.
 * @param cpu_count Number of discovered logical CPUs to represent.
 * @return Reserved, font-aligned top height; zero means nothing was drawn.
 */
uint32_t boot_logo_draw(const struct framebuffer *target, uint32_t cpu_count)
{
    uint32_t band, size = LOGO_SOURCE_SIZE, gap, columns, max_rows;
    uint32_t count, rows, reserved;
    uint64_t capacity;

    if (!target || !target->available || !target->pixels || !cpu_count ||
        (target->bytes_per_pixel != 3u && target->bytes_per_pixel != 4u) ||
        target->pitch < (uint64_t)target->width * target->bytes_per_pixel ||
        target->width < 2u * LOGO_MARGIN + LOGO_MIN_SIZE ||
        target->height < LOGO_TEXT_ROWS * RELIEFOS_FONT_H) {
        return 0;
    }
    band = (target->height - LOGO_TEXT_ROWS * RELIEFOS_FONT_H) /
           RELIEFOS_FONT_H * RELIEFOS_FONT_H;
    if (band < 2u * LOGO_MARGIN + LOGO_MIN_SIZE) return 0;

    for (;;) {
        gap = size / 5u;
        if (gap < 4u) gap = 4u;
        columns = (target->width - 2u * LOGO_MARGIN + gap) / (size + gap);
        max_rows = (band - 2u * LOGO_MARGIN + gap) / (size + gap);
        capacity = (uint64_t)columns * max_rows;
        if (capacity >= cpu_count || size == LOGO_MIN_SIZE) break;
        size /= 2u;
        if (size < LOGO_MIN_SIZE) size = LOGO_MIN_SIZE;
    }
    if (!capacity) return 0;
    count = capacity < cpu_count ? (uint32_t)capacity : cpu_count;
    rows = 1u + (count - 1u) / columns;
    reserved = 2u * LOGO_MARGIN + rows * size + (rows - 1u) * gap;
    reserved = (reserved + RELIEFOS_FONT_H - 1u) /
               RELIEFOS_FONT_H * RELIEFOS_FONT_H;
    framebuffer_rect_to(target, 0, 0, target->width, reserved, 0);

    for (uint32_t index = 0; index < count; ++index) {
        uint32_t left = LOGO_MARGIN + (index % columns) * (size + gap);
        uint32_t top = LOGO_MARGIN + (index / columns) * (size + gap);
        for (uint32_t y = 0; y < size; ++y) {
            const uint8_t *source = boot_logo_pixels +
                (y * LOGO_SOURCE_SIZE / size) * LOGO_SOURCE_SIZE;
            uint32_t x = 0;
            while (x < size) {
                uint32_t start = x;
                uint8_t color = source[x * LOGO_SOURCE_SIZE / size];
                do { ++x; } while (x < size &&
                    source[x * LOGO_SOURCE_SIZE / size] == color);
                if (boot_logo_palette[color])
                    framebuffer_rect_to(target, left + start, top + y,
                                        x - start, 1, boot_logo_palette[color]);
            }
        }
    }
    return reserved;
}
