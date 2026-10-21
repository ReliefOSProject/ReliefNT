#include "internal.h"

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
    uint32_t p0 = fb_ansi_param(0, (final == 'J' || final == 'K') ? 0u : 1u);
    uint32_t p1 = fb_ansi_param(1, 1);
    uint32_t movement = p0 ? p0 : 1u;
    if (p0 == 0u && (final == 'G' || final == 'H' || final == 'f')) {
        p0 = 1u;
    }
    if (p1 == 0u && (final == 'H' || final == 'f')) {
        p1 = 1u;
    }

    switch (final) {
    case 'A':
        fb_row = fb_row >= movement ? fb_row - movement : 0;
        break;
    case 'B':
        fb_row = fb_row + movement < fb_rows ? fb_row + movement : fb_rows - 1u;
        break;
    case 'C':
        fb_col = fb_col + movement < fb_cols ? fb_col + movement : fb_cols - 1u;
        break;
    case 'D':
        fb_col = fb_col >= movement ? fb_col - movement : 0;
        break;
    case 'G':
        fb_col = p0 > fb_cols ? fb_cols - 1u : p0 - 1u;
        break;
    case 'H':
    case 'f':
        fb_row = p0 > fb_rows ? fb_rows - 1u : p0 - 1u;
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
        } else {
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
        } else {
            fb_console_clear_row_range(fb_row, fb_col, fb_cols);
        }
        break;
    case 'E':
        fb_row += movement;
        if (fb_row >= fb_rows) fb_row = fb_rows - 1u;
        fb_col = 0;
        break;
    case 'F':
        fb_row = fb_row >= movement ? fb_row - movement : 0u;
        fb_col = 0;
        break;
    case 'd':
        fb_row = p0 > fb_rows ? fb_rows - 1u : p0 - 1u;
        break;
    case 'e':
        fb_row += movement;
        if (fb_row >= fb_rows) fb_row = fb_rows - 1u;
        break;
    case 'h':
    case 'l':
        /* Accept cursor visibility, bracketed paste and other mode toggles. */
        break;
    case 'r':
        if (n >= 2u && p0 >= 1u && p1 >= p0 && p1 <= fb_rows) {
            fb_scroll_top = p0 - 1u;
            fb_scroll_bottom = p1 - 1u;
            fb_row = fb_scroll_top;
            fb_col = 0;
        }
        break;
    case 'S':
        for (uint32_t i = 0; i < movement; ++i) fb_console_scroll();
        break;
    case 'X':
    case '@':
    case 'P':
        fb_console_clear_row_range(fb_row, fb_col,
                                   fb_col + movement > fb_cols ? fb_cols : fb_col + movement);
        break;
    case 'L':
    case 'M':
        fb_console_clear_row_range(fb_row, 0, fb_cols);
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
                }
            }
        }
        break;
    case 's':
        fb_saved_col = fb_col;
        fb_saved_row = fb_row;
        break;
    case 'u':
        fb_col = fb_saved_col < fb_cols ? fb_saved_col : 0u;
        fb_row = fb_saved_row < fb_rows ? fb_saved_row : 0u;
        break;
    default:
        break;
    }
}

void fb_console_ansi_feed(char ch)
{
    unsigned char byte = (unsigned char)ch;
    if (!fb_console_enabled) {
        return;
    }
    if (fb_ansi_state == FB_ANSI_NORMAL) {
        if (byte == 0x07u || byte == 0x7fu || byte == 0x00u || byte == 0x0eu || byte == 0x0fu) {
            return;
        }
        if (byte == 0x1bU) {
            fb_ansi_state = FB_ANSI_ESC;
            return;
        }
        fb_console_putc(ch);
        return;
    }
    if (fb_ansi_state == FB_ANSI_ESC) {
        if (ch == '[') {
            fb_ansi_state = FB_ANSI_CSI;
            fb_ansi_param_count = 0;
            fb_ansi_param_value = 0;
            fb_ansi_param_active = false;
            return;
        }
        if (ch == '7') {
            fb_saved_col = fb_col;
            fb_saved_row = fb_row;
            fb_console_reset_ansi();
            return;
        }
        if (ch == 'c') {
            fb_col = 0;
            fb_row = 0;
            fb_scroll_top = 0;
            fb_scroll_bottom = fb_rows ? fb_rows - 1u : 0u;
            fb_console_reset_attributes();
            fb_console_clear_log();
            fb_console_reset_ansi();
            return;
        }
        if (ch == 'D') {
            fb_console_newline();
            fb_console_reset_ansi();
            return;
        }
        if (ch == 'M') {
            if (fb_row > fb_scroll_top) --fb_row;
            fb_console_reset_ansi();
            return;
        }
        if (ch == 'E') {
            fb_console_newline();
            fb_col = 0;
            fb_console_reset_ansi();
            return;
        }
        if (ch == '(' || ch == ')') {
            fb_ansi_state = FB_ANSI_CHARSET;
            return;
        }
        if (ch == ']' || ch == 'P' || ch == '^' || ch == '_' || ch == 'X') {
            fb_ansi_state = FB_ANSI_OSC;
            return;
        }
        if (ch == '\\' || ch == '=' || ch == '>') {
            fb_console_reset_ansi();
            return;
        }
        if (ch == '8') {
            fb_col = fb_saved_col < fb_cols ? fb_saved_col : 0u;
            fb_row = fb_saved_row < fb_rows ? fb_saved_row : 0u;
            fb_console_reset_ansi();
            return;
        }
        /* Unknown ESC sequences are control traffic; never render their
         * final byte as user-visible text. */
        fb_console_reset_ansi();
        return;
    }
    if (fb_ansi_state == FB_ANSI_CHARSET) {
        fb_console_reset_ansi();
        return;
    }
    if (fb_ansi_state == FB_ANSI_OSC) {
        if (byte == 0x07u) {
            fb_console_reset_ansi();
        } else if (byte == 0x1bu) {
            fb_ansi_state = FB_ANSI_OSC_ESC;
        }
        return;
    }
    if (fb_ansi_state == FB_ANSI_OSC_ESC) {
        fb_console_reset_ansi();
        return;
    }
    if ((ch == '?' || ch == '>') && !fb_ansi_param_count &&
        !fb_ansi_param_active) {
        fb_ansi_private = true;
        return;
    }
    if (byte >= '0' && byte <= '9') {
        if (fb_ansi_param_value < 10000u) {
            fb_ansi_param_value = fb_ansi_param_value * 10u + (byte - '0');
        }
        fb_ansi_param_active = true;
        return;
    }
    if (ch == ';' || ch == ':') {
        if (fb_ansi_param_count < FB_ANSI_MAX_PARAMS) {
            fb_ansi_params[fb_ansi_param_count++] =
                fb_ansi_param_active ? fb_ansi_param_value : 0u;
            fb_ansi_param_colon[fb_ansi_param_count - 1u] = ch == ':';
        }
        fb_ansi_param_value = 0;
        fb_ansi_param_active = false;
        return;
    }
    if (byte >= 0x40u && byte <= 0x7eu) {
        if (fb_ansi_param_count < FB_ANSI_MAX_PARAMS &&
            (fb_ansi_param_active || fb_ansi_param_count)) {
            fb_ansi_params[fb_ansi_param_count++] =
                fb_ansi_param_active ? fb_ansi_param_value : 0u;
            fb_ansi_param_colon[fb_ansi_param_count - 1u] = false;
        }
        if (!fb_ansi_private) {
            fb_console_execute_csi(ch);
        }
        fb_console_reset_ansi();
        return;
    }
    fb_console_reset_ansi();
}
