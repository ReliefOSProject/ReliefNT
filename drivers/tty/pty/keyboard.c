#include "internal.h"

static int console_key_to_bytes(uint8_t keycode, char *buffer, uint32_t *length);

/**
 * @brief Record a modifier make/break code and refresh the derived key levels.
 * @param held Bit identifying this physical modifier key.
 * @param pressed Non-zero for a make code, zero for a break code.
 *
 * The deferred keyboard path holds the kernel execution transaction, so
 * the held-key set needs no additional lock. Tracking left and right sides separately
 * keeps, for example, a released left Ctrl from clearing a still-held right
 * Ctrl, which would otherwise turn the next plain letter into a control byte.
 */
static void console_update_modifier(uint32_t held, uint8_t pressed);


static int console_key_to_bytes(uint8_t keycode, char *buffer, uint32_t *length)
{
    char ch = 0;
    if (!buffer || !length) {
        return 0;
    }
    *length = 0;
    switch (keycode) {
    case 1: buffer[0] = '\033'; *length = 1; return 1;
    case 72: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'A'; *length = 3; return 1;
    case 80: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'B'; *length = 3; return 1;
    case 75: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'D'; *length = 3; return 1;
    case 77: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'C'; *length = 3; return 1;
    case 71: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'H'; *length = 3; return 1;
    case 79: buffer[0] = '\033'; buffer[1] = '['; buffer[2] = 'F'; *length = 3; return 1;
    case 28: ch = '\r'; break;
    case 14: ch = '\177'; break;
    case 15: ch = '\t'; break;
    case 57: ch = ' '; break;
    case 2: ch = '1'; break; case 3: ch = '2'; break; case 4: ch = '3'; break;
    case 5: ch = '4'; break; case 6: ch = '5'; break; case 7: ch = '6'; break;
    case 8: ch = '7'; break; case 9: ch = '8'; break; case 10: ch = '9'; break;
    case 11: ch = '0'; break; case 12: ch = '-'; break; case 13: ch = '='; break;
    case 16: ch = 'q'; break; case 17: ch = 'w'; break; case 18: ch = 'e'; break;
    case 19: ch = 'r'; break; case 20: ch = 't'; break; case 21: ch = 'y'; break;
    case 22: ch = 'u'; break; case 23: ch = 'i'; break; case 24: ch = 'o'; break;
    case 25: ch = 'p'; break; case 26: ch = '['; break; case 27: ch = ']'; break;
    case 30: ch = 'a'; break; case 31: ch = 's'; break; case 32: ch = 'd'; break;
    case 33: ch = 'f'; break; case 34: ch = 'g'; break; case 35: ch = 'h'; break;
    case 36: ch = 'j'; break; case 37: ch = 'k'; break; case 38: ch = 'l'; break;
    case 39: ch = ';'; break; case 40: ch = '\''; break; case 41: ch = '`'; break;
    case 43: ch = '\\'; break; case 44: ch = 'z'; break; case 45: ch = 'x'; break;
    case 46: ch = 'c'; break; case 47: ch = 'v'; break; case 48: ch = 'b'; break;
    case 49: ch = 'n'; break; case 50: ch = 'm'; break; case 51: ch = ','; break;
    case 52: ch = '.'; break; case 53: ch = '/'; break;
    default: return 0;
    }
    if (ch >= 'a' && ch <= 'z') {
        if ((console_shift_down ? 1 : 0) != input_caps_lock_active()) {
            ch = (char)(ch - 'a' + 'A');
        }
    } else if (console_shift_down) {
        switch (ch) {
        case '1': ch = '!'; break; case '2': ch = '@'; break; case '3': ch = '#'; break;
        case '4': ch = '$'; break; case '5': ch = '%'; break; case '6': ch = '^'; break;
        case '7': ch = '&'; break; case '8': ch = '*'; break; case '9': ch = '('; break;
        case '0': ch = ')'; break; case '-': ch = '_'; break; case '=': ch = '+'; break;
        case '[': ch = '{'; break; case ']': ch = '}'; break; case ';': ch = ':'; break;
        case '\'': ch = '"'; break; case '`': ch = '~'; break; case '\\': ch = '|'; break;
        case ',': ch = '<'; break; case '.': ch = '>'; break; case '/': ch = '?'; break;
        default: break;
        }
    }
    if (console_ctrl_down && ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))) {
        ch = (char)(((ch | 0x20) - 'a') + 1);
    }
    buffer[0] = ch;
    *length = 1;
    return 1;
}

/**
 * @brief Record a modifier make/break code and refresh the derived key levels.
 * @param held Bit identifying this physical modifier key.
 * @param pressed Non-zero for a make code, zero for a break code.
 *
 * The deferred keyboard path holds the kernel execution transaction, so
 * the held-key set needs no additional lock. Tracking left and right sides separately
 * keeps, for example, a released left Ctrl from clearing a still-held right
 * Ctrl, which would otherwise turn the next plain letter into a control byte.
 */
static void console_update_modifier(uint32_t held, uint8_t pressed)
{
    if (pressed) {
        console_modifier_keys |= held;
    } else {
        console_modifier_keys &= ~held;
    }
    console_shift_down = (uint8_t)((console_modifier_keys &
                                    (CONSOLE_KEY_LSHIFT | CONSOLE_KEY_RSHIFT)) != 0);
    console_ctrl_down = (uint8_t)((console_modifier_keys &
                                   (CONSOLE_KEY_LCTRL | CONSOLE_KEY_RCTRL)) != 0);
    console_alt_down = (uint8_t)((console_modifier_keys &
                                  (CONSOLE_KEY_LALT | CONSOLE_KEY_RALT)) != 0);
}

/**
 * @brief Offer one physical keyboard event to the console terminal.
 * @param keycode Set-1 make/break code after 0xe0 extension normalization.
 * @param pressed Non-zero for a make code, zero for a break code.
 * @return Nothing.
 *
 * Keyboard translation is independent of KD display mode. OFF and raw modes
 * suppress cooked shortcuts; raw streams include modifiers and supported E0
 * extensions. The deferred input consumer still forwards the physical
 * Ctrl+Alt+Fn chord while a graphical evdev grab is active, so this handler
 * can complete a VT switch without leaking printable keys to the console.
 * It runs after VT initialization under the kernel execution transaction,
 * never from a keyboard ISR.
 */
void pty_console_key_event(uint8_t keycode, uint8_t pressed)
{
    uint32_t pty_id = active_vt;
    struct pty_session *session = find_session(pty_id);
    char bytes[8];
    uint32_t length;
    int keyboard_mode;
    int modifier = 1;
    if (!session || !pty_vt_number(pty_id)) return;
    switch (keycode) {
    case 42: console_update_modifier(CONSOLE_KEY_LSHIFT, pressed); break;
    case 54: console_update_modifier(CONSOLE_KEY_RSHIFT, pressed); break;
    case 29: console_update_modifier(CONSOLE_KEY_LCTRL, pressed); break;
    case 116: console_update_modifier(CONSOLE_KEY_RCTRL, pressed); break;
    case 56: console_update_modifier(CONSOLE_KEY_LALT, pressed); break;
    case 115: console_update_modifier(CONSOLE_KEY_RALT, pressed); break;
    case 58: break;
    default: modifier = 0; break;
    }
    keyboard_mode = vt_keyboard_modes[pty_id - 1u];
    /* VT switching is handled by the console controller before keyboard
     * translation. K_RAW/K_MEDIUMRAW must not make Ctrl+Alt+F1..F6 vanish
     * when an evdev client still owns the device grab. */
    if (vt_ready && (keyboard_mode == K_XLATE ||
                     vt_graphical[pty_id - 1u]) &&
        pressed && console_ctrl_down && console_alt_down &&
        keycode >= 59u && keycode <= 64u) {
        (void)pty_vt_switch((uint32_t)keycode - 58u);
        return;
    }
    if (keyboard_mode == K_OFF) return;
    if (keyboard_mode == K_RAW || keyboard_mode == K_MEDIUMRAW) {
        uint8_t code = keycode;
        uint8_t extended = 0;
        if (keyboard_mode == K_RAW) {
            switch (keycode) {
            case 112: code = 0x5b; extended = 1; break;
            case 113: code = 0x5c; extended = 1; break;
            case 114: code = 0x5d; extended = 1; break;
            case 115: code = 0x38; extended = 1; break;
            case 116: code = 0x1d; extended = 1; break;
            /* The existing normalized path treats these as navigation keys. */
            case 71: case 72: case 73: case 75: case 77:
            case 79: case 80: case 81: case 82: case 83:
                extended = 1; break;
            default: break;
            }
        } else {
            switch (keycode) {
            case 71: code = KEY_HOME; break; case 72: code = KEY_UP; break;
            case 73: code = KEY_PAGEUP; break; case 75: code = KEY_LEFT; break;
            case 77: code = KEY_RIGHT; break; case 79: code = KEY_END; break;
            case 80: code = KEY_DOWN; break; case 81: code = KEY_PAGEDOWN; break;
            case 82: code = KEY_INSERT; break; case 83: code = KEY_DELETE; break;
            case 112: code = KEY_LEFTMETA; break;
            case 113: code = KEY_RIGHTMETA; break;
            case 114: code = KEY_COMPOSE; break;
            case 115: code = KEY_RIGHTALT; break;
            case 116: code = KEY_RIGHTCTRL; break;
            default: break;
            }
        }
        length = 0;
        if (extended) bytes[length++] = (char)0xe0;
        if (keyboard_mode == K_MEDIUMRAW && code >= 128) {
            bytes[length++] = (char)(pressed ? 0 : 0x80);
            bytes[length++] = (char)((code >> 7) | 0x80);
            bytes[length++] = (char)(code | 0x80);
        } else bytes[length++] = (char)(code | (pressed ? 0 : 0x80));
        (void)pty_write_input(session->owner_pid, pty_id, bytes, length);
        return;
    }
    if (modifier) return;
    /* Keep the diagnostic escape hatch on the logging console. */
    if (pressed && keycode == 88 && console_ctrl_down &&
        console_alt_down && console_shift_down) {
        console_info("[console] diagnostic shortcut received on VT%u\n", pty_id);
        return;
    }
    if (!pressed || !console_key_to_bytes(keycode, bytes, &length)) {
        return;
    }
    /* Feed through the same canonical/ISIG handling used by PTY hosts. */
    (void)pty_write_input(session->owner_pid, pty_id, bytes, length);
    if (session->termios.c_lflag & RELIEFOS_PTY_LFLAG_ECHO) {
        if (keycode == 14) {
            static const char erase[] = "\b \b";
            console_vt_write(active_vt, erase, sizeof(erase) - 1U);
        } else if (keycode == 28) {
            static const char newline[] = "\r\n";
            console_vt_write(active_vt, newline, sizeof(newline) - 1U);
        } else if (length == 1 && (uint8_t)bytes[0] >= 32U) {
            console_vt_write(active_vt, bytes, 1);
        }
    }
}
