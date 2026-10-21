/*
 * ReliefOS kernel console interface: declares early and diagnostic output.
 * Provides formatted console logging used before and during userland startup.
 */
#ifndef RELIEFNT_CONSOLE_H
#define RELIEFNT_CONSOLE_H

#include <reliefnt/types.h>
#include <reliefos/kernel_log.h>

/**
 * @brief Set up the kernel console backend before any output is written.
 */
void console_init(void);
/**
 * @brief Write a single character to the active console backend.
 */
void console_putc(char ch);
/**
 * @brief Write a NUL-terminated string to the console.
 */
void console_write(const char *s);
/**
 * @brief Append raw diagnostics to the kernel log and serial sink.
 * @param s Bytes to write; need not be NUL-terminated.
 * @param len Number of bytes to write.
 */
void console_write_len(const char *s, size_t len);
/**
 * @brief Mirror system-console output to serial without adding kernel log records.
 * @param s Bytes already accepted by the console terminal; need not be NUL-terminated.
 * @param len Number of bytes to mirror.
 */
void console_serial_write_len(const char *s, size_t len);
/** @brief Record text emitted by a VT and paint it only while that VT is visible. */
void console_vt_write(uint32_t number, const char *s, size_t len);
/** @brief Present the active text VT or yield the framebuffer to graphics. */
void console_vt_activate(uint32_t number, bool graphical);
/** @brief Refresh the active graphical VT's framebuffer scanout. */
void console_display_tick(void);

/** Original display owner captured before a fatal console takeover. */
struct console_panic_context {
    uint32_t vt;
    bool graphical;
};
/**
 * @brief Take over the current display without allocation or ordinary locks.
 * The panic owner must mask interrupts and stop other CPUs before calling.
 * @return Original VT number (zero during boot) and graphical ownership.
 */
struct console_panic_context console_panic_begin(void);
/**
 * @brief Write literal panic text on black, recording it on the current VT.
 * @param text Bytes with no terminal control sequences.
 * @param length Number of bytes to consume.
 * @param foreground Logical RGB foreground; red/yellow/white select ANSI colors.
 */
void console_panic_write(const char *text, size_t length, uint32_t foreground);
/** @brief Publish panic pixels with bounded hardware polling and no locks. */
void console_panic_flush(void);
/**
 * @brief Write directly to COM1 without registered drivers or console locks.
 * @param text Bytes to send; newline is expanded to CR/LF.
 * @param length Number of bytes; polling per character is bounded.
 */
void serial_write_emergency(const char *text, size_t length);
/** @brief Select black VGA text attributes, preserving a valid text cursor. */
void vga_panic_begin(void);
/**
 * @brief Write one literal VGA panic byte with a black background.
 * @param ch ASCII byte to render.
 * @param foreground VGA foreground index, low four bits only.
 */
void vga_panic_putc(char ch, uint8_t foreground);
/**
 * @brief Format INFO-level kernel diagnostics; raw output uses console_write.
 */
void console_printf(const char *fmt, ...);

void console_log_configure(const char *cmdline);
void console_log(enum console_log_level level, const char *fmt, ...);
void console_log_write(const char *text);

#define console_debug(...) console_log(CONSOLE_LOG_DEBUG, __VA_ARGS__)
#define console_info(...) console_log(CONSOLE_LOG_INFO, __VA_ARGS__)
#define console_warn(...) console_log(CONSOLE_LOG_WARN, __VA_ARGS__)
#define console_error(...) console_log(CONSOLE_LOG_ERROR, __VA_ARGS__)
#define console_fatal(...) console_log(CONSOLE_LOG_FATAL, __VA_ARGS__)
/**
 * @brief Route console output to the framebuffer, replaying the boot log.
 */
void console_enable_framebuffer(void);
/**
 * @brief Turn off framebuffer output and fall back to another backend.
 */
void console_disable_framebuffer(void);
/**
 * @brief Hide boot logos and restore the full text area before userland starts.
 * Called once by the BSP after kernel initialization, with interrupts disabled.
 */
void console_boot_complete(void);
/**
 * @brief Route console output to the legacy VGA text buffer.
 */
void console_enable_vga_fallback(void);
/**
 * @brief Select the UI color theme used by the framebuffer console.
 */
void console_set_ui_theme(uint32_t theme);
/**
 * @brief Initialize the COM1 serial port for early diagnostic output.
 */
void serial_init(void);
/**
 * @brief Return non-zero when the serial backend is available for output.
 */
int serial_is_ready(void);
/**
 * @brief Write a string to the serial backend (or the early COM1 fallback).
 */
void serial_write(const char *s);
/**
 * @brief Initialize the legacy VGA text-mode console.
 */
void vga_init(void);
/**
 * @brief Write one character to the VGA text buffer, handling newlines and scrolling.
 */
void vga_putc(char ch);
/**
 * @brief Write a string into the VGA text buffer at column x, row y.
 */
void vga_write_at(uint8_t x, uint8_t y, const char *s);

#endif
