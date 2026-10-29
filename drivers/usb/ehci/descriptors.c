/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Validate a USB2 endpoint's packet geometry without masking invalid bits. */
static bool endpoint_valid(uint8_t speed, uint8_t type, uint16_t packet, uint8_t interval)
{
    if (!packet || (packet & 0xf800u)) return false;
    if (type == 2) {
        if (speed == USB_SPEED_HIGH) return packet == 512;
        return speed == USB_SPEED_FULL && (packet == 8 || packet == 16 || packet == 32 || packet == 64);
    }
    if (type == 3) {
        if (!interval) return false;
        if (speed == USB_SPEED_HIGH) return interval <= 16 && packet <= 1024;
        return packet <= (speed == USB_SPEED_LOW ? 8 : 64);
    }
    return true;
}

/** @brief Parse bounded alternate-zero class interfaces from a complete configuration. */
int usb_parse_configuration(const uint8_t *bytes, uint32_t length,
                             uint8_t speed, struct usb_config *out)
{
    if (!bytes || !out || length < 9 || bytes[0] != 9 || bytes[1] != 2 ||
        !bytes[4] || !bytes[5] || speed > USB_SPEED_HIGH) return -EINVAL;
    uint32_t total = bytes[2] | (uint32_t)bytes[3] << 8;
    if (total != length || total > EHCI_MAX_CONFIG) return -EINVAL;
    *out = (struct usb_config){.value = bytes[5]};
    struct usb_config_interface *interface = NULL;
    uint8_t expected = 0, seen = 0;
    for (uint32_t offset = bytes[0]; offset < total;) {
        if (total - offset < 2 || bytes[offset] < 2 || bytes[offset] > total - offset)
            return -EINVAL;
        const uint8_t *descriptor = bytes + offset;
        if (descriptor[1] == 4) {
            if (interface && seen != expected) return -EINVAL;
            interface = NULL; seen = expected = 0;
            if (descriptor[0] < 9) return -EINVAL;
            if (!descriptor[3]) {
                if (out->count == EHCI_MAX_INTERFACES) return -ENOSPC;
                for (unsigned i = 0; i < out->count; ++i)
                    if (out->interfaces[i].number == descriptor[2]) return -EINVAL;
                interface = &out->interfaces[out->count++];
                *interface = (struct usb_config_interface){.number = descriptor[2],
                    .class_code = descriptor[5], .subclass = descriptor[6], .protocol = descriptor[7]};
                expected = descriptor[4];
            }
        } else if (descriptor[1] == 5 && interface) {
            if (descriptor[0] < 7 || !(descriptor[2] & 15u) || (descriptor[2] & 0x70u))
                return -EINVAL;
            uint8_t type = descriptor[3] & 3u;
            uint16_t packet = descriptor[4] | (uint16_t)descriptor[5] << 8;
            if (!endpoint_valid(speed, type, packet, descriptor[6]) || ++seen > expected)
                return -EINVAL;
            struct usb_endpoint *endpoint = NULL;
            if (type == 2) endpoint = descriptor[2] & 0x80u ? &interface->in : &interface->out;
            if (type == 3 && (descriptor[2] & 0x80u)) endpoint = &interface->interrupt;
            if (endpoint) {
                if (endpoint->address) return -EINVAL;
                *endpoint = (struct usb_endpoint){.address = descriptor[2], .max_packet = packet,
                    .interval = descriptor[6]};
            }
        }
        offset += descriptor[0];
    }
    if ((interface && seen != expected) || out->count != bytes[4]) return -EINVAL;
    return 0;
}
