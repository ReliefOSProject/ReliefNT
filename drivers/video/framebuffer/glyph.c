#include "internal.h"
#include <generated/cjk_font.h>

/**
 * @brief Find the immutable Unicode glyph, falling back to U+FFFD.
 * @param cp Unicode scalar to look up.
 * @return Glyph index, or zero if even the replacement is unavailable.
 */
static unsigned framebuffer_glyph_index(uint32_t cp);


/**
 * @brief Find the immutable Unicode glyph, falling back to U+FFFD.
 * @param cp Unicode scalar to look up.
 * @return Glyph index, or zero if even the replacement is unavailable.
 */
static unsigned framebuffer_glyph_index(uint32_t cp)
{
    unsigned lo = 0, hi = sizeof(cjk_glyphs) / sizeof(cjk_glyphs[0]);
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (cjk_glyphs[mid].cp < cp) lo = mid + 1;
        else hi = mid;
    }
    if (lo < sizeof(cjk_glyphs) / sizeof(cjk_glyphs[0]) && cjk_glyphs[lo].cp == cp)
        return lo;
    return cp == 0xfffdU ? 0 : framebuffer_glyph_index(0xfffdU);
}

/** @brief Return the font's one- or two-column width for a Unicode scalar. */
uint32_t framebuffer_codepoint_width(uint32_t cp)
{
    return cp < 128U ? 1U : cjk_glyphs[framebuffer_glyph_index(cp)].width;
}

/**
 * @brief Draw a Unicode scalar without allocating memory or accessing storage.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar.
 * @param fg Foreground color.
 * @param bg Background color.
 */
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t fg, uint32_t bg)
{
    framebuffer_codepoint_to(&fb, x, y, cp, fg, bg);
}

/**
 * @brief Draw a Unicode scalar into an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the glyph.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar to look up.
 * @param fg Foreground color.
 * @param bg Background color.
 */
void framebuffer_codepoint_to(const struct framebuffer *target,
                              uint32_t x, uint32_t y, uint32_t cp,
                              uint32_t fg, uint32_t bg)
{
    if (cp < 128U) {
        framebuffer_draw_glyph_to(target, x, y, reliefos_psf_glyph((char)cp), 1u, fg, bg);
        return;
    }
    unsigned index = framebuffer_glyph_index(cp);
    framebuffer_draw_glyph_to(target, x, y, cjk_glyphs[index].bits,
                              cjk_glyphs[index].width, fg, bg);
}

/** @brief Thicken the bitmap without drawing a shifted opaque background or
 * crossing into an adjacent cell. Both fonts have at most two 8-pixel columns.
 */
void framebuffer_codepoint_bold_to(const struct framebuffer *target, uint32_t x,
                                    uint32_t y, uint32_t cp, uint32_t fg, uint32_t bg)
{
    uint32_t columns = 1;
    const uint8_t *bits;
    if (cp < 128) bits = reliefos_psf_glyph((char)cp);
    else {
        unsigned index = framebuffer_glyph_index(cp);
        bits = cjk_glyphs[index].bits;
        columns = cjk_glyphs[index].width;
    }
    if (!columns || columns > 2) return;
    uint8_t bold[RELIEFOS_FONT_H * 2];
    for (uint32_t row = 0; row < RELIEFOS_FONT_H; ++row)
        for (uint32_t col = 0; col < columns; ++col) {
            uint32_t at = row * columns + col;
            bold[at] = bits[at] | (bits[at] >> 1) |
                       (col ? bits[at - 1] << 7 : 0);
        }
    framebuffer_draw_glyph_to(target, x, y, bold, columns, fg, bg);
}
