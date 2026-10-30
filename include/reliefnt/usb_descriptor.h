/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_USB_DESCRIPTOR_H
#define RELIEFNT_USB_DESCRIPTOR_H
#include <reliefnt/usb_device.h>

#define USB_MAX_INTERFACES 32u
#define USB_MAX_CONFIG 4096u

struct usb_config_interface {
    uint8_t number, class_code, subclass, protocol, endpoint_count;
    bool unsupported_endpoint;
    struct usb_endpoint in, out, interrupt;
};
struct usb_config {
    uint8_t value, count;
    struct usb_config_interface interfaces[USB_MAX_INTERFACES];
};

/* Parse an entire configuration and reject malformed SuperSpeed companions. */
int usb_parse_configuration(const uint8_t *bytes, uint32_t length,
                            uint8_t speed, struct usb_config *out);
#endif
