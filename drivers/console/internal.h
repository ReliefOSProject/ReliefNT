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
#include "vga_console.h"

#define CONSOLE_LOG_CAP 65536
#define CONSOLE_MAX_COLS 512u
#define CONSOLE_MAX_ROWS 192u
#define FB_ANSI_MAX_PARAMS 16u
#define FB_CONSOLE_CURSOR_HEIGHT 2u
#define FB_CONSOLE_CURSOR_BYTES (RELIEFOS_FONT_W * FB_CONSOLE_CURSOR_HEIGHT * 4u)
#define CONSOLE_VT_COUNT 6u
#define CONSOLE_VT_TEXT_CAP 65536u

struct fb_saved_rendition {
    uint32_t foreground, background, underline_color;
    bool bold, underline, reverse, conceal, underline_color_set, origin, wrap;
    uint8_t charset[2], charset_bank;
};

struct fb_terminal_modes {
    bool wrap, pending_wrap, origin, newline, conceal;
    bool tabs[CONSOLE_MAX_COLS];
    uint32_t last;
    struct fb_saved_rendition saved;
};

/* Pixel checkpoints retain the complete streaming parser across VT changes. */
struct fb_console_state {
    struct reliefos_utf8_stream utf8;
    uint32_t x;
    uint32_t y;
    uint32_t cols;
    uint32_t rows;
    uint32_t col;
    uint32_t row;
    uint32_t saved_col;
    uint32_t saved_row;
    uint32_t foreground;
    uint32_t background;
    bool bold;
    bool underline;
    bool reverse;
    bool insert_mode;
    struct fb_terminal_modes modes;
    uint8_t charset[2], charset_bank;
    uint32_t underline_color;
    bool underline_color_set;
    uint32_t scroll_top;
    uint32_t scroll_bottom;
    uint8_t ansi_state;
    uint32_t param_count;
    uint32_t param_value;
    bool param_active;
    bool private_mode;
    bool dec_private;
    bool overflow, string_osc;
    uint8_t prefix, intermediate;
    uint8_t charset_target;
    uint32_t params[FB_ANSI_MAX_PARAMS];
    bool param_colon[FB_ANSI_MAX_PARAMS];
};

struct console_vt_text {
    char bytes[CONSOLE_VT_TEXT_CAP];
    size_t length;
    bool application_cursor, cursor_hidden;
    bool alternate;
    uint8_t charset[2], charset_bank;
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
    FB_ANSI_IGNORE_ONE,
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
extern uint32_t fb_console_render_vt;
extern bool fb_console_replaying;
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
extern bool console_service_logs_only;
extern uint32_t fb_saved_col;
extern uint32_t fb_saved_row;
extern uint32_t fb_console_fg_color;
extern uint32_t fb_console_bg_color;
extern bool fb_console_bold;
extern bool fb_console_underline;
extern bool fb_console_reverse;
extern bool fb_console_insert_mode;
extern struct fb_terminal_modes fb_modes;
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

/**
 * @brief Move clipped text rows upward without changing padding or other pixels.
 * @par Context
 * Caller serializes console state and retains the rendering surface; exact
 * overlap-safe scalar-word copies with a byte tail require no allocation,
 * SIMD state or mapping changes, even for unaligned 24-bit row layouts.
 */
void fb_console_scroll(uint32_t lines);
/** @brief Scroll an inclusive row region in either direction without moving the cursor. */
void fb_console_scroll_region(uint32_t top, uint32_t bottom, uint32_t lines, bool reverse);
/** @brief Apply IND/RI, scrolling only when the cursor is on its corresponding margin. */
void fb_console_index(bool reverse, bool carriage);
/** @brief Move to a configured tab stop without changing existing cells. */
void fb_console_tab(uint32_t count, bool reverse);
/** @brief Repeat the last printable scalar with the normal wrap/insertion rules. */
void fb_console_repeat(uint32_t count);
/** @brief Save/restore the cursor, rendition, character set and DEC modes. */
void fb_console_save_cursor(void);
void fb_console_restore_cursor(void);
/** @brief Select a retained primary/alternate framebuffer page for one VT. */
void fb_console_alternate(bool on, bool clear, bool save_cursor);
/** @brief Discard retained alternate page contents during a terminal reset. */
void fb_console_reset_alternate(void);
void fb_console_mark_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
void fb_console_publish_damage(void);

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

/** @brief Insert or delete cells at the cursor, shifting only the current row.
 * @param count Number of cells, clipped to the remaining columns.
 * @param insert True to shift right and blank the inserted cells; false to
 * shift left and blank the vacated right margin. The cursor does not move.
 * Caller serializes console access and hides the software cursor first.
 */
void fb_console_shift_characters(uint32_t count, bool insert);

void fb_console_ansi_feed(char ch);

/**
 * @brief Save the serialized renderer and partial ANSI/UTF-8 parser.
 * @param state Required writable checkpoint storage owned by the caller.
 */
void fb_console_state_save(struct fb_console_state *state);

/**
 * @brief Restore a renderer checkpoint for its retained pixel surface.
 * @param state Required checkpoint saved for the matching surface layout.
 */
void fb_console_state_restore(const struct fb_console_state *state);
/** @brief Restore page rendering state while leaving the active byte parser intact. */
void fb_console_page_restore(const struct fb_console_state *state);
/** @brief Restore only the shared byte parser after switching terminal pages. */
void fb_console_stream_restore(const struct fb_console_state *state);


/** @brief Reset text state and clear the full active rendering surface. */
void fb_console_initialize_fullscreen(void);

void console_emit_display(const char *text, size_t length);

/** @brief Draw the cursor and publish an active text grid when not staging. */
void console_present(void);


void console_init(void);

void console_log_configure(const char *cmdline);

void console_set_ui_theme(uint32_t theme);



/**
 * @brief Append length-delimited input as INFO records in the kernel log.
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
