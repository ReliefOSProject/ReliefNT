#ifndef RELIEFNT_DRIVERS_CONSOLE_CONSOLE_C_INTERNAL_H
#define RELIEFNT_DRIVERS_CONSOLE_CONSOLE_C_INTERNAL_H

#include <reliefos/psf_font.h>
#include <reliefos/utf8_stream.h>


#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/boot_logo.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/smp.h>
#include <reliefnt/time.h>

#define CONSOLE_LOG_CAP 65536
#define CONSOLE_MAX_COLS 512u
#define CONSOLE_MAX_ROWS 192u
#define FB_ANSI_MAX_PARAMS 16u
#define FB_CONSOLE_CURSOR_BYTES (RELIEFOS_FONT_W * RELIEFOS_FONT_H * 4u)
#define CONSOLE_VT_COUNT 6u
#define CONSOLE_VT_TEXT_CAP 65536u

struct console_vt_text {
    char bytes[CONSOLE_VT_TEXT_CAP];
    size_t length;
};






























































/* Match the desktop terminal's readable ANSI palette while retaining a
 * black ANSI background for conventional TTY output. */


enum fb_ansi_state {
    FB_ANSI_NORMAL = 0,
    FB_ANSI_ESC,
    FB_ANSI_CSI,
    FB_ANSI_OSC,
    FB_ANSI_OSC_ESC,
    FB_ANSI_CHARSET,
};













































enum fb_ansi_color_target {
    FB_ANSI_COLOR_FOREGROUND,
    FB_ANSI_COLOR_BACKGROUND,
    FB_ANSI_COLOR_UNDERLINE,
};


extern struct reliefos_utf8_stream console_utf8;
extern struct console_vt_text vt_text[CONSOLE_VT_COUNT];
extern uint32_t console_active_vt;
extern bool console_vt_graphical;
extern const struct framebuffer *fb_console_render_target;
extern char log_buffer[CONSOLE_LOG_CAP];
extern size_t log_len;
extern size_t log_start;
extern bool fb_console_enabled;
extern bool fb_console_panic_overlay;
extern bool fb_console_panic_clipped;
extern bool console_boot_logo_active;
extern bool vga_console_enabled;
extern uint32_t fb_x;
extern uint32_t fb_y;
extern uint32_t fb_cols;
extern uint32_t fb_rows;
extern uint32_t fb_col;
extern uint32_t fb_row;
extern uint32_t console_ui_theme;
extern bool console_line_start;
extern bool console_service_logs_only;
extern uint32_t fb_saved_col;
extern uint32_t fb_saved_row;
extern uint32_t fb_console_fg_color;
extern uint32_t fb_console_bg_color;
extern bool fb_console_bold;
extern bool fb_console_underline;
extern bool fb_console_reverse;
extern uint32_t fb_console_underline_color;
extern bool fb_console_underline_color_set;
extern uint32_t fb_scroll_top;
extern uint32_t fb_scroll_bottom;
extern bool fb_console_tty_cursor_active;
extern bool fb_console_tty_cursor_visible;
extern uint8_t fb_ansi_state;
extern uint32_t fb_ansi_param_count;
extern uint32_t fb_ansi_param_value;
extern bool fb_ansi_param_active;
extern bool fb_ansi_private;

/**
 * @brief Return the framebuffer currently used by the text renderer.
 * @return The temporary replay surface, or the active framebuffer.
 */
const struct framebuffer *fb_console_framebuffer(void);

uint32_t console_panel(void);

uint32_t console_fg(void);

void fb_console_reset_attributes(void);

void fb_console_reset_ansi(void);

void log_store(char ch);

size_t console_log_replay_start(void);

/** @brief Clear the text grid on the current rendering surface. */
void fb_console_clear_log(void);

/** @brief Restore the saved pixels beneath the visible text cursor. */
void fb_console_hide_tty_cursor(void);

/** @brief Save the underlying pixels and draw the text cursor. */
void fb_console_show_tty_cursor(void);

/** @brief Scroll the active text region on the current rendering surface. */
void fb_console_scroll(void);

/** @brief Advance the text cursor; graphical panic overlays clip instead of scrolling. */
void fb_console_newline(void);

/** @brief Decode one output byte, retaining incomplete UTF-8 across writes. */
void fb_console_putc(char ch);

/**
 * @brief Clear a half-open cell range in one text row.
 * @param row Text row to clear.
 * @param first First cell in the range.
 * @param last End cell, not included.
 */
void fb_console_clear_row_range(uint32_t row, uint32_t first, uint32_t last);

void fb_console_ansi_feed(char ch);

/** @brief Reset text state and clear the full active rendering surface. */
void fb_console_initialize_fullscreen(void);

void console_emit_raw(char ch);

/** @brief Draw the cursor and publish an active text grid when not staging. */
void console_present(void);

/**
 * @brief Refresh the active graphical VT's framebuffer scanout.
 *
 * The VMware legacy FIFO does not observe direct mmap writes from Xorg, so a
 * periodic full-screen UPDATE is required while a graphical VT is active.
 * This path is independent of the text-console renderer: Xorg owns the
 * framebuffer while the active VT is graphical, even if text rendering has
 * been disabled for that hand-off.
 */
void console_display_tick(void);

void console_emit_timestamp(void);

void console_init(void);

void console_log_configure(const char *cmdline);

void console_set_ui_theme(uint32_t theme);

void console_putc(char ch);

void console_write(const char *s);

/**
 * @brief Append raw diagnostic output to the kernel log and serial sink.
 * @param s Bytes to write; need not be NUL-terminated.
 * @param len Number of bytes to write.
 */
void console_write_len(const char *s, size_t len);

/**
 * @brief Mirror system-console bytes to serial without creating kernel log records.
 * @param s Bytes already accepted by the console terminal.
 * @param len Number of bytes to mirror; s need not be NUL-terminated.
 */
void console_serial_write_len(const char *s, size_t len);

/**
 * @brief Append terminal output to its own VT history and paint only the active text VT.
 * @param number Virtual terminal from one through six.
 * @param s Bytes to append; need not be NUL terminated.
 * @param len Number of bytes to append.
 */
void console_vt_write(uint32_t number, const char *s, size_t len);

/**
 * @brief Select the framebuffer owner and restore a VT's text state when visible.
 * @param number Virtual terminal from one through six.
 * @param graphical True if the VT's graphical session owns the display.
 */
void console_vt_activate(uint32_t number, bool graphical);

/**
 * @brief Begin lock-free, allocation-free panic output on the active display.
 * @return The original display owner, retained for the diagnostic report.
 */
struct console_panic_context console_panic_begin(void);

/**
 * @brief Render literal fatal text on black and mirror controlled ANSI colors.
 * @param text Bytes without terminal control sequences.
 * @param length Number of bytes to render.
 * @param foreground RGB foreground color.
 */
void console_panic_write(const char *text, size_t length, uint32_t foreground);

/** @brief Submit panic scanout without entering the ordinary display lock. */
void console_panic_flush(void);

void console_vlog(enum console_log_level level, const char *fmt, va_list arguments);

void console_log(enum console_log_level level, const char *fmt, ...);

void console_printf(const char *fmt, ...);

void console_log_write(const char *text);

void console_enable_framebuffer(void);

void console_disable_framebuffer(void);

/**
 * @brief Remove boot logos and replay diagnostics using the full text screen.
 * Called by the BSP after initialization and before the first user task runs.
 * Subsequent console re-enables and VT switches cannot reintroduce the logos.
 */
void console_boot_complete(void);

void console_enable_vga_fallback(void);

#endif
