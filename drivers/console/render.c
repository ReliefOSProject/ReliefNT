#include "internal.h"

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
 * @brief Clear one text row on the current rendering surface.
 * @param row Text row to clear.
 */
static void fb_console_clear_log_line(uint32_t row);

/**
 * @brief Render one Unicode scalar on the current text-rendering surface.
 * @param ch Unicode scalar to render or terminal control to process.
 */
static void fb_console_scalar(uint32_t ch);

static uint32_t fb_console_tty_cursor_x;
static uint32_t fb_console_tty_cursor_y;
static uint8_t fb_console_tty_cursor_bpp;
static uint8_t fb_console_tty_cursor_saved[FB_CONSOLE_CURSOR_BYTES];
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
    if (fb_console_render_target) {
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

/**
 * @brief Clear one text row on the current rendering surface.
 * @param row Text row to clear.
 */
static void fb_console_clear_log_line(uint32_t row)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (!fb->available || !fb_cols || row >= fb_rows) {
        return;
    }
    fb_console_rect(fb_x, fb_y + row * RELIEFOS_FONT_H,
                    fb_cols * RELIEFOS_FONT_W, RELIEFOS_FONT_H, fb_console_bg_color);
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
    if (!fb->available || !fb->pixels || !fb_console_tty_cursor_bpp ||
        fb_console_tty_cursor_bpp > 4u ||
        fb_console_tty_cursor_x + RELIEFOS_FONT_W > fb->width ||
        fb_console_tty_cursor_y + RELIEFOS_FONT_H > fb->height) {
        return;
    }
    row_bytes = RELIEFOS_FONT_W * fb_console_tty_cursor_bpp;
    for (row = 0; row < RELIEFOS_FONT_H; ++row) {
        uint8_t *dst = (uint8_t *)fb->pixels +
                       (uint64_t)(fb_console_tty_cursor_y + row) * fb->pitch +
                       (uint64_t)fb_console_tty_cursor_x * fb_console_tty_cursor_bpp;
        uint8_t *src = fb_console_tty_cursor_saved + row * row_bytes;
        for (uint32_t byte = 0; byte < row_bytes; ++byte) {
            dst[byte] = src[byte];
        }
    }
}

/** @brief Save the underlying pixels and draw the text cursor. */
void fb_console_show_tty_cursor(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    uint32_t row;
    uint32_t row_bytes;

    if (!fb_console_tty_cursor_active || fb_console_tty_cursor_visible ||
        !fb->available || !fb->pixels || !fb_cols || !fb_rows ||
        fb_col >= fb_cols || fb_row >= fb_rows || !fb->bytes_per_pixel ||
        fb->bytes_per_pixel > 4u) {
        return;
    }
    fb_console_tty_cursor_x = fb_x + fb_col * RELIEFOS_FONT_W;
    fb_console_tty_cursor_y = fb_y + fb_row * RELIEFOS_FONT_H;
    if (fb_console_tty_cursor_x + RELIEFOS_FONT_W > fb->width ||
        fb_console_tty_cursor_y + RELIEFOS_FONT_H > fb->height) {
        return;
    }
    fb_console_tty_cursor_bpp = fb->bytes_per_pixel;
    row_bytes = RELIEFOS_FONT_W * fb_console_tty_cursor_bpp;
    for (row = 0; row < RELIEFOS_FONT_H; ++row) {
        const uint8_t *src = (const uint8_t *)fb->pixels +
                             (uint64_t)(fb_console_tty_cursor_y + row) * fb->pitch +
                             (uint64_t)fb_console_tty_cursor_x * fb_console_tty_cursor_bpp;
        uint8_t *dst = fb_console_tty_cursor_saved + row * row_bytes;
        for (uint32_t byte = 0; byte < row_bytes; ++byte) {
            dst[byte] = src[byte];
        }
    }
    fb_console_rect(fb_console_tty_cursor_x,
                    fb_console_tty_cursor_y + RELIEFOS_FONT_H - 2u,
                    RELIEFOS_FONT_W, 2u, fb_console_fg_color);
    fb_console_tty_cursor_visible = true;
}

/** @brief Scroll the active text region on the current rendering surface. */
void fb_console_scroll(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    uint32_t top = fb_scroll_top < fb_rows ? fb_scroll_top : 0u;
    uint32_t bottom = fb_scroll_bottom < fb_rows ? fb_scroll_bottom : fb_rows - 1u;
    if (bottom <= top) {
        top = 0u;
        bottom = fb_rows - 1u;
    }
    if (!fb->available || bottom <= top || fb_x >= fb->width || fb_y >= fb->height) {
        fb_console_clear_log();
        fb_row = top;
        return;
    }

    uint32_t width = fb_cols * RELIEFOS_FONT_W;
    if (width > fb->width - fb_x) {
        width = fb->width - fb_x;
    }

    uint32_t src_y = fb_y + (top + 1u) * RELIEFOS_FONT_H;
    if (src_y >= fb->height) {
        fb_console_clear_log();
        fb_row = 0;
        return;
    }

    uint32_t copy_h = (bottom - top) * RELIEFOS_FONT_H;
    if (copy_h > fb->height - src_y) {
        copy_h = fb->height - src_y;
    }

    for (uint32_t y = 0; y < copy_h; ++y) {
        uint8_t *dst = (uint8_t *)fb->pixels + (uint64_t)(fb_y + top * RELIEFOS_FONT_H + y) * fb->pitch +
                       (uint64_t)fb_x * fb->bytes_per_pixel;
        const uint8_t *src = (const uint8_t *)fb->pixels +
                             (uint64_t)(src_y + y) * fb->pitch +
                             (uint64_t)fb_x * fb->bytes_per_pixel;
        for (uint32_t byte = 0; byte < width * fb->bytes_per_pixel; ++byte) {
            dst[byte] = src[byte];
        }
    }

    fb_row = bottom;
    fb_console_clear_log_line(fb_row);
}

/** @brief Advance the text cursor; graphical panic overlays clip instead of scrolling. */
void fb_console_newline(void)
{
    fb_col = 0;
    uint32_t bottom = fb_scroll_bottom < fb_rows ? fb_scroll_bottom : fb_rows - 1u;
    if (fb_row < bottom) {
        ++fb_row;
    } else if (fb_console_panic_overlay) {
        /* GUI panic text must never scroll/copy untouched desktop pixels.
         * Keep the visible prefix; the full report still reaches serial/VT. */
        fb_console_panic_clipped = true;
    } else {
        fb_console_scroll();
    }
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
        return;
    }
    if (ch == '\n') {
        fb_console_newline();
        return;
    }
    if (ch == '\b') {
        if (fb_col) {
            --fb_col;
            fb_console_rect(fb_x + fb_col * RELIEFOS_FONT_W,
                            fb_y + fb_row * RELIEFOS_FONT_H,
                            RELIEFOS_FONT_W, RELIEFOS_FONT_H, fb_console_bg_color);
        }
        return;
    }
    if (ch == '\t') {
        uint32_t spaces = 8u - (fb_col & 7u);
        while (spaces--) {
            fb_console_scalar(' ');
        }
        return;
    }
    if (ch < 0x20u) {
        return;
    }
    if (ch == 0x7fu) {
        return;
    }
    uint32_t cells = framebuffer_codepoint_width(ch);
    if (fb_console_panic_overlay && cells > fb_cols) {
        fb_console_panic_clipped = true;
        return;
    }
    if (fb_col + cells > fb_cols) {
        fb_console_newline();
    }
    if (fb_console_panic_clipped) return;
    {
        uint32_t fg = fb_console_reverse ? fb_console_bg_color : fb_console_fg_color;
        uint32_t bg = fb_console_reverse ? fb_console_fg_color : fb_console_bg_color;
        uint32_t x = fb_x + fb_col * RELIEFOS_FONT_W;
        uint32_t y = fb_y + fb_row * RELIEFOS_FONT_H;
        fb_console_rect(x, y, RELIEFOS_FONT_W, RELIEFOS_FONT_H, bg);
        fb_console_codepoint(x, y, ch, fg, bg);
        if (fb_console_bold && x + 1u < fb->width) {
            fb_console_codepoint(x + 1u, y, ch, fg, bg);
        }
        if (fb_console_underline) {
            fb_console_rect(x, y + RELIEFOS_FONT_H - 2u, RELIEFOS_FONT_W, 1u,
                            fb_console_underline_color_set ? fb_console_underline_color : fg);
        }
    }
    fb_col += cells;
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
    fb_console_reset_attributes();
    fb_console_reset_ansi();
    /* The repaint covers the whole screen, so it must be published as a
     * whole: the scanout only refreshes regions an update command names, and
     * the grid (`fb_rows * RELIEFOS_FONT_H`) cannot reach the remainder strip
     * below the last font row. Publishing only the grid left that strip
     * showing the previous graphical picture after a switch to text. */
    if (!fb_console_render_target) {
        framebuffer_present();
    }
}
