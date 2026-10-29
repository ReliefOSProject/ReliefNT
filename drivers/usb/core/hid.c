/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/usb_hid.h>
#include <reliefnt/input.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/export.h>

static uint8_t usb_hid_usage_to_keycode(uint8_t usage)
{
    static const uint8_t letters[26] = {
        0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24,
        0x25, 0x26, 0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14,
        0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
    };
    if (usage >= 0x04u && usage <= 0x1du) {
        return letters[usage - 0x04u];
    }
    if (usage >= 0x1eu && usage <= 0x27u) {
        static const uint8_t digits[10] = {0x02, 0x03, 0x04, 0x05, 0x06,
                                           0x07, 0x08, 0x09, 0x0a, 0x0b};
        return digits[usage - 0x1eu];
    }
    if (usage >= 0x53u && usage <= 0x63u)
        return (uint8_t)(INPUT_KEY_NUMLOCK + usage - 0x53u);
    switch (usage) {
    case 0x28: return 0x1cu;
    case 0x29: return 0x01u;
    case 0x2a: return 0x0eu;
    case 0x2b: return 0x0fu;
    case 0x2c: return 0x39u;
    case 0x2d: return 0x0cu;
    case 0x2e: return 0x0du;
    case 0x2f: return 0x1au;
    case 0x30: return 0x1bu;
    case 0x31: return 0x2bu;
    case 0x33: return 0x27u;
    case 0x34: return 0x28u;
    case 0x35: return 0x29u;
    case 0x36: return 0x33u;
    case 0x37: return 0x34u;
    case 0x38: return 0x35u;
    case 0x39: return 0x3au;
    case 0x3a: return 0x3bu;
    case 0x3b: return 0x3cu;
    case 0x3c: return 0x3du;
    case 0x3d: return 0x3eu;
    case 0x3e: return 0x3fu;
    case 0x3f: return 0x40u;
    case 0x40: return 0x41u;
    case 0x41: return 0x42u;
    case 0x42: return 0x43u;
    case 0x43: return 0x44u;
    case 0x44: return 0x57u;
    case 0x45: return 0x58u;
    case 0x49: return 0x52u;
    case 0x4a: return 0x47u;
    case 0x4b: return 0x49u;
    case 0x4c: return 0x53u;
    case 0x4d: return 0x4fu;
    case 0x4e: return 0x51u;
    case 0x4f: return 0x4du;
    case 0x50: return 0x4bu;
    case 0x51: return 0x50u;
    case 0x52: return 0x48u;
    default: return 0;
    }
}

static uint8_t usb_hid_modifier_keycode(uint8_t bit)
{
    static const uint8_t codes[8] = {29u, 42u, 56u, 112u, 116u, 54u, 115u, 113u};
    return bit < 8u ? codes[bit] : 0;
}

static int usb_hid_contains(const uint8_t keys[6], uint8_t usage)
{
    for (uint32_t i = 0; i < 6; ++i) {
        if (keys[i] == usage) {
            return 1;
        }
    }
    return 0;
}

/** @brief Diff a complete, non-rollover keyboard report against held state. */
void usb_hid_boot_keyboard(struct usb_hid_state *device,
                                    const uint8_t *report, uint32_t length)
{
    uint8_t modifier;
    uint8_t keys[6] = {0};
    if (!device || !report || length < 8) {
        return;
    }
    /* ErrorRollOver, POSTFail and ErrorUndefined do not release held keys. */
    for (unsigned i = 2; i < 8; ++i)
        if (report[i] >= 1 && report[i] <= 3) return;
    modifier = report[0];
    for (uint32_t i = 0; i < 6 && i + 2u < length; ++i) {
        keys[i] = report[i + 2u];
    }
    for (uint8_t bit = 0; bit < 8; ++bit) {
        uint8_t old_state = (uint8_t)((device->modifier >> bit) & 1u);
        uint8_t new_state = (uint8_t)((modifier >> bit) & 1u);
        if (old_state != new_state) {
            input_handle_scancode(usb_hid_modifier_keycode(bit), new_state);
        }
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (device->keys[i] && !usb_hid_contains(keys, device->keys[i])) {
            uint8_t code = usb_hid_usage_to_keycode(device->keys[i]);
            if (code) {
                input_handle_scancode(code, 0);
            }
        }
    }
    for (uint32_t i = 0; i < 6; ++i) {
        if (keys[i] && !usb_hid_contains(device->keys, keys[i])) {
            bool duplicate = false;
            for (unsigned old = 0; old < i; ++old) if (keys[old] == keys[i]) duplicate = true;
            if (duplicate) continue;
            uint8_t code = usb_hid_usage_to_keycode(keys[i]);
            if (code) {
                input_handle_scancode(code, 1);
            }
        }
    }
    device->modifier = modifier;
    __builtin_memcpy(device->keys, keys, sizeof(device->keys));
}

/** @brief Decode boot mouse bytes with USB's positive-down screen Y. */
void usb_hid_boot_mouse(struct usb_hid_state *state, const uint8_t *report,
                        uint32_t length)
{
    if (!state || !report || length < 3) return;
    int32_t dx = (int8_t)report[1], dy = (int8_t)report[2];
    const struct framebuffer *fb = framebuffer_get();
    if (fb->available && fb->width && fb->height) {
        state->x += dx; state->y += dy;
        if (state->x < 0) state->x = 0;
        if (state->y < 0) state->y = 0;
        if ((uint32_t)state->x >= fb->width) state->x = fb->width - 1;
        if ((uint32_t)state->y >= fb->height) state->y = fb->height - 1;
    }
    state->buttons = report[0] & 7u;
    input_push_mouse(state->x, state->y, dx, dy, state->buttons);
    if (length >= 4 && report[3])
        input_push_mouse_wheel(state->x, state->y, (int8_t)report[3], state->buttons);
}

/** @brief Release held physical input before retiring its USB interface. */
void usb_hid_boot_release(struct usb_hid_state *state)
{
    if (!state) return;
    const uint8_t zero[8] = {0};
    usb_hid_boot_keyboard(state, zero, sizeof(zero));
    if (state->buttons) {
        state->buttons = 0;
        input_push_mouse(state->x, state->y, 0, 0, 0);
    }
}
EXPORT_SYMBOL(usb_hid_boot_keyboard);
EXPORT_SYMBOL(usb_hid_boot_mouse);
EXPORT_SYMBOL(usb_hid_boot_release);
