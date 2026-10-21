#include "internal.h"

static void vga_console_ansi_feed(char character);

struct reliefos_utf8_stream console_utf8;

struct console_vt_text vt_text[CONSOLE_VT_COUNT];

uint32_t console_active_vt;

bool console_vt_graphical;

const struct framebuffer *fb_console_render_target;

char log_buffer[CONSOLE_LOG_CAP];

size_t log_len;

size_t log_start;

bool fb_console_enabled;

bool fb_console_panic_overlay;

bool fb_console_panic_clipped;

bool console_boot_logo_active = true;

bool vga_console_enabled;

uint32_t fb_x;

uint32_t fb_y;

uint32_t fb_cols;

uint32_t fb_rows;

uint32_t fb_col;

uint32_t fb_row;

uint32_t console_ui_theme = 1u;

bool console_line_start = true;

bool console_service_logs_only;

static bool console_presenting;
static uint8_t vga_ansi_state;
uint32_t fb_saved_col;

uint32_t fb_saved_row;

uint32_t fb_console_fg_color;

uint32_t fb_console_bg_color;

bool fb_console_bold;

bool fb_console_underline;

bool fb_console_reverse;

uint32_t fb_console_underline_color;

bool fb_console_underline_color_set;

uint32_t fb_scroll_top;

uint32_t fb_scroll_bottom;

bool fb_console_tty_cursor_active;

bool fb_console_tty_cursor_visible;

uint8_t fb_ansi_state;

uint32_t fb_ansi_param_count;

uint32_t fb_ansi_param_value;

bool fb_ansi_param_active;

bool fb_ansi_private;

static void vga_console_ansi_feed(char character)
{
    if (vga_ansi_state == 0u) {
        if (character == '\033') vga_ansi_state = 1u;
        else vga_putc(character);
    } else if (vga_ansi_state == 1u) {
        vga_ansi_state = character == '[' ? 2u : 0u;
    } else if ((unsigned char)character >= 0x40u &&
               (unsigned char)character <= 0x7eu) {
        vga_ansi_state = 0u;
    }
}

void console_emit_raw(char ch)
{
    char s[2] = {ch, 0};
    if (!console_service_logs_only) fb_console_hide_tty_cursor();
    log_store(ch);
    serial_write(s);
    if (vga_console_enabled && !console_service_logs_only) {
        vga_putc(ch);
    }
    if (!console_service_logs_only) {
        fb_console_putc(ch);
    }
    console_line_start = ch == '\n';
}

/** @brief Draw the cursor and publish an active text grid when not staging. */
void console_present(void)
{
    const struct framebuffer *fb = fb_console_framebuffer();
    if (console_presenting || console_vt_graphical || !fb_console_enabled ||
        !fb->available) {
        return;
    }
    console_presenting = true;
    fb_console_show_tty_cursor();
    if (fb_cols && fb_rows && !fb_console_render_target) {
        framebuffer_present_region(fb_x, fb_y,
                                   fb_cols * RELIEFOS_FONT_W,
                                   fb_rows * RELIEFOS_FONT_H);
    }
    console_presenting = false;
}

/**
 * @brief Refresh the active graphical VT's framebuffer scanout.
 *
 * The VMware legacy FIFO does not observe direct mmap writes from Xorg, so a
 * periodic full-screen UPDATE is required while a graphical VT is active.
 * This path is independent of the text-console renderer: Xorg owns the
 * framebuffer while the active VT is graphical, even if text rendering has
 * been disabled for that hand-off.
 */
void console_display_tick(void)
{
    if (!console_vt_graphical || !framebuffer_get()->available) {
        return;
    }
    framebuffer_display_tick();
}

void console_init(void)
{
    serial_init();
}

void console_set_ui_theme(uint32_t theme)
{
    console_ui_theme = theme == 0u ? 0u : 1u;
}

void console_putc(char ch)
{
    if (console_line_start) {
        console_emit_timestamp();
    }
    console_emit_raw(ch);
}

void console_write(const char *s)
{
    while (s && *s) {
        console_putc(*s++);
    }
}

/**
 * @brief Append raw diagnostic output to the kernel log and serial sink.
 * @param s Bytes to write; need not be NUL-terminated.
 * @param len Number of bytes to write.
 */
void console_write_len(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        console_putc(s[i]);
    }
}

/**
 * @brief Mirror system-console bytes to serial without creating kernel log records.
 * @param s Bytes already accepted by the console terminal.
 * @param len Number of bytes to mirror; s need not be NUL-terminated.
 */
void console_serial_write_len(const char *s, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char byte[2] = {s[i], 0};
        serial_write(byte);
    }
}

void console_printf(const char *fmt, ...)
{
    va_list arguments;
    va_start(arguments, fmt);
    console_vlog(CONSOLE_LOG_INFO, fmt, arguments);
    va_end(arguments);
}

void console_enable_framebuffer(void)
{
    if (!framebuffer_get()->available) {
        return;
    }
    fb_console_enabled = true;
    fb_console_initialize_fullscreen();
    fb_console_clear_log();
    for (size_t i = console_log_replay_start(); i < log_len; ++i) {
        fb_console_ansi_feed(log_buffer[(log_start + i) % CONSOLE_LOG_CAP]);
    }
    console_present();
}

void console_disable_framebuffer(void)
{
    fb_console_hide_tty_cursor();
    fb_console_tty_cursor_active = false;
    fb_console_enabled = false;
}

/**
 * @brief Remove boot logos and replay diagnostics using the full text screen.
 * Called by the BSP after initialization and before the first user task runs.
 * Subsequent console re-enables and VT switches cannot reintroduce the logos.
 */
void console_boot_complete(void)
{
    if (!console_boot_logo_active) return;
    console_boot_logo_active = false;
    if (fb_console_enabled && !console_vt_graphical) {
        fb_console_hide_tty_cursor();
        console_enable_framebuffer();
    }
}

void console_enable_vga_fallback(void)
{
    if (framebuffer_get()->available) {
        return;
    }
    vga_init();
    vga_console_enabled = true;
    vga_ansi_state = 0u;
    for (size_t i = console_log_replay_start(); i < log_len; ++i) {
        vga_console_ansi_feed(log_buffer[(log_start + i) % CONSOLE_LOG_CAP]);
    }
}
