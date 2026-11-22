#include "internal.h"
#include <reliefnt/pty.h>

static uint32_t fb_ansi_param(uint32_t index, uint32_t fallback);

static uint32_t fb_ansi_color(uint32_t code);

static uint32_t fb_ansi_palette(uint32_t index);

static void fb_console_set_ansi_color(enum fb_ansi_color_target target, uint32_t color);

static bool fb_console_apply_extended_color(uint32_t index,
                                            enum fb_ansi_color_target target,
                                            uint32_t *last_index);

static void fb_console_execute_csi(char final);

static const uint32_t fb_ansi_colors[16] = {
    0x00000000u, 0x00d06060u, 0x006fc77au, 0x00d5b85au,
    0x007f9ee8u, 0x00c38bd7u, 0x006bc8d8u, 0x00d7e3f4u,
    0x00708090u, 0x00ff8484u, 0x008de39au, 0x00f0d37bu,
    0x009bb7ffu, 0x00e8a5ffu, 0x0088e8fau, 0x00ffffffu,
};
static uint32_t fb_ansi_params[FB_ANSI_MAX_PARAMS];
static bool fb_ansi_param_colon[FB_ANSI_MAX_PARAMS];
static bool fb_ansi_dec_private;
static uint8_t fb_ansi_charset_target;
static bool fb_ansi_overflow, fb_ansi_string_osc;
static uint8_t fb_ansi_prefix, fb_ansi_intermediate;

/**
 * @brief Save the serialized renderer and partial ANSI/UTF-8 parser.
 * @param state Required writable checkpoint storage owned by the caller.
 */
void fb_console_state_save(struct fb_console_state *state)
{
    state->utf8 = console_utf8;
    state->x = fb_x;
    state->y = fb_y;
    state->cols = fb_cols;
    state->rows = fb_rows;
    state->col = fb_col;
    state->row = fb_row;
    state->saved_col = fb_saved_col;
    state->saved_row = fb_saved_row;
    state->foreground = fb_console_fg_color;
    state->background = fb_console_bg_color;
    state->bold = fb_console_bold;
    state->underline = fb_console_underline;
    state->reverse = fb_console_reverse;
    state->insert_mode = fb_console_insert_mode;
    state->modes = fb_modes;
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    state->charset[0] = state->charset[1] = state->charset_bank = 0;
    if (vt && vt <= CONSOLE_VT_COUNT) {
        const struct console_vt_text *t = &vt_text[vt - 1];
        __builtin_memcpy(state->charset, t->charset, sizeof(t->charset));
        state->charset_bank = t->charset_bank;
    }
    state->underline_color = fb_console_underline_color;
    state->underline_color_set = fb_console_underline_color_set;
    state->scroll_top = fb_scroll_top;
    state->scroll_bottom = fb_scroll_bottom;
    state->ansi_state = fb_ansi_state;
    state->param_count = fb_ansi_param_count;
    state->param_value = fb_ansi_param_value;
    state->param_active = fb_ansi_param_active;
    state->private_mode = fb_ansi_private;
    state->dec_private = fb_ansi_dec_private;
    state->overflow = fb_ansi_overflow;
    state->string_osc = fb_ansi_string_osc;
    state->prefix = fb_ansi_prefix;
    state->intermediate = fb_ansi_intermediate;
    state->charset_target = fb_ansi_charset_target;
    __builtin_memcpy(state->params, fb_ansi_params, sizeof(state->params));
    __builtin_memcpy(state->param_colon, fb_ansi_param_colon, sizeof(state->param_colon));
}

/**
 * @brief Restore a renderer checkpoint for its retained pixel surface.
 * @param state Required checkpoint saved for the matching surface layout.
 */
void fb_console_page_restore(const struct fb_console_state *state)
{
    fb_x = state->x;
    fb_y = state->y;
    fb_cols = state->cols;
    fb_rows = state->rows;
    fb_col = state->col;
    fb_row = state->row;
    fb_saved_col = state->saved_col;
    fb_saved_row = state->saved_row;
    fb_console_fg_color = state->foreground;
    fb_console_bg_color = state->background;
    fb_console_bold = state->bold;
    fb_console_underline = state->underline;
    fb_console_reverse = state->reverse;
    fb_console_insert_mode = state->insert_mode;
    fb_modes = state->modes;
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (vt && vt <= CONSOLE_VT_COUNT) {
        struct console_vt_text *t = &vt_text[vt - 1];
        __builtin_memcpy(t->charset, state->charset, sizeof(t->charset));
        t->charset_bank = state->charset_bank;
    }
    fb_console_underline_color = state->underline_color;
    fb_console_underline_color_set = state->underline_color_set;
    fb_scroll_top = state->scroll_top;
    fb_scroll_bottom = state->scroll_bottom;
}

void fb_console_stream_restore(const struct fb_console_state *state)
{
    console_utf8 = state->utf8;
    fb_ansi_state = state->ansi_state;
    fb_ansi_param_count = state->param_count;
    fb_ansi_param_value = state->param_value;
    fb_ansi_param_active = state->param_active;
    fb_ansi_private = state->private_mode;
    fb_ansi_dec_private = state->dec_private;
    fb_ansi_overflow = state->overflow;
    fb_ansi_string_osc = state->string_osc;
    fb_ansi_prefix = state->prefix;
    fb_ansi_intermediate = state->intermediate;
    fb_ansi_charset_target = state->charset_target;
    __builtin_memcpy(fb_ansi_params, state->params, sizeof(state->params));
    __builtin_memcpy(fb_ansi_param_colon, state->param_colon, sizeof(state->param_colon));
}

void fb_console_state_restore(const struct fb_console_state *state)
{
    fb_console_page_restore(state);
    fb_console_stream_restore(state);
}

void fb_console_save_cursor(void)
{
    fb_saved_col = fb_col;
    fb_saved_row = fb_row;
    fb_modes.saved = (struct fb_saved_rendition){
        .foreground = fb_console_fg_color, .background = fb_console_bg_color,
        .underline_color = fb_console_underline_color,
        .bold = fb_console_bold, .underline = fb_console_underline,
        .reverse = fb_console_reverse, .conceal = fb_modes.conceal,
        .underline_color_set = fb_console_underline_color_set,
        .origin = fb_modes.origin, .wrap = fb_modes.wrap,
    };
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (vt && vt <= CONSOLE_VT_COUNT) {
        const struct console_vt_text *t = &vt_text[vt - 1];
        __builtin_memcpy(fb_modes.saved.charset, t->charset, sizeof(t->charset));
        fb_modes.saved.charset_bank = t->charset_bank;
    }
}

void fb_console_restore_cursor(void)
{
    fb_col = fb_saved_col < fb_cols ? fb_saved_col : (fb_cols ? fb_cols - 1 : 0);
    fb_row = fb_saved_row < fb_rows ? fb_saved_row : (fb_rows ? fb_rows - 1 : 0);
    const struct fb_saved_rendition *s = &fb_modes.saved;
    fb_console_fg_color = s->foreground;
    fb_console_bg_color = s->background;
    fb_console_underline_color = s->underline_color;
    fb_console_bold = s->bold;
    fb_console_underline = s->underline;
    fb_console_reverse = s->reverse;
    fb_modes.conceal = s->conceal;
    fb_console_underline_color_set = s->underline_color_set;
    fb_modes.origin = s->origin;
    fb_modes.wrap = s->wrap;
    if (fb_modes.origin) {
        if (fb_row < fb_scroll_top) fb_row = fb_scroll_top;
        if (fb_row > fb_scroll_bottom) fb_row = fb_scroll_bottom;
    }
    fb_modes.pending_wrap = false;
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (vt && vt <= CONSOLE_VT_COUNT) {
        struct console_vt_text *t = &vt_text[vt - 1];
        __builtin_memcpy(t->charset, s->charset, sizeof(t->charset));
        t->charset_bank = s->charset_bank;
    }
}

/** @brief Deliver bounded DSR/DA replies to the addressed terminal without echo. */
static void fb_ansi_reply(const char *text)
{
    if (fb_console_replaying) return;
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    size_t length = 0;
    while (text[length]) ++length;
    if (vt && vt <= CONSOLE_VT_COUNT) pty_vt_reply(vt, text, length);
}

static char *fb_ansi_decimal(char *text, uint32_t n)
{
    char digits[10];
    unsigned count = 0;
    do { digits[count++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (count) *text++ = digits[--count];
    return text;
}

static void fb_ansi_mode(uint32_t mode, bool on)
{
    uint32_t vt = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (!fb_ansi_private) {
        if (mode == 4) fb_console_insert_mode = on;
        if (mode == 20) fb_modes.newline = on;
        return;
    }
    if (!fb_ansi_dec_private) return;
    struct console_vt_text *t = vt && vt <= CONSOLE_VT_COUNT ? &vt_text[vt - 1] : NULL;
    if (mode == 1 && t) t->application_cursor = on;
    if (mode == 25 && t) t->cursor_hidden = !on;
    if (mode == 6) {
        fb_modes.origin = on;
        fb_row = on ? fb_scroll_top : 0;
        fb_col = 0;
        fb_modes.pending_wrap = false;
    }
    if (mode == 7) { fb_modes.wrap = on; fb_modes.pending_wrap = false; }
    if (mode == 1048) {
        if (on) fb_console_save_cursor();
        else fb_console_restore_cursor();
    }
    if (mode == 47 || mode == 1047 || mode == 1049)
        fb_console_alternate(on, mode != 47, mode == 1049);
}

static uint32_t fb_ansi_param(uint32_t index, uint32_t fallback)
{
    if (index >= fb_ansi_param_count) {
        return fallback;
    }
    return fb_ansi_params[index];
}

static uint32_t fb_ansi_color(uint32_t code)
{
    if (code >= 90u && code <= 97u) {
        return fb_ansi_colors[code - 90u + 8u];
    }
    if (code >= 30u && code <= 37u) {
        return fb_ansi_colors[code - 30u];
    }
    return console_fg();
}

static uint32_t fb_ansi_palette(uint32_t index)
{
    if (index < 16u) return fb_ansi_colors[index];
    if (index >= 232u) {
        uint32_t level = 8u + (index - 232u) * 10u;
        return (level << 16) | (level << 8) | level;
    }
    index -= 16u;
    {
        uint32_t r = index / 36u;
        uint32_t g = (index / 6u) % 6u;
        uint32_t b = index % 6u;
        r = r ? 55u + r * 40u : 0u;
        g = g ? 55u + g * 40u : 0u;
        b = b ? 55u + b * 40u : 0u;
        return (r << 16) | (g << 8) | b;
    }
}

static void fb_console_set_ansi_color(enum fb_ansi_color_target target, uint32_t color)
{
    if (target == FB_ANSI_COLOR_FOREGROUND) {
        fb_console_fg_color = color;
    } else if (target == FB_ANSI_COLOR_BACKGROUND) {
        fb_console_bg_color = color;
    } else {
        fb_console_underline_color = color;
        fb_console_underline_color_set = true;
    }
}

static bool fb_console_apply_extended_color(uint32_t index,
                                            enum fb_ansi_color_target target,
                                            uint32_t *last_index)
{
    uint32_t mode_index = index + 1u;
    uint32_t color;

    if (mode_index >= fb_ansi_param_count) {
        return false;
    }
    if (fb_ansi_params[mode_index] == 5u) {
        if (mode_index + 1u >= fb_ansi_param_count) {
            return false;
        }
        color = fb_ansi_palette(fb_ansi_params[mode_index + 1u] > 255u
                                    ? 255u : fb_ansi_params[mode_index + 1u]);
        *last_index = mode_index + 1u;
    } else if (fb_ansi_params[mode_index] == 2u) {
        uint32_t rgb_index = mode_index + 1u;
        if (fb_ansi_param_colon[index]) {
            /* ISO 8613-6 form: 38:2:<color-space>:red:green:blue. */
            if (mode_index + 4u >= fb_ansi_param_count) {
                return false;
            }
            rgb_index = mode_index + 2u;
            *last_index = mode_index + 4u;
        } else {
            if (mode_index + 3u >= fb_ansi_param_count) {
                return false;
            }
            *last_index = mode_index + 3u;
        }
        color = ((fb_ansi_params[rgb_index] > 255u ? 255u : fb_ansi_params[rgb_index]) << 16) |
                ((fb_ansi_params[rgb_index + 1u] > 255u ? 255u : fb_ansi_params[rgb_index + 1u]) << 8) |
                (fb_ansi_params[rgb_index + 2u] > 255u ? 255u : fb_ansi_params[rgb_index + 2u]);
    } else {
        return false;
    }
    fb_console_set_ansi_color(target, color);
    return true;
}

static void fb_console_execute_csi(char final)
{
    uint32_t n = fb_ansi_param_count;
    if (!fb_rows || !fb_cols || fb_ansi_intermediate) return;
    if (final == 'h' || final == 'l') {
        for (uint32_t i = 0; i < n; ++i) fb_ansi_mode(fb_ansi_params[i], final == 'h');
        return;
    }
    if (final == 'n' && (!fb_ansi_prefix || fb_ansi_dec_private)) {
        if (fb_ansi_param(0, 0) == 5 && !fb_ansi_private) fb_ansi_reply("\033[0n");
        if (fb_ansi_param(0, 0) == 6) {
            char response[32], *p = response;
            *p++ = 27; *p++ = '[';
            if (fb_ansi_dec_private) *p++ = '?';
            p = fb_ansi_decimal(p, fb_row - (fb_modes.origin ? fb_scroll_top : 0) + 1);
            *p++ = ';'; p = fb_ansi_decimal(p, fb_col + 1);
            *p++ = 'R'; *p = 0;
            fb_ansi_reply(response);
        }
        return;
    }
    if (final == 'c') {
        if (!fb_ansi_param(0, 0)) {
            if (!fb_ansi_prefix) fb_ansi_reply("\033[?6c");
            if (fb_ansi_prefix == '>') fb_ansi_reply("\033[>0;1;0c");
        }
        return;
    }
    if (fb_ansi_private) return;
    uint32_t p0 = fb_ansi_param(0, (final == 'J' || final == 'K') ? 0u : 1u);
    uint32_t p1 = fb_ansi_param(1, 1);
    uint32_t movement = p0 ? p0 : 1u;
    if (p0 == 0u && (final == 'G' || final == '`' || final == 'd' || final == 'H' || final == 'f')) {
        p0 = 1u;
    }
    if (p1 == 0u && (final == 'H' || final == 'f')) {
        p1 = 1u;
    }

    uint32_t low = fb_modes.origin ? fb_scroll_top : 0;
    uint32_t high = fb_modes.origin ? fb_scroll_bottom : fb_rows - 1u;
    if (final != 'm' && final != 's' && final != 'b') fb_modes.pending_wrap = false;
    switch (final) {
    case 'A':
        fb_row = fb_row > low + movement ? fb_row - movement : low;
        break;
    case 'B':
        fb_row = fb_row + movement <= high ? fb_row + movement : high;
        break;
    case 'C': case 'a':
        fb_col = fb_col + movement < fb_cols ? fb_col + movement : fb_cols - 1u;
        break;
    case 'D':
        fb_col = fb_col >= movement ? fb_col - movement : 0;
        break;
    case 'G': case '`':
        fb_col = p0 > fb_cols ? fb_cols - 1u : p0 - 1u;
        break;
    case 'H':
    case 'f':
        fb_row = low + p0 - 1u > high ? high : low + p0 - 1u;
        fb_col = p1 > fb_cols ? fb_cols - 1u : p1 - 1u;
        break;
    case 'J':
        if (p0 == 2u || p0 == 3u) {
            fb_console_clear_log();
        } else if (p0 == 1u) {
            for (uint32_t row = 0; row < fb_row; ++row) {
                fb_console_clear_row_range(row, 0, fb_cols);
            }
            fb_console_clear_row_range(fb_row, 0, fb_col + 1u);
        } else if (p0 == 0u) {
            fb_console_clear_row_range(fb_row, fb_col, fb_cols);
            for (uint32_t row = fb_row + 1u; row < fb_rows; ++row) {
                fb_console_clear_row_range(row, 0, fb_cols);
            }
        }
        break;
    case 'K':
        if (p0 == 2u) {
            fb_console_clear_row_range(fb_row, 0, fb_cols);
        } else if (p0 == 1u) {
            fb_console_clear_row_range(fb_row, 0, fb_col + 1u);
        } else if (p0 == 0u) {
            fb_console_clear_row_range(fb_row, fb_col, fb_cols);
        }
        break;
    case 'E':
        fb_row += movement;
        if (fb_row > high) fb_row = high;
        fb_col = 0;
        break;
    case 'F':
        fb_row = fb_row > low + movement ? fb_row - movement : low;
        fb_col = 0;
        break;
    case 'd':
        fb_row = low + p0 - 1u > high ? high : low + p0 - 1u;
        break;
    case 'e':
        fb_row += movement;
        if (fb_row > high) fb_row = high;
        break;
    case 'r':
        p0 = fb_ansi_param(0, 1); if (!p0) p0 = 1;
        p1 = fb_ansi_param(1, fb_rows); if (!p1) p1 = fb_rows;
        if (p0 < p1 && p1 <= fb_rows) {
            fb_scroll_top = p0 - 1u;
            fb_scroll_bottom = p1 - 1u;
            fb_row = fb_modes.origin ? fb_scroll_top : 0;
            fb_col = 0;
        }
        break;
    case 'S':
        fb_console_scroll(movement);
        break;
    case 'T':
        fb_console_scroll_region(fb_scroll_top, fb_scroll_bottom, movement, true);
        break;
    case 'X':
        fb_console_clear_row_range(fb_row, fb_col,
                                   fb_col + movement > fb_cols ? fb_cols : fb_col + movement);
        break;
    case '@':
    case 'P':
        fb_console_shift_characters(movement, final == '@');
        break;
    case 'L':
    case 'M':
        if (fb_row >= fb_scroll_top && fb_row <= fb_scroll_bottom)
            fb_console_scroll_region(fb_row, fb_scroll_bottom, movement, final == 'L');
        break;
    case 'I': case 'Z':
        fb_console_tab(movement, final == 'Z');
        break;
    case 'g':
        if (!fb_ansi_param(0, 0)) fb_modes.tabs[fb_col] = false;
        if (fb_ansi_param(0, 0) == 3) __builtin_memset(fb_modes.tabs, 0, sizeof(fb_modes.tabs));
        break;
    case 'b':
        fb_console_repeat(movement);
        break;
    case 'm':
        if (!n) {
            fb_console_reset_attributes();
            break;
        }
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t code = fb_ansi_params[i];
            if (code == 0u) {
                fb_console_reset_attributes();
            } else if (code == 1u) {
                fb_console_bold = true;
            } else if (code == 2u) {
                fb_console_bold = false;
            } else if (code == 4u) {
                fb_console_underline = true;
            } else if (code == 7u) {
                fb_console_reverse = true;
            } else if (code == 8u || code == 28u) {
                fb_modes.conceal = code == 8u;
            } else if (code == 22u) {
                fb_console_bold = false;
            } else if (code == 24u) {
                fb_console_underline = false;
            } else if (code == 27u) {
                fb_console_reverse = false;
            } else if ((code >= 30u && code <= 37u) || (code >= 90u && code <= 97u)) {
                fb_console_fg_color = fb_ansi_color(code);
            } else if (code == 39u) {
                fb_console_fg_color = console_fg();
            } else if (code >= 40u && code <= 47u) {
                fb_console_bg_color = fb_ansi_color(code - 10u);
            } else if (code >= 100u && code <= 107u) {
                fb_console_bg_color = fb_ansi_color(code - 10u);
            } else if (code == 49u) {
                fb_console_bg_color = console_panel();
            } else if (code == 59u) {
                fb_console_underline_color = console_fg();
                fb_console_underline_color_set = false;
            } else if (code == 38u || code == 48u || code == 58u) {
                enum fb_ansi_color_target target = code == 38u ? FB_ANSI_COLOR_FOREGROUND :
                                                    code == 48u ? FB_ANSI_COLOR_BACKGROUND :
                                                                 FB_ANSI_COLOR_UNDERLINE;
                uint32_t last_index = i;
                if (fb_console_apply_extended_color(i, target, &last_index)) {
                    i = last_index;
                } else i = n;
            }
        }
        break;
    case 's':
        fb_console_save_cursor();
        break;
    case 'u':
        fb_console_restore_cursor();
        break;
    default:
        break;
    }
}

void fb_console_ansi_feed(char ch)
{
    unsigned char byte = (unsigned char)ch;
    uint32_t number = fb_console_render_vt ? fb_console_render_vt : console_active_vt;
    if (!fb_console_enabled) return;
    if (byte == 0x18 || byte == 0x1a) {
        fb_console_reset_ansi(); console_utf8.remaining = 0; return;
    }
    if (fb_ansi_state == FB_ANSI_OSC || fb_ansi_state == FB_ANSI_OSC_ESC) {
        if ((fb_ansi_string_osc && byte == 7) ||
            (fb_ansi_state == FB_ANSI_OSC_ESC && ch == '\\')) fb_console_reset_ansi();
        else fb_ansi_state = byte == 27 ? FB_ANSI_OSC_ESC : FB_ANSI_OSC;
        return;
    }
    if (byte == 27) {
        fb_ansi_state = FB_ANSI_ESC; console_utf8.remaining = 0; return;
    }
    /* C0 controls execute inside CSI without terminating its parameter stream. */
    if (byte < 32 || byte == 127) {
        if (byte == 14 || byte == 15) {
            if (number && number <= CONSOLE_VT_COUNT)
                vt_text[number - 1].charset_bank = byte == 14;
        } else if (byte != 7 && byte != 127 && byte != 0) {
            if (byte == 11 || byte == 12) ch = '\n';
            fb_console_putc(ch);
        }
        return;
    }
    if (fb_ansi_state == FB_ANSI_NORMAL) { fb_console_putc(ch); return; }
    if (fb_ansi_state == FB_ANSI_ESC) {
        fb_console_reset_ansi();
        if (ch == '[') {
            fb_ansi_state = FB_ANSI_CSI;
            fb_ansi_prefix = fb_ansi_intermediate = 0;
            fb_ansi_dec_private = fb_ansi_overflow = false;
        } else if (ch == '7') fb_console_save_cursor();
        else if (ch == '8') fb_console_restore_cursor();
        else if (ch == 'c') {
            fb_console_alternate(false, false, false);
            fb_console_reset_alternate();
            if (number && number <= CONSOLE_VT_COUNT) {
                struct console_vt_text *state = &vt_text[number - 1];
                state->application_cursor = state->cursor_hidden = false;
                state->charset[0] = state->charset[1] = state->charset_bank = 0;
            }
            fb_col = fb_row = fb_saved_col = fb_saved_row = 0;
            fb_scroll_top = 0;
            fb_scroll_bottom = fb_rows ? fb_rows - 1 : 0;
            fb_console_insert_mode = false;
            fb_modes = (struct fb_terminal_modes){.wrap = true};
            for (uint32_t col = 8; col < fb_cols; col += 8) fb_modes.tabs[col] = true;
            fb_console_reset_attributes();
            fb_console_save_cursor();
            fb_console_clear_log();
        } else if (ch == 'D' || ch == 'M' || ch == 'E')
            fb_console_index(ch == 'M', ch == 'E');
        else if (ch == 'H') {
            if (fb_col < CONSOLE_MAX_COLS) fb_modes.tabs[fb_col] = true;
        } else if (ch == 'Z') fb_ansi_reply("\033[?6c");
        else if (ch == '(' || ch == ')') {
            fb_ansi_charset_target = ch == ')'; fb_ansi_state = FB_ANSI_CHARSET;
        } else if (ch == ']' || ch == 'P' || ch == '^' || ch == '_' || ch == 'X') {
            fb_ansi_string_osc = ch == ']'; fb_ansi_state = FB_ANSI_OSC;
        } else if (ch == '#' || ch == '%' || ch == '*' || ch == '+' ||
                   ch == '-' || ch == '.' || ch == '/') fb_ansi_state = FB_ANSI_IGNORE_ONE;
        return;
    }
    if (fb_ansi_state == FB_ANSI_CHARSET || fb_ansi_state == FB_ANSI_IGNORE_ONE) {
        if (fb_ansi_state == FB_ANSI_CHARSET && number && number <= CONSOLE_VT_COUNT)
            vt_text[number - 1].charset[fb_ansi_charset_target] = ch == '0';
        fb_console_reset_ansi(); return;
    }
    if (byte >= '0' && byte <= '9') {
        if (fb_ansi_intermediate) fb_ansi_overflow = true;
        uint32_t next = fb_ansi_param_value * 10u + byte - '0';
        fb_ansi_param_value = next > 65535 ? 65535 : next;
        fb_ansi_param_active = true;
    } else if (ch == ';' || ch == ':') {
        if (fb_ansi_intermediate) fb_ansi_overflow = true;
        if (fb_ansi_param_count < FB_ANSI_MAX_PARAMS) {
            fb_ansi_params[fb_ansi_param_count] = fb_ansi_param_value;
            fb_ansi_param_colon[fb_ansi_param_count++] = ch == ':';
        } else fb_ansi_overflow = true;
        fb_ansi_param_value = 0; fb_ansi_param_active = false;
    } else if (byte >= 0x3c && byte <= 0x3f && !fb_ansi_param_count &&
               !fb_ansi_param_active && !fb_ansi_prefix && !fb_ansi_intermediate) {
        fb_ansi_prefix = byte;
        fb_ansi_private = true; fb_ansi_dec_private = ch == '?';
    } else if (byte >= 0x20 && byte <= 0x2f) {
        if (fb_ansi_intermediate) fb_ansi_overflow = true;
        fb_ansi_intermediate = byte;
    } else if (byte >= 0x40 && byte <= 0x7e) {
        if (fb_ansi_param_count || fb_ansi_param_active) {
            if (fb_ansi_param_count < FB_ANSI_MAX_PARAMS) {
                fb_ansi_params[fb_ansi_param_count] = fb_ansi_param_value;
                fb_ansi_param_colon[fb_ansi_param_count++] = false;
            } else fb_ansi_overflow = true;
        }
        if (!fb_ansi_overflow) fb_console_execute_csi(ch);
        fb_console_reset_ansi();
    } else fb_ansi_overflow = true;
}
