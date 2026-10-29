/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_USB_HID_H
#define RELIEFNT_USB_HID_H
#include <reliefnt/types.h>

struct usb_hid_state {
    uint8_t modifier, keys[6], buttons;
    int32_t x, y;
};

/** @brief Translate one complete HID boot keyboard report, ignoring rollover.
 * @param state Per-interface held-key state, exclusively owned by the poller.
 * @param report Borrowed boot report. @param length Actual readable byte count.
 * IRQ/task context; emits physical input without allocation or blocking.
 */
void usb_hid_boot_keyboard(struct usb_hid_state *state, const uint8_t *report,
                           uint32_t length);
/** @brief Deliver USB-relative mouse motion with screen-positive-down Y.
 * @param state Per-interface pointer state. @param report Borrowed boot bytes.
 * @param length Actual readable byte count. IRQ/task context, bounded input only.
 * Boot protocol's first three bytes are decoded; optional fourth byte is wheel.
 */
void usb_hid_boot_mouse(struct usb_hid_state *state, const uint8_t *report,
                        uint32_t length);
/** @brief Release all held keys/buttons before an interface is disconnected.
 * @param state Exclusive retained interface state. IRQ/task context; no wait.
 */
void usb_hid_boot_release(struct usb_hid_state *state);
#endif
