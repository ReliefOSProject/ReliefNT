#include "internal.h"
#include <reliefnt/graphics_service.h>
#include "dec_graphics.h"

/**
 * @brief Fill a rectangle on the current text-rendering surface.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param width Rectangle width in pixels.
 * @param height Rectangle height in pixels.
 * @param color Logical RGB color to fill.
 */
static void fb_console_rect(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height, uint32_t color);

/**
 * @brief Draw a scalar on the current text-rendering surface.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar to draw.
 * @param fg Logical RGB foreground color.
 * @param bg Logical RGB background color.
 */
static void fb_console_codepoint(uint32_t x, uint32_t y, uint32_t cp,
                                 uint32_t fg, uint32_t bg);

/**
 * @brief Render one Unicode scalar on the current text-rendering surface.
 * @param ch Unicode scalar to render or terminal control to process.
 */
static void fb_console_scalar(uint32_t ch);

static uint32_t fb_console_tty_cursor_x;
static uint32_t fb_console_tty_cursor_y;
static const uint32_t *fb_console_tty_cursor_pixels;
static uint32_t fb_console_tty_cursor_pitch;
static uint8_t fb_console_tty_cursor_bpp;
static uint8_t fb_console_tty_cursor_saved[FB_CONSOLE_CURSOR_BYTES];
static struct framebuffer_damage fb_console_damage;

/** @brief Merge visible text damage; replay and fatal rendering publish separately. */
void fb_console_mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if (fb_console_render_target || fb_console_panic_overlay) return;
    const struct framebuffer *fb = framebuffer_get();
    if (!w || !h || x >= fb->width || y >= fb->height) return;
    uint32_t right = x + (w > fb->width - x ? fb->width - x : w);
    uint32_t bottom = y + (h > fb->height - y ? fb->height - y : h);
    framebuffer_damage_union(&fb_console_damage,
                             (struct framebuffer_damage){x, y, right, bottom});
}

/** @brief Publish the current drawing batch, including the software cursor. */
void fb_console_publish_damage(void)
{
    if (!fb_console_damage.x2 || fb_console_render_target) return;
    struct framebuffer_damage damage = fb_console_damage;
    fb_console_damage = (struct framebuffer_damage){0};
    framebuffer_present_region(damage.x1, damage.y1,
                               damage.x2 - damage.x1, damage.y2 - damage.y1);
}
/**
 * @brief Return the framebuffer currently used by the text renderer.
 * @return The temporary replay surface, or the active framebuffer.
 */
const struct framebuffer *fb_console_framebuffer(void)
{
    return fb_console_render_target ? fb_console_render_target : framebuffer_get();
}

/**
 * @brief Fill a rectangle on the current text-rendering surface.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param width Rectangle width in pixels.
 * @param height Rectangle height in pixels.
 * @param color Logical RGB color to fill.
 */
static void fb_console_rect(uint32_t x, uint32_t y, uint32_t width,
                            uint32_t height, uint32_t color)
{
    if (fb_console_render_target) {
        framebuffer_rect_to(fb_console_render_target, x, y, width, height, color);
    } else {
        framebuffer_rect(x, y, width, height, color);
        fb_console_mark_damage(x, y, width, height);
    }
}

/**
 * @brief Draw a scalar on the current text-rendering surface.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar to draw.
 * @param fg Logical RGB foreground color.
 * @param bg Logical RGB background color.
 */
static void fb_console_codepoint(uint32_t x, uint32_t y, uint32_t cp,
                                 uint32_t fg, uint32_t bg)
{
    if (fb_console_bold) {
        framebuffer_codepoint_bold_to(fb_console_framebuffer(), x, y, cp, fg, bg);
    } else if (fb_console_render_target) {
        framebuffer_codepoint_to(fb_console_render_target, x, y, cp, fg, bg);
    } else {
        framebuffer_codepoint(x, y, cp, fg, bg);
    }
}

uint32_t console_panel(void)
{
    /* Kernel logs and the fullscreen TTY always use the conventional
     * terminal background, independent of the desktop appearance theme. */
    return 0x00000000u;
}

uint32_t console_fg(void)
{
    return console_ui_theme == 0u ? 0x0000ff00u : 0x00ffffffu;
}

void fb_console_reset_attributes(void)
{
    fb_console_bold = false;
    fb_console_underline = false;
    fb_console_reverse = false;
    fb_modes.conceal = false;
    fb_console_underline_color = console_fg();
    fb_console_underline_color_set = false;
    fb_console_fg_color = console_fg();
    fb_console_bg_color = console_panel();
}

void fb_console_reset_ansi(void)
{
    fb_ansi_state = FB_ANSI_NORMAL;
    fb_ansi_param_count = 0;
    fb_ansi_param_value = 0;
    fb_ansi_param_active = false;
    fb_ansi_private = false;
}

/** @brief Clear the text grid on the current rendering surface. */
void fb_console_clear_log(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!fb->available || !fb_cols || !fb_rows) {
        return;
    }
    fb_console_rect(fb_x, fb_y, fb_cols * RELIEFOS_FONT_W,
                    fb_rows * RELIEFOS_FONT_H, fb_console_bg_color);
}

/** @brief Restore the saved pixels beneath the visible text cursor. */
void fb_console_hide_tty_cursor(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    uint32_t row;
    uint32_t row_bytes;

    if (!fb_console_tty_cursor_visible) {
        return;
    }
    fb_console_tty_cursor_visible = false;
    if (!fb->available || !fb->pixels || fb->pixels != fb_console_tty_cursor_pixels ||
        fb->pitch != fb_console_tty_cursor_pitch ||
        fb->bytes_per_pixel != fb_console_tty_cursor_bpp ||
        fb_console_tty_cursor_x + RELIEFOS_FONT_W > fb->width ||
        fb_console_tty_cursor_y + FB_CONSOLE_CURSOR_HEIGHT > fb->height) {
        return;
    }
    row_bytes = RELIEFOS_FONT_W * fb_console_tty_cursor_bpp;
    for (row = 0; row < FB_CONSOLE_CURSOR_HEIGHT; ++row) {
        uint8_t *dst = (uint8_t *)fb->pixels +
                       (uint64_t)(fb_console_tty_cursor_y + row) * fb->pitch +
                       (uint64_t)fb_console_tty_cursor_x * fb_console_tty_cursor_bpp;
        uint8_t *src = fb_console_tty_cursor_saved + row * row_bytes;
        for (uint32_t byte = 0; byte < row_bytes; ++byte) {
            dst[byte] = src[byte];
        }
    }
    fb_console_mark_damage(fb_console_tty_cursor_x, fb_console_tty_cursor_y,
                           RELIEFOS_FONT_W, FB_CONSOLE_CURSOR_HEIGHT);
}

/** @brief Save the underlying pixels and draw the text cursor. */
void fb_console_show_tty_cursor(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    uint32_t row;
    uint32_t row_bytes;

    if (!fb_console_tty_cursor_active || fb_console_tty_cursor_visible ||
        (console_active_vt && console_active_vt <= CONSOLE_VT_COUNT &&
         vt_text[console_active_vt - 1].cursor_hidden) ||
        !fb->available || !fb->pixels || !fb_cols || !fb_rows ||
        fb_col >= fb_cols || fb_row >= fb_rows || !fb->bytes_per_pixel ||
        fb->bytes_per_pixel > 4u) {
        return;
    }
    fb_console_tty_cursor_x = fb_x + fb_col * RELIEFOS_FONT_W;
    uint32_t cell_y = fb_y + fb_row * RELIEFOS_FONT_H;
    if (fb_console_tty_cursor_x + RELIEFOS_FONT_W > fb->width ||
        cell_y + RELIEFOS_FONT_H > fb->height ||
        fb->pitch < (uint64_t)fb->width * fb->bytes_per_pixel) {
        return;
    }
    fb_console_tty_cursor_y = cell_y + RELIEFOS_FONT_H - FB_CONSOLE_CURSOR_HEIGHT;
    fb_console_tty_cursor_pixels = fb->pixels;
    fb_console_tty_cursor_pitch = fb->pitch;
    fb_console_tty_cursor_bpp = fb->bytes_per_pixel;
    row_bytes = RELIEFOS_FONT_W * fb_console_tty_cursor_bpp;
    for (row = 0; row < FB_CONSOLE_CURSOR_HEIGHT; ++row) {
        const uint8_t *src = (const uint8_t *)fb->pixels +
                             (uint64_t)(fb_console_tty_cursor_y + row) * fb->pitch +
                             (uint64_t)fb_console_tty_cursor_x * fb_console_tty_cursor_bpp;
        uint8_t *dst = fb_console_tty_cursor_saved + row * row_bytes;
        for (uint32_t byte = 0; byte < row_bytes; ++byte) {
            dst[byte] = src[byte];
        }
    }
    fb_console_rect(fb_console_tty_cursor_x, fb_console_tty_cursor_y,
                    RELIEFOS_FONT_W, FB_CONSOLE_CURSOR_HEIGHT, fb_console_fg_color);
    fb_console_tty_cursor_visible = true;
}

/**
 * @brief Scroll an entire ANSI request with one copy of the retained rows.
 * @param lines Number of text rows to move up.
 * @par Context
 * Caller serializes console state and retains the current rendering surface.
 * Rows move upward, preserving padding and pixels outside the clipped region.
 * Uses exact forward scalar word copies with a byte tail; no allocation,
 * SIMD state or cache-attribute change. Full VRAM words avoid a separately
 * emulated uncached read/write for each byte on hardware-assisted VMs.
 * Forward reads precede their matching stores; upward row order preserves overlap.
 */
void fb_console_scroll_region(uint32_t top, uint32_t bottom, uint32_t lines, bool reverse)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!lines || !fb_rows || !fb_cols || !fb->available || !fb->pixels ||
        !fb->bytes_per_pixel || fb->bytes_per_pixel > 4u || fb_x >= fb->width ||
        fb_y >= fb->height || fb->pitch < (uint64_t)fb->width * fb->bytes_per_pixel)
        return;
    if (top > bottom || bottom >= fb_rows) return;
    uint32_t width = fb_cols * RELIEFOS_FONT_W;
    if (width > fb->width - fb_x) {
        width = fb->width - fb_x;
    }

    uint32_t rows = bottom - top + 1u;
    if (lines > rows) lines = rows;
    uint32_t dst_y = fb_y + top * RELIEFOS_FONT_H;
    uint32_t src_y = dst_y + (reverse ? 0u : lines * RELIEFOS_FONT_H);
    if (reverse) dst_y += lines * RELIEFOS_FONT_H;
    uint32_t copy_h = (rows - lines) * RELIEFOS_FONT_H;
    if (src_y >= fb->height) copy_h = 0;
    else if (copy_h > fb->height - src_y) copy_h = fb->height - src_y;

    size_t bytes = (size_t)width * fb->bytes_per_pixel;
    if (dst_y >= fb->height) copy_h = 0;
    else if (copy_h > fb->height - dst_y) copy_h = fb->height - dst_y;
    for (uint32_t row = 0; row < copy_h; ++row) {
        uint32_t y = reverse ? copy_h - row - 1u : row;
        uint8_t *dst = (uint8_t *)fb->pixels + (uint64_t)(dst_y + y) * fb->pitch +
                       (uint64_t)fb_x * fb->bytes_per_pixel;
        const uint8_t *src = (const uint8_t *)fb->pixels +
                             (uint64_t)(src_y + y) * fb->pitch +
                             (uint64_t)fb_x * fb->bytes_per_pixel;
        size_t words = bytes / sizeof(uint64_t), tail = bytes % sizeof(uint64_t);
        /* x86 permits unaligned scalar words; neither operation crosses the
         * exact clipped extent, including 24-bit/padded framebuffer layouts. */
        __asm__ volatile("cld; rep movsq"
                         : "+D"(dst), "+S"(src), "+c"(words) : : "memory", "cc");
        __asm__ volatile("rep movsb"
                         : "+D"(dst), "+S"(src), "+c"(tail) : : "memory", "cc");
    }

    uint32_t first_y = fb_y + top * RELIEFOS_FONT_H;
    fb_console_rect(fb_x, reverse ? first_y : first_y + copy_h, width, lines * RELIEFOS_FONT_H,
                    fb_console_bg_color);
    fb_console_mark_damage(fb_x, first_y, width, rows * RELIEFOS_FONT_H);
}

/** @brief Scroll the configured margins upward without moving the cursor. */
void fb_console_scroll(uint32_t lines)
{
    if (!fb_rows) return;
    uint32_t top = fb_scroll_top < fb_rows ? fb_scroll_top : 0u;
    uint32_t bottom = fb_scroll_bottom < fb_rows ? fb_scroll_bottom : fb_rows - 1u;
    fb_console_scroll_region(top, bottom, lines, false);
}

/** @brief Apply line feed/reverse index without crossing the scrolling margins. */
void fb_console_index(bool reverse, bool carriage)
{
    if (!fb_rows) return;
    if (carriage) fb_col = 0;
    fb_modes.pending_wrap = false;
    uint32_t bottom = fb_scroll_bottom < fb_rows ? fb_scroll_bottom : fb_rows - 1u;
    if (reverse) {
        if (fb_row == fb_scroll_top)
            fb_console_scroll_region(fb_scroll_top, bottom, 1u, true);
        else if (fb_row) --fb_row;
    } else if (fb_row != bottom) {
        if (fb_row < fb_rows - 1u) ++fb_row;
    } else if (fb_console_panic_overlay) {
        /* GUI panic text must never scroll/copy untouched desktop pixels.
         * Keep the visible prefix; the full report still reaches serial/VT. */
        fb_console_panic_clipped = true;
    } else {
        fb_console_scroll(1u);
    }
}

/** @brief Advance to column zero of the next row for logs, wrap and panic text. */
void fb_console_newline(void) { fb_console_index(false, true); }

/** @brief Tabulation moves the cursor without painting over the traversed text. */
void fb_console_tab(uint32_t count, bool reverse)
{
    if (!fb_cols) return;
    if (count > fb_cols) count = fb_cols;
    while (count--) {
        do {
            if (reverse ? !fb_col : fb_col >= fb_cols - 1u) break;
            if (reverse) --fb_col;
            else ++fb_col;
        } while (!fb_modes.tabs[fb_col]);
    }
    fb_modes.pending_wrap = false;
}

/**
 * @brief Render one Unicode scalar on the current text-rendering surface.
 * @param ch Unicode scalar to render or terminal control to process.
 */
static void fb_console_scalar(uint32_t ch)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!fb_console_enabled || !fb->available || !fb_cols || !fb_rows ||
        fb_console_panic_clipped) {
        return;
    }
    if (ch == '\r') {
        fb_col = 0;
        fb_modes.pending_wrap = false;
        return;
    }
    if (ch == '\n') {
        uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
        fb_console_index(false, fb_modes.newline || !vt || fb_console_panic_overlay);
        return;
    }
    if (ch == '\b') {
        /* Readline uses BS to move across existing text. */
        if (fb_col) {
            --fb_col;
        }
        fb_modes.pending_wrap = false;
        return;
    }
    if (ch == '\t') {
        fb_console_tab(1, false);
        return;
    }
    if (ch < 0x20u) {
        return;
    }
    if (ch == 0x7fu) {
        return;
    }
    uint32_t number = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (number && number <= CONSOLE_VT_COUNT) {
        const struct console_vt_text *state = &vt_text[number - 1];
        if (state->charset[state->charset_bank]) ch = console_dec_graphic(ch);
    }
    uint32_t cells = framebuffer_codepoint_width(ch);
    if (fb_console_panic_overlay && cells > fb_cols) {
        fb_console_panic_clipped = true;
        return;
    }
    if (fb_modes.pending_wrap && fb_modes.wrap) fb_console_newline();
    if (cells > fb_cols) return;
    if (fb_col + cells > fb_cols) {
        if (fb_modes.wrap) fb_console_newline();
        else fb_col = fb_cols - cells;
    }
    if (fb_console_panic_clipped) return;
    if (fb_console_insert_mode) fb_console_shift_characters(cells, true);
    {
        uint32_t fg = fb_console_reverse ? fb_console_bg_color : fb_console_fg_color;
        uint32_t bg = fb_console_reverse ? fb_console_fg_color : fb_console_bg_color;
        if (fb_modes.conceal) fg = bg;
        uint32_t x = fb_x + fb_col * RELIEFOS_FONT_W;
        uint32_t y = fb_y + fb_row * RELIEFOS_FONT_H;
        uint32_t width = cells * RELIEFOS_FONT_W;
        fb_console_codepoint(x, y, ch, fg, bg);
        if (fb_console_underline) {
            fb_console_rect(x, y + RELIEFOS_FONT_H - 2u, width, 1u,
                            fb_console_underline_color_set ? fb_console_underline_color : fg);
        }
        if (fb_console_panic_overlay) {
            if (x < fb->width) {
                if (width > fb->width - x) width = fb->width - x;
                graphics_service_panic_damage(x, y, width, RELIEFOS_FONT_H);
            }
        }
        else
            fb_console_mark_damage(x, y, width, RELIEFOS_FONT_H);
    }
    fb_modes.last = ch;
    fb_col += cells;
    fb_modes.pending_wrap = fb_col == fb_cols && fb_modes.wrap;
    if (fb_col >= fb_cols) fb_col = fb_cols - 1u;
}

/** @brief Decode one output byte, retaining incomplete UTF-8 across writes. */
void fb_console_putc(char ch)
{
    uint32_t cp;
    int result = reliefos_utf8_feed(&console_utf8, (unsigned char)ch, &cp);
    if (result) fb_console_scalar(cp);
    if (result < 0 && reliefos_utf8_feed(&console_utf8, (unsigned char)ch, &cp))
        fb_console_scalar(cp);
}

void fb_console_repeat(uint32_t count)
{
    if (count > fb_cols * fb_rows) count = fb_cols * fb_rows;
    uint32_t cp = fb_modes.last;
    if (cp) while (count--) fb_console_scalar(cp);
}

/**
 * @brief Clear a half-open cell range in one text row.
 * @param row Text row to clear.
 * @param first First cell in the range.
 * @param last End cell, not included.
 */
void fb_console_clear_row_range(uint32_t row, uint32_t first, uint32_t last)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!fb->available || row >= fb_rows || first >= fb_cols) {
        return;
    }
    if (last > fb_cols) {
        last = fb_cols;
    }
    if (last <= first) {
        return;
    }
    fb_console_rect(fb_x + first * RELIEFOS_FONT_W,
                    fb_y + row * RELIEFOS_FONT_H,
                    (last - first) * RELIEFOS_FONT_W, RELIEFOS_FONT_H,
                    fb_console_bg_color);
}

/** @brief Move an overlapping suffix in each scanline, retaining glyph colors
 * and styles. Clip to complete visible cells and preserve pitch padding.
 * No allocation or cursor movement; the caller hides the software cursor.
 */
void fb_console_shift_characters(uint32_t count, bool insert)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!count || !fb->available || !fb->pixels || !fb_cols || !fb_rows ||
        fb_col >= fb_cols || fb_row >= fb_rows || !fb->bytes_per_pixel ||
        fb->bytes_per_pixel > 4u || fb_x >= fb->width || fb_y >= fb->height ||
        fb->pitch < (uint64_t)fb->width * fb->bytes_per_pixel)
        return;

    uint32_t cols = (fb->width - fb_x) / RELIEFOS_FONT_W;
    if (cols > fb_cols) cols = fb_cols;
    if (fb_col >= cols || fb_row >= (fb->height - fb_y) / RELIEFOS_FONT_H)
        return;
    if (count > cols - fb_col) count = cols - fb_col;
    uint32_t x = fb_x + fb_col * RELIEFOS_FONT_W;
    uint32_t y = fb_y + fb_row * RELIEFOS_FONT_H;
    size_t shift = (size_t)count * RELIEFOS_FONT_W * fb->bytes_per_pixel;
    size_t keep = (size_t)(cols - fb_col - count) * RELIEFOS_FONT_W * fb->bytes_per_pixel;
    for (uint32_t row = 0; keep && row < RELIEFOS_FONT_H; ++row) {
        uint8_t *pixels = (uint8_t *)fb->pixels + (uint64_t)(y + row) * fb->pitch +
                          (uint64_t)x * fb->bytes_per_pixel;
        if (insert) __builtin_memmove(pixels + shift, pixels, keep);
        else __builtin_memmove(pixels, pixels + shift, keep);
    }
    uint32_t first = insert ? fb_col : cols - count;
    fb_console_clear_row_range(fb_row, first, first + count);
    fb_console_mark_damage(x, y, (cols - fb_col) * RELIEFOS_FONT_W, RELIEFOS_FONT_H);
}

/** @brief Reset text state and clear the full active rendering surface. */
void fb_console_initialize_fullscreen(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!fb->available) {
        return;
    }
    fb_x = 0;
    fb_console_rect(0, 0, fb->width, fb->height, console_panel());
    fb_y = console_boot_logo_active && !console_active_vt ?
           boot_logo_draw(fb, smp_cpu_count()) : 0u;
    fb_cols = fb->width / RELIEFOS_FONT_W;
    fb_rows = (fb->height - fb_y) / RELIEFOS_FONT_H;
    if (fb_cols > CONSOLE_MAX_COLS) {
        fb_cols = CONSOLE_MAX_COLS;
    }
    if (fb_rows > CONSOLE_MAX_ROWS) {
        fb_rows = CONSOLE_MAX_ROWS;
    }
    fb_col = 0;
    fb_row = 0;
    fb_saved_col = 0;
    fb_saved_row = 0;
    fb_scroll_top = 0u;
    fb_scroll_bottom = fb_rows ? fb_rows - 1u : 0u;
    fb_console_insert_mode = false;
    fb_modes = (struct fb_terminal_modes){.wrap = true};
    for (uint32_t col = 8; col < fb_cols; col += 8) fb_modes.tabs[col] = true;
    fb_console_reset_attributes();
    fb_console_save_cursor();
    fb_console_reset_ansi();
    /* The full clear marks even the strip below the last font row. Keep it
     * in the same publication as the replayed text and boot logo. */
}
