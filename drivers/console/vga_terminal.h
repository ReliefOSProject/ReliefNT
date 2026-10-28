#ifndef RELIEFNT_VGA_TERMINAL_H
#define RELIEFNT_VGA_TERMINAL_H
#include <reliefnt/types.h>
#include <reliefos/utf8_stream.h>

#define VGA_TEXT_COLS 80u
#define VGA_TEXT_ROWS 25u
#define VGA_TEXT_CELLS (VGA_TEXT_COLS * VGA_TEXT_ROWS)
#define VGA_TEXT_PARAMS 32u

struct vga_cursor {
    unsigned x, y;
    uint8_t fg, bg, charset[2], g1;
    bool reverse, conceal, origin, wrap;
};
struct vga_page {
    uint16_t cells[VGA_TEXT_CELLS];
    struct vga_cursor cursor, saved;
    unsigned top, bottom;
    bool pending_wrap, insert, newline;
};
struct vga_terminal {
    struct vga_page page[2];
    struct reliefos_utf8_stream utf8;
    unsigned alternate, parser, count, value, cursor_style;
    uint32_t params[VGA_TEXT_PARAMS], last;
    uint8_t prefix, intermediate, charset_target;
    bool visible, reverse_screen, tabs[VGA_TEXT_COLS], colon[VGA_TEXT_PARAMS];
    bool application_cursor;
    bool digit, overflow, osc;
    char reply[64];
    unsigned reply_length;
};

/** @brief Reset both screens, parser, default 16-color attributes and tab stops.
 * @param t Caller-owned terminal; never allocates or performs I/O. */
void vga_terminal_reset(struct vga_terminal *t);
/** @brief Consume one output byte; unsupported strings are discarded through ST.
 * @param t Serialized caller-owned terminal. @param byte One ANSI/UTF-8 byte. */
void vga_terminal_feed(struct vga_terminal *t, uint8_t byte);
/** @brief Translate Unicode through the fixed PSF font; missing glyphs become '?'.
 * @param codepoint Unicode scalar. @return Loaded VGA glyph slot. */
uint8_t vga_terminal_glyph(uint32_t codepoint);
/** @brief Pack foreground/background, reverse and conceal into a VGA attribute.
 * @param p Active page. @return Attribute byte with blink disabled. */
uint8_t vga_terminal_attribute(const struct vga_page *p);
/** @brief Erase [first,last) cells using current attributes, clipping to screen.
 * @param p Page. @param first First cell. @param last Exclusive last cell. */
void vga_terminal_erase(struct vga_page *p, unsigned first, unsigned last);
/** @brief Scroll a bounded inclusive row region without moving the cursor.
 * @param p Page. @param top First row. @param bottom Last row.
 * @param lines Positive scrolls up, negative scrolls down; clipped to region. */
void vga_terminal_scroll(struct vga_page *p, unsigned top, unsigned bottom, int lines);
/** @brief Emit a scalar using charset mapping, insertion and delayed autowrap.
 * @param t Terminal. @param cp Unicode scalar, excluding C0 controls. */
void vga_terminal_scalar(struct vga_terminal *t, uint32_t cp);
/** @brief Move vertically at margins, optionally returning to column zero.
 * @param p Page. @param reverse True for reverse index. @param carriage CR too. */
void vga_terminal_index(struct vga_page *p, bool reverse, bool carriage);
/** @brief Execute a parsed CSI command; parameter storage has bounded length.
 * @param t Terminal. @param command Final CSI byte. */
void vga_terminal_csi(struct vga_terminal *t, uint8_t command);
/** @brief Set one active screen to initial state.
 * @param p Page to clear and reset. */
void vga_terminal_page_reset(struct vga_page *p);
#endif
