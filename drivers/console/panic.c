#include "internal.h"

/**
 * @brief Record panic bytes in the kernel ring and the current VT only.
 * @param text Bytes to retain, including controlled SGR attributes.
 * @param length Number of bytes to append.
 */
static void console_panic_store(const char *text, size_t length);


/**
 * @brief Record panic bytes in the kernel ring and the current VT only.
 * @param text Bytes to retain, including controlled SGR attributes.
 * @param length Number of bytes to append.
 */
static void console_panic_store(const char *text, size_t length)
{
    struct console_vt_text *screen = console_active_vt >= 1u &&
        console_active_vt <= CONSOLE_VT_COUNT ? &vt_text[console_active_vt - 1u] : NULL;
    if (log_len > CONSOLE_LOG_CAP || log_start >= CONSOLE_LOG_CAP)
        log_len = log_start = 0;
    if (screen && screen->length > CONSOLE_VT_TEXT_CAP) screen->length = 0;
    for (size_t i = 0; i < length; ++i) {
        log_store(text[i]);
        if (!screen) continue;
        if (screen->length == CONSOLE_VT_TEXT_CAP) {
            const size_t drop = 4096u;
            for (size_t j = 0; j < CONSOLE_VT_TEXT_CAP - drop; ++j)
                screen->bytes[j] = screen->bytes[j + drop];
            screen->length -= drop;
        }
        screen->bytes[screen->length++] = text[i];
    }
}

/**
 * @brief Begin lock-free, allocation-free panic output on the active display.
 * @return The original display owner, retained for the diagnostic report.
 */
struct console_panic_context console_panic_begin(void)
{
    const struct framebuffer *active = framebuffer_get();
    /* arch_userland_init may have disabled a GOP buffer that cannot survive
     * the active identity map. Never undo that decision in a fatal path. */
    bool usable = fb_console_enabled && active->available && active->pixels &&
        (active->bytes_per_pixel == 3u || active->bytes_per_pixel == 4u) &&
        active->pitch >= (uint64_t)active->width * active->bytes_per_pixel;
    struct console_panic_context context = {
        .vt = console_active_vt, .graphical = console_vt_graphical,
    };
    if (console_vga_text_mode) {
        vga_console_panic_begin();
        console_vt_graphical=false;
        console_boot_logo_active=false;
        fb_console_enabled=false;
        serial_write_emergency("\n",1);
        return context;
    }
    fb_console_panic_overlay = context.graphical;
    fb_console_panic_clipped = false;
    bool reset = context.graphical || fb_console_render_target ||
        console_boot_logo_active || !fb_console_enabled ||
        fb_x || fb_y || !fb_cols || !fb_rows ||
        fb_cols != active->width / RELIEFOS_FONT_W ||
        fb_rows != active->height / RELIEFOS_FONT_H ||
        fb_col > fb_cols || fb_row >= fb_rows;
    fb_console_render_target = NULL;
    if (usable && !context.graphical) fb_console_hide_tty_cursor();
    console_boot_logo_active = false;
    console_vt_graphical = false;
    fb_console_tty_cursor_active = fb_console_tty_cursor_visible = false;
    fb_console_enabled = usable;
    if (usable) framebuffer_panic_prepare();
    fb_x = fb_y = 0;
    fb_cols = active->width / RELIEFOS_FONT_W;
    fb_rows = active->height / RELIEFOS_FONT_H;
    if (fb_cols > CONSOLE_MAX_COLS) fb_cols = CONSOLE_MAX_COLS;
    if (fb_rows > CONSOLE_MAX_ROWS) fb_rows = CONSOLE_MAX_ROWS;
    fb_console_bold = fb_console_underline = fb_console_reverse = false;
    fb_console_underline_color_set = false;
    fb_console_fg_color = 0x00ffffffu;
    fb_console_bg_color = 0;
    fb_scroll_top = 0;
    fb_scroll_bottom = fb_rows ? fb_rows - 1u : 0;
    fb_console_reset_ansi();
    console_utf8 = (struct reliefos_utf8_stream){0};
    if (reset) {
        fb_col = fb_row = 0;
        if (fb_console_enabled && !context.graphical)
            framebuffer_rect(0, 0, active->width, active->height, 0);
    }
    vga_console_enabled = !fb_console_enabled;
    if (vga_console_enabled) vga_panic_begin();
    serial_write_emergency("\n", 1);
    if (!context.graphical) console_panic_write("\n", 1, 0x00ffffffu);
    return context;
}

/**
 * @brief Render literal fatal text on black and mirror controlled ANSI colors.
 * @param text Bytes without terminal control sequences.
 * @param length Number of bytes to render.
 * @param foreground RGB foreground color.
 */
void console_panic_write(const char *text, size_t length, uint32_t foreground)
{
    const char *sgr = foreground == 0x00ff5555u ? "\033[0;40;91m" :
                      foreground == 0x00ffff55u ? "\033[0;40;93m" : "\033[0;40;97m";
    uint8_t vga_foreground = foreground == 0x00ff5555u ? 12u :
                             foreground == 0x00ffff55u ? 14u : 15u;
    if (!text || !length) return;
    console_panic_store(sgr, 10u);
    serial_write_emergency(sgr, 10u);
    console_panic_store(text, length);
    serial_write_emergency(text, length);
    fb_console_fg_color = foreground;
    fb_console_bg_color = 0;
    if (console_vga_text_mode) {
        vga_console_panic_write(text,length,vga_foreground);
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        if (fb_console_enabled) fb_console_putc(text[i]);
        else vga_panic_putc(text[i], vga_foreground);
    }
}

/** @brief Submit panic scanout without entering the ordinary display lock. */
void console_panic_flush(void)
{
    if (fb_console_enabled) framebuffer_panic_present();
}
