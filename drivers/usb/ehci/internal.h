/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EHCI_INTERNAL_H
#define RELIEFNT_EHCI_INTERNAL_H
#include <reliefnt/usb.h>
#include <reliefnt/usb_device.h>
#include <reliefnt/usb_hid.h>
#include <reliefnt/pci.h>
#include <reliefnt/driver_module.h>
#include <linux/errno.h>

#define EHCI_FRAME_COUNT 1024u
#define EHCI_MAX_HID 32u
#define EHCI_MAX_INTERFACES 32u
#define EHCI_MAX_PORTS 15u
#define EHCI_MAX_CONFIG 4096u
#define EHCI_TRANSFER_BYTES 65536u
#define EHCI_QTD_COUNT 8u
#define EHCI_TERMINATE 1u
#define EHCI_LINK_QH 2u
#define EHCI_CMD 0x00u
#define EHCI_STATUS 0x04u
#define EHCI_INTR 0x08u
#define EHCI_FRINDEX 0x0cu
#define EHCI_SEGMENT 0x10u
#define EHCI_PERIODIC 0x14u
#define EHCI_ASYNC 0x18u
#define EHCI_CONFIGFLAG 0x40u
#define EHCI_PORTSC 0x44u
#define EHCI_RUN 1u
#define EHCI_RESET 2u
#define EHCI_PSE (1u << 4)
#define EHCI_ASE (1u << 5)
#define EHCI_HALTED (1u << 12)
#define EHCI_PSS (1u << 14)
#define EHCI_ASS (1u << 15)
#define EHCI_PORT_CONNECT 1u
#define EHCI_PORT_CHANGE 2u
#define EHCI_PORT_ENABLE 4u
#define EHCI_PORT_W1C 0x2au
#define EHCI_PORT_RESET (1u << 8)
#define EHCI_PORT_POWER (1u << 12)
#define EHCI_PORT_OWNER (1u << 13)
#define EHCI_TD_ACTIVE (1u << 7)
#define EHCI_TD_HALTED (1u << 6)
#define EHCI_TD_ERRORS (0x3cu)
#define EHCI_PID_OUT 0u
#define EHCI_PID_IN 1u
#define EHCI_PID_SETUP 2u

/* Extended buffer words are zero: all schedules/data use a 32-bit DMA mask.
 * Padding keeps every hardware object within one 4KiB page. */
struct ehci_qtd {
    volatile uint32_t next, alternate, token, buffer[5], high_buffer[5];
    uint32_t reserved[3];
} __attribute__((aligned(32)));
struct ehci_qh {
    volatile uint32_t link, characteristics, capabilities, current;
    volatile uint32_t next, alternate, token, buffer[5], high_buffer[5];
    uint32_t reserved[15];
} __attribute__((aligned(32)));
struct ehci_dma {
    uint32_t frames[EHCI_FRAME_COUNT] __attribute__((aligned(4096)));
    struct ehci_qh head, transfer;
    struct ehci_qtd descriptors[EHCI_QTD_COUNT];
    struct ehci_qh hid_heads[EHCI_MAX_HID] __attribute__((aligned(4096)));
    struct ehci_qtd hid_descriptors[EHCI_MAX_HID];
    uint8_t reports[EHCI_MAX_HID][1024] __attribute__((aligned(4096)));
    uint8_t setup[8] __attribute__((aligned(64)));
    uint8_t data[EHCI_TRANSFER_BYTES] __attribute__((aligned(4096)));
};
struct usb_config_interface {
    uint8_t number, class_code, subclass, protocol;
    struct usb_endpoint in, out, interrupt;
};
struct usb_config {
    uint8_t value, count;
    struct usb_config_interface interfaces[EHCI_MAX_INTERFACES];
};
struct ehci_device {
    struct usb_device usb;
    struct ehci_device *next, *parent;
    struct usb_storage *storage[EHCI_MAX_INTERFACES];
    uint8_t parent_port, hub_ports, depth, storage_count;
    /* Single-TT altsetting: unresolved bulk/control clears block this route. */
    uint16_t tt_value;
    uint8_t tt_pending;
};
struct ehci_bandwidth {
    uint16_t period, phase;
    uint8_t smask, cmask;
    uint32_t start_ns, complete_ns, tt_ns;
};
struct ehci_hid {
    struct ehci_device *device;
    struct usb_endpoint endpoint;
    struct usb_hid_state input;
    uint16_t period, phase;
    uint8_t smask, cmask;
    uint32_t start_ns, complete_ns, tt_ns;
    uint8_t protocol, interface_number;
    bool stalled;
};
struct ehci_controller {
    struct ehci_controller *next;
    struct pci_device pci;
    volatile uint8_t *mapping, *operational;
    struct ehci_dma *dma;
    uint64_t dma_phys;
    struct usb_host *host;
    struct ehci_device *devices;
    struct ehci_hid hid[EHCI_MAX_HID];
    uint64_t addresses[2], next_hub_scan;
    uint32_t port_work, busy;
    uint8_t port_count;
    bool per_port_power, failed, stopped;
};
struct ehci_timer { uint32_t last, mask; uint64_t ticks; };
/** @brief Start an IRQ-independent hardware deadline, or EOPNOTSUPP without a clock. */
int ehci_timer_start(struct ehci_timer *timer);
/** @brief Accumulate timer wraps and test a bounded microsecond deadline. */
bool ehci_timer_expired(struct ehci_timer *timer, uint64_t microseconds);

/** @brief Read an EHCI operational register through its retained UC mapping. */
uint32_t ehci_read(struct ehci_controller *controller, unsigned offset);
/** @brief Write an EHCI operational register; controller and MMIO stay owned. */
void ehci_write(struct ehci_controller *controller, unsigned offset, uint32_t value);
/** @brief Wait bounded milliseconds without changing the caller's final IF. */
void ehci_delay(unsigned milliseconds);
/** @brief Wait for masked register bits, returning ETIMEDOUT on bounded expiry. */
int ehci_wait(struct ehci_controller *controller, unsigned offset, uint32_t mask,
               uint32_t wanted, unsigned milliseconds);
/** @brief Translate an owned schedule/data byte, rejecting unrelated pointers. */
uint32_t ehci_bus(const struct ehci_controller *controller, const void *pointer);
/** @brief Prepare an inactive descriptor; buffer must fit five mapped pages. */
int ehci_qtd_prepare(struct ehci_controller *controller, struct ehci_qtd *td,
                      uint8_t pid, uint8_t toggle, void *buffer, uint32_t length);
/** @brief Initialize a detached endpoint QH with speed/TT routing and DATA0. */
void ehci_qh_prepare(struct ehci_qh *qh, const struct usb_device *device,
                     uint8_t endpoint, uint16_t packet, bool control);
/** @brief Decode one validated configuration into alternate-zero interfaces. */
int usb_parse_configuration(const uint8_t *bytes, uint32_t length,
                             uint8_t speed, struct usb_config *out);
/** @brief Stop/start a hardware schedule and prove the matching status first. */
int ehci_schedule_enable(struct ehci_controller *controller, bool periodic, bool enabled);
extern const struct usb_device_operations ehci_device_operations;
/** @brief Initialize controller-owned asynchronous/periodic schedules before RUN. */
void ehci_schedule_init(struct ehci_controller *controller);
/** @brief Enumerate initial/hotplug ports while retaining controller gate ownership. */
void ehci_service(void *context);
/** @brief Observe port changes and bounded periodic HID completions in IRQ context. */
void ehci_poll(void *context);
/** @brief Rebuild periodic endpoint links with the hardware schedule fully stopped. */
int ehci_periodic_rebuild(struct ehci_controller *controller);
/** @brief Retire configured devices only after callbacks and hardware have stopped. */
void ehci_devices_release(struct ehci_controller *controller);
/** @brief Issue one standard/class request while the host transaction gate is held. */
int ehci_request(struct usb_device *device, uint8_t type, uint8_t request,
                   uint16_t value, uint16_t index, void *data, uint16_t length, uint32_t *actual);
/** @brief Address/configure a reset device, retaining every successful class binding. */
int ehci_device_attach(struct ehci_controller *controller, struct ehci_device *parent,
                         uint8_t parent_port, uint8_t root_port, uint8_t speed);
/** @brief Mark a device and its descendants removed before further transfers. */
void ehci_device_disconnect(struct ehci_controller *controller, struct ehci_device *device);
/** @brief Retire disconnected class states once their external block users close. */
void ehci_devices_collect(struct ehci_controller *controller);
/** @brief Read hub descriptor, power ports and enumerate their attached devices. */
int ehci_hub_initialize(struct ehci_controller *controller, struct ehci_device *hub);
/** @brief Poll connected hubs for hotplug changes in waitable task context. */
void ehci_hubs_scan(struct ehci_controller *controller);
/** @brief Reserve bounded HS and single-TT periodic time without mutating state. */
int ehci_bandwidth_admit(const struct ehci_controller *controller,
                         const struct usb_device *device, const struct usb_endpoint *endpoint,
                         struct ehci_bandwidth *result);
/** @brief Bind one boot HID interrupt endpoint to a periodic DMA slot. */
int ehci_hid_add(struct ehci_controller *controller, struct ehci_device *device,
                   const struct usb_config_interface *interface);
/** @brief Deliver a bounded batch of already-completed HID reports. */
void ehci_hid_poll(struct ehci_controller *controller);
/** @brief Recover stalled interrupt endpoints from task context. */
void ehci_hid_service(struct ehci_controller *controller);
/** @brief Remove all retired HID queue heads after stopping periodic hardware. */
int ehci_hid_collect(struct ehci_controller *controller);
#endif
