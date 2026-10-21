#include "internal.h"

static void framebuffer_write_native(uint32_t x, uint32_t y, uint32_t color);

/**
 * @brief Store one native-format pixel in an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the pixel.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param color Native-format pixel value.
 */
static void framebuffer_write_native_to(const struct framebuffer *target,
                                        uint32_t x, uint32_t y, uint32_t color);

static uint32_t framebuffer_read_native(uint32_t x, uint32_t y);

static void framebuffer_char(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg);

static void framebuffer_char_transparent(uint32_t x, uint32_t y, char ch, uint32_t fg);


uint8_t *framebuffer_pixel_ptr(uint32_t x, uint32_t y)
{
    return (uint8_t *)fb.pixels + (uint64_t)y * fb.pitch +
           (uint64_t)x * fb.bytes_per_pixel;
}

static void framebuffer_write_native(uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *pixel = framebuffer_pixel_ptr(x, y);
    for (uint8_t byte = 0; byte < fb.bytes_per_pixel; ++byte) {
        pixel[byte] = (uint8_t)(color >> (byte * 8u));
    }
}

/**
 * @brief Store one native-format pixel in an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the pixel.
 * @param x Pixel column.
 * @param y Pixel row.
 * @param color Native-format pixel value.
 */
static void framebuffer_write_native_to(const struct framebuffer *target,
                                        uint32_t x, uint32_t y, uint32_t color)
{
    uint8_t *pixel = (uint8_t *)target->pixels + (uint64_t)y * target->pitch +
                     (uint64_t)x * target->bytes_per_pixel;
    for (uint8_t byte = 0; byte < target->bytes_per_pixel; ++byte) {
        pixel[byte] = (uint8_t)(color >> (byte * 8u));
    }
}

static uint32_t framebuffer_read_native(uint32_t x, uint32_t y)
{
    const uint8_t *pixel = framebuffer_pixel_ptr(x, y);
    uint32_t color = 0;
    for (uint8_t byte = 0; byte < fb.bytes_per_pixel; ++byte) {
        color |= (uint32_t)pixel[byte] << (byte * 8u);
    }
    return color;
}

void framebuffer_clear(uint32_t color)
{
    uint32_t native_color;
    if (!fb.available) {
        return;
    }
    native_color = framebuffer_native_color(color);
    for (uint32_t y = 0; y < fb.height; ++y) {
        for (uint32_t x = 0; x < fb.width; ++x) {
            framebuffer_write_native(x, y, native_color);
        }
    }
}

void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    framebuffer_rect_to(&fb, x, y, w, h, color);
}

/**
 * @brief Fill an axis-aligned rectangle in an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the fill.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param w Rectangle width in pixels.
 * @param h Rectangle height in pixels.
 * @param color Logical RGB color to fill.
 */
void framebuffer_rect_to(const struct framebuffer *target,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t color)
{
    uint64_t minimum_pitch;
    uint32_t native_color;
    if (!target || !target->available || !target->pixels || !target->width ||
        !target->height || !target->bytes_per_pixel ||
        target->bytes_per_pixel > 4u || x >= target->width || y >= target->height) {
        return;
    }
    minimum_pitch = (uint64_t)target->width * target->bytes_per_pixel;
    if (target->pitch < minimum_pitch) {
        return;
    }
    if (w > target->width - x) {
        w = target->width - x;
    }
    if (h > target->height - y) {
        h = target->height - y;
    }
    native_color = framebuffer_native_color_for(target, color);
    for (uint32_t yy = y; yy < y + h; ++yy) {
        for (uint32_t xx = x; xx < x + w; ++xx) {
            framebuffer_write_native_to(target, xx, yy, native_color);
        }
    }
}

void framebuffer_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride, const uint32_t *pixels)
{
    if (!fb.available || !pixels || x >= fb.width || y >= fb.height || stride < w) {
        return;
    }
    if (x + w > fb.width) {
        w = fb.width - x;
    }
    if (y + h > fb.height) {
        h = fb.height - y;
    }
    for (uint32_t yy = 0; yy < h; ++yy) {
        const uint32_t *src = pixels + (uint64_t)yy * stride;
        for (uint32_t xx = 0; xx < w; ++xx) {
            framebuffer_write_native(x + xx, y + yy,
                                     framebuffer_native_color(src[xx]));
        }
    }
}

uint32_t framebuffer_get_pixel(uint32_t x, uint32_t y)
{
    if (!fb.available || x >= fb.width || y >= fb.height) {
        return 0;
    }
    return framebuffer_logical_color(framebuffer_read_native(x, y));
}

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!fb.available || x >= fb.width || y >= fb.height) {
        return;
    }
    framebuffer_write_native(x, y, framebuffer_native_color(color));
}

uint32_t framebuffer_get_pixel_public(uint32_t x, uint32_t y)
{
    return framebuffer_get_pixel(x, y);
}

void framebuffer_put_pixel_public(uint32_t x, uint32_t y, uint32_t color)
{
    framebuffer_put_pixel(x, y, color);
}

static void framebuffer_char(uint32_t x, uint32_t y, char ch, uint32_t fg, uint32_t bg)
{
    const uint8_t *glyph = reliefos_psf_glyph(ch);
    for (uint32_t row = 0; row < RELIEFOS_FONT_H; ++row) {
        for (uint32_t col = 0; col < RELIEFOS_FONT_W; ++col) {
            uint32_t color = (glyph[row] & (uint8_t)(0x80u >> col)) ? fg : bg;
            framebuffer_rect(x + col, y + row, 1, 1, color);
        }
    }
}

static void framebuffer_char_transparent(uint32_t x, uint32_t y, char ch, uint32_t fg)
{
    const uint8_t *glyph = reliefos_psf_glyph(ch);
    for (uint32_t row = 0; row < RELIEFOS_FONT_H; ++row) {
        for (uint32_t col = 0; col < RELIEFOS_FONT_W; ++col) {
            if (glyph[row] & (uint8_t)(0x80u >> col)) {
                framebuffer_rect(x + col, y + row, 1, 1, fg);
            }
        }
    }
}

void framebuffer_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg)
{
    for (uint32_t i = 0; text && text[i]; ++i) {
        framebuffer_char(x + i * RELIEFOS_FONT_W, y, text[i], fg, bg);
    }
}

void framebuffer_text_transparent(uint32_t x, uint32_t y, const char *text, uint32_t fg)
{
    for (uint32_t i = 0; text && text[i]; ++i) {
        framebuffer_char_transparent(x + i * RELIEFOS_FONT_W, y, text[i], fg);
    }
}
