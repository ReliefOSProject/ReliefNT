#include "internal.h"
#include <reliefnt/serial.h>

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
bool console_vga_text_mode;

uint32_t fb_x;

uint32_t fb_y;

uint32_t fb_cols;

uint32_t fb_rows;

uint32_t fb_col;

uint32_t fb_row;

uint32_t console_ui_theme = 1u;

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

bool fb_console_insert_mode;
struct fb_terminal_modes fb_modes;

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

/** @brief Render one encoded record while the boot console owns the display. */
void console_emit_display(const char *text, size_t length)
{
    if (console_service_logs_only) return;
    if (!console_vga_text_mode) fb_console_hide_tty_cursor();
    for (size_t i = 0; i < length; ++i) {
        char ch = text[i];
        if (console_vga_text_mode) vga_console_boot_byte(ch);
        else {
            if (vga_console_enabled) vga_console_ansi_feed(ch);
            if (ch == '\n') fb_console_ansi_feed('\r');
            fb_console_ansi_feed(ch);
        }
    }
    console_present();
}

/** @brief Draw the cursor and publish an active text grid when not staging. */
void console_present(void)
{
    if (console_vga_text_mode) { vga_console_present(); return; }
    const struct framebuffer *fb = fb_console_framebuffer();
    if (console_presenting || console_vt_graphical || !fb_console_enabled ||
        !fb->available) {
        return;
    }
    console_presenting = true;
    fb_console_show_tty_cursor();
    fb_console_publish_damage();
    console_presenting = false;
}

void console_init(void)
{
    serial_init();
}

void console_set_ui_theme(uint32_t theme)
{
    console_ui_theme = theme == 0u ? 0u : 1u;
}

void console_serial_write_len(const char *text, size_t length)
{
    if (!text) return;
    char data[512];
    /* Do not retain user pointers or run their faults in the TX lock. */
    while (length) {
        size_t chunk = length < sizeof(data) ? length : sizeof(data);
        __builtin_memcpy(data, text, chunk);
        serial_tx_record(data, chunk, false);
        length -= chunk;
        text += chunk;
    }
}

void console_enable_framebuffer(void)
{
    if (console_vga_text_mode) return;
    if (!framebuffer_get()->available) {
        return;
    }
    fb_console_enabled = true;
    fb_console_initialize_fullscreen();
    fb_console_clear_log();
    for (size_t i = console_log_replay_start(); i < log_len; ++i) {
        char ch = log_buffer[(log_start + i) % CONSOLE_LOG_CAP];
        if (ch == '\n') fb_console_ansi_feed('\r');
        fb_console_ansi_feed(ch);
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
    if (console_vga_text_mode) return;
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
