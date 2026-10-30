/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_USB_DEVICE_H
#define RELIEFNT_USB_DEVICE_H

#include <reliefnt/types.h>

struct module;
struct usb_device;
/* The first three speed encodings also match EHCI queue-head EPS. */
enum usb_speed { USB_SPEED_FULL, USB_SPEED_LOW, USB_SPEED_HIGH, USB_SPEED_SUPER,
                 USB_SPEED_SUPER_PLUS };

struct usb_endpoint {
    uint8_t address, interval, toggle, ping;
    uint16_t max_packet;
    /* SuperSpeed endpoint companion fields; zero for USB2 endpoints. */
    uint8_t max_burst, companion_attributes;
    uint16_t bytes_per_interval;
};

/* Caller owns the host's transaction gate across a complete class command.
 * Every DMA buffer is transport-owned; callers supply ordinary CPU memory.
 * Transfers return a negative errno or zero with the actual byte count.
 * A stalled endpoint returns -EPIPE, and a removed device returns -ENODEV.
 * clear_halt resets the wire endpoint and its software data toggle together. */
struct usb_device_operations {
    int (*acquire)(struct usb_device *device);
    void (*release)(struct usb_device *device);
    int (*control)(struct usb_device *device, const uint8_t setup[8],
                   void *data, uint16_t length, uint32_t *actual);
    int (*bulk)(struct usb_device *device, struct usb_endpoint *endpoint,
                void *data, uint32_t length, uint32_t *actual);
    int (*clear_halt)(struct usb_device *device, struct usb_endpoint *endpoint);
};

/* Host-owned identity, stable until class detach has drained its block users.
 * disconnected is atomic: IRQ observation revokes I/O before task reclamation.
 * The class uses owner for every block registration; it never self-pins it.
 * The host retains this object and all callbacks through that owner's ledger. */
struct usb_device {
    const struct usb_device_operations *operations;
    void *host_context;
    struct module *owner;
    uint8_t address, speed, root_port;
    uint16_t ep0_max_packet;
    uint8_t hub_address, hub_port;
    uint8_t bus, slot, function;
    uint16_t vendor, product;
    bool disconnected;
};

struct usb_storage;
/** @brief Discover SCSI BOT LUNs and publish native removable block devices.
 * @param device Stable configured class device with its host gate already held.
 * @param interface_number BOT interface. @param in Bulk IN. @param out Bulk OUT.
 * @param result Required empty owned class-state output, including partial probe.
 * @return Zero or negative errno. Task/init context; no registry lock spans I/O.
 * The caller owns class state through successful detach or stopped final release.
 */
int usb_storage_probe(struct usb_device *device, uint8_t interface_number,
                      struct usb_endpoint in, struct usb_endpoint out,
                      struct usb_storage **result);
/** @brief Revoke a disconnected class and unpublish each idle block identity.
 * @param storage Class state, nullable. Device is already marked disconnected.
 * @return Zero when all identities are retired, -EBUSY while open/mounted users
 * retain any identity. Task context; no DMA or waits. State stays caller-owned.
 */
int usb_storage_disconnect(struct usb_storage *storage);
/** @brief Consume class state after successful disconnect or owner ledger drain.
 * @param storage Owned state, nullable; no admitted block users or callbacks.
 * Task teardown, hardware already stopped. Frees CPU metadata only.
 */
void usb_storage_release(struct usb_storage *storage);

#endif
