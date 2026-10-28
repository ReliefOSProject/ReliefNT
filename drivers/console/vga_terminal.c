#include "vga_terminal.h"
#include <reliefos/psf_font.h>

/** @brief Resolve an individual Unicode mapping from the immutable PSF1 table. */
uint8_t vga_terminal_glyph(uint32_t cp)
{
    if (cp >= 32 && cp < 127) return (uint8_t)cp;
    unsigned glyph = 0;
    bool sequence = false;
    for (unsigned i = 4 + 256 * 16; i + 1 < reliefos_lat15_vga16_psf_len; i += 2) {
        unsigned value = reliefos_lat15_vga16_psf[i] | (reliefos_lat15_vga16_psf[i+1] << 8);
        if (value == 0xffff) { ++glyph; sequence = false; }
        else if (value == 0xfffe) sequence = true;
        else if (!sequence && value == cp && glyph < 256) return (uint8_t)glyph;
    }
    return '?';
}

/** @brief Resolve current colors without interpreting unsupported font styles. */
uint8_t vga_terminal_attribute(const struct vga_page *p)
{
    unsigned fg = p->cursor.fg, bg = p->cursor.bg;
    if (p->cursor.reverse) { unsigned swap = fg; fg = bg; bg = swap; }
    if (p->cursor.conceal) fg = bg;
    return (uint8_t)(fg | (bg << 4));
}

/** @brief Clear a clipped range without changing the terminal's cursor. */
void vga_terminal_erase(struct vga_page *p, unsigned first, unsigned last)
{
    uint16_t cell = ' ' | ((uint16_t)vga_terminal_attribute(p) << 8);
    if (last > VGA_TEXT_CELLS) last = VGA_TEXT_CELLS;
    for (unsigned i = first; i < last; ++i) p->cells[i] = cell;
}

/** @brief Initialize page state and clear using conventional gray-on-black. */
void vga_terminal_page_reset(struct vga_page *p)
{
    __builtin_memset(p, 0, sizeof(*p));
    p->cursor.fg = 7;
    p->cursor.wrap = true;
    p->saved = p->cursor;
    p->bottom = VGA_TEXT_ROWS - 1;
    vga_terminal_erase(p, 0, VGA_TEXT_CELLS);
}

/** @brief Reset primary/alternate screens and independent stream-parser state. */
void vga_terminal_reset(struct vga_terminal *t)
{
    __builtin_memset(t, 0, sizeof(*t));
    vga_terminal_page_reset(&t->page[0]);
    vga_terminal_page_reset(&t->page[1]);
    t->visible = true;
    for (unsigned i = 8; i < VGA_TEXT_COLS; i += 8) t->tabs[i] = true;
}

/** @brief Shift only the requested rows; vacated cells use current colors. */
void vga_terminal_scroll(struct vga_page *p, unsigned top, unsigned bottom, int lines)
{
    if (top > bottom || bottom >= VGA_TEXT_ROWS || !lines) return;
    unsigned height = bottom - top + 1;
    unsigned amount = lines > 0 ? (unsigned)lines : (unsigned)-lines;
    if (amount > height) amount = height;
    unsigned keep = (height - amount) * VGA_TEXT_COLS;
    if (lines > 0) {
        __builtin_memmove(p->cells + top * VGA_TEXT_COLS,
            p->cells + (top + amount) * VGA_TEXT_COLS, keep * sizeof(uint16_t));
        vga_terminal_erase(p, (bottom + 1 - amount) * VGA_TEXT_COLS, (bottom + 1) * VGA_TEXT_COLS);
    } else {
        __builtin_memmove(p->cells + (top + amount) * VGA_TEXT_COLS,
            p->cells + top * VGA_TEXT_COLS, keep * sizeof(uint16_t));
        vga_terminal_erase(p, top * VGA_TEXT_COLS, (top + amount) * VGA_TEXT_COLS);
    }
}

/** @brief Implement IND/RI and explicit newline without crossing scroll margins. */
void vga_terminal_index(struct vga_page *p, bool reverse, bool carriage)
{
    if (reverse) {
        if (p->cursor.y == p->top) vga_terminal_scroll(p, p->top, p->bottom, -1);
        else if (p->cursor.y) --p->cursor.y;
    } else {
        if (p->cursor.y == p->bottom) vga_terminal_scroll(p, p->top, p->bottom, 1);
        else if (p->cursor.y < VGA_TEXT_ROWS - 1) ++p->cursor.y;
    }
    if (carriage) p->cursor.x = 0;
    p->pending_wrap = false;
}

/** @brief Map the VT100 special-graphics subset to Unicode font mappings. */
static uint32_t vga_dec_graphic(uint32_t cp)
{
    static const uint16_t table[] = {
        0x25c6,0x2592,0x2409,0x240c,0x240d,0x240a,0x00b0,0x00b1,
        0x2424,0x240b,0x2518,0x2510,0x250c,0x2514,0x253c,0x23ba,
        0x23bb,0x2500,0x23bc,0x23bd,0x251c,0x2524,0x2534,0x252c,
        0x2502,0x2264,0x2265,0x03c0,0x2260,0x00a3,0x00b7,
    };
    return cp >= 0x60 && cp <= 0x7e ? table[cp - 0x60] : cp;
}

/** @brief Place a printable scalar with delayed right-margin wrap and insertion. */
void vga_terminal_scalar(struct vga_terminal *t, uint32_t cp)
{
    struct vga_page *p = &t->page[t->alternate];
    if (p->pending_wrap && p->cursor.wrap) vga_terminal_index(p, false, true);
    if (p->cursor.charset[p->cursor.g1]) cp = vga_dec_graphic(cp);
    unsigned at = p->cursor.y * VGA_TEXT_COLS + p->cursor.x;
    if (p->insert && p->cursor.x + 1 < VGA_TEXT_COLS)
        __builtin_memmove(p->cells + at + 1, p->cells + at,
            (VGA_TEXT_COLS - p->cursor.x - 1) * sizeof(uint16_t));
    p->cells[at] = vga_terminal_glyph(cp) | ((uint16_t)vga_terminal_attribute(p) << 8);
    t->last = cp;
    if (p->cursor.x + 1 == VGA_TEXT_COLS) p->pending_wrap = p->cursor.wrap;
    else { ++p->cursor.x; p->pending_wrap = false; }
}

enum { NORMAL, ESCAPE, CSI, STRING, STRING_ESC, CHARSET, HASH };

/** @brief Incrementally consume a bounded escape stream and UTF-8 scalars. */
void vga_terminal_feed(struct vga_terminal *t, uint8_t b)
{
    struct vga_page *p = &t->page[t->alternate];
    if (b == 0x18 || b == 0x1a) { t->parser = NORMAL; t->utf8.remaining = 0; return; }
    if (t->parser == STRING || t->parser == STRING_ESC) {
        if ((t->osc && b == 7) || (t->parser == STRING_ESC && b == '\\')) t->parser = NORMAL;
        else t->parser = b == 27 ? STRING_ESC : STRING;
        return;
    }
    if (b == 27) { t->parser = ESCAPE; t->utf8.remaining = 0; return; }
    if (b < 32 || b == 127) {
        if (b == '\r') { p->cursor.x = 0; p->pending_wrap = false; }
        else if (b == '\n' || b == '\v' || b == '\f') vga_terminal_index(p, false, p->newline);
        else if (b == '\b') { if (p->cursor.x) --p->cursor.x; p->pending_wrap = false; }
        else if (b == '\t') {
            do { if (p->cursor.x == VGA_TEXT_COLS - 1) break; ++p->cursor.x; } while (!t->tabs[p->cursor.x]);
            p->pending_wrap = false;
        } else if (b == 14 || b == 15) p->cursor.g1 = b == 14;
        return;
    }
    if (t->parser == ESCAPE) {
        t->parser = NORMAL;
        if (b == '[') {
            t->parser = CSI; t->count = t->value = 0;
            t->digit = t->overflow = false; t->prefix = t->intermediate = 0;
        } else if (b == ']' || b == 'P' || b == '^' || b == '_' || b == 'X') {
            t->parser = STRING; t->osc = b == ']';
        } else if (b == '(' || b == ')') {
            t->parser = CHARSET; t->charset_target = b == ')';
        } else if (b == '#') t->parser = HASH;
        else if (b == '7') p->saved = p->cursor;
        else if (b == '8') { p->cursor = p->saved; p->pending_wrap = false; }
        else if (b == 'c') vga_terminal_reset(t);
        else if (b == 'D' || b == 'E' || b == 'M') vga_terminal_index(p, b == 'M', b == 'E');
        else if (b == 'H') t->tabs[p->cursor.x] = true;
        else if (b == 'Z') { t->reply_length = 5; __builtin_memcpy(t->reply,"\033[?6c",5); }
        return;
    }
    if (t->parser == CHARSET) {
        p->cursor.charset[t->charset_target] = b == '0'; t->parser = NORMAL; return;
    }
    if (t->parser == HASH) {
        if (b == '8') for (unsigned i = 0; i < VGA_TEXT_CELLS; ++i) p->cells[i] = 0x0745;
        t->parser = NORMAL; return;
    }
    if (t->parser == CSI) {
        if (b >= '0' && b <= '9') {
            unsigned next = t->value * 10 + b - '0';
            t->value = next > 65535 ? 65535 : next; t->digit = true;
        } else if (b == ';' || b == ':') {
            if (t->count < VGA_TEXT_PARAMS) {
                t->params[t->count] = t->value; t->colon[t->count++] = b == ':';
            } else t->overflow = true;
            t->value = 0; t->digit = false;
        } else if (b >= 0x3c && b <= 0x3f && !t->count && !t->digit && !t->intermediate) t->prefix = b;
        else if (b >= 0x20 && b <= 0x2f) t->intermediate = b;
        else if (b >= 0x40 && b <= 0x7e) {
            if (t->count < VGA_TEXT_PARAMS) {
                t->params[t->count] = t->value; t->colon[t->count++] = false;
            } else t->overflow = true;
            if (!t->overflow) vga_terminal_csi(t, b);
            t->parser = NORMAL;
        } else t->overflow = true;
        return;
    }
    uint32_t cp;
    int result = reliefos_utf8_feed(&t->utf8, b, &cp);
    if (result) vga_terminal_scalar(t, cp);
    if (result < 0 && reliefos_utf8_feed(&t->utf8, b, &cp)) vga_terminal_scalar(t, cp);
}
