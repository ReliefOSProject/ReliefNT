/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_XHCI_INTERNAL_H
#define RELIEFNT_XHCI_INTERNAL_H

#include <reliefnt/types.h>
#include <reliefnt/usb_device.h>
#include <reliefnt/usb_descriptor.h>
#include <reliefnt/usb_hid.h>
#include <reliefnt/usb.h>
#include <reliefnt/pci.h>
#include <reliefnt/lock.h>
#include <reliefnt/driver_module.h>
#include <linux/errno.h>

#define XHCI_TD_MAX_TRBS 8u
#define XHCI_TRB_CYCLE 1u
#define XHCI_TRB_CHAIN (1u << 4)
#define XHCI_TRB_TYPE(t) ((uint32_t)(t) << 10)
#define XHCI_TRB_LINK 6u
#define XHCI_EVENT_TRANSFER 32u
#define XHCI_EVENT_COMMAND 33u
#define XHCI_MAX_PORTS 255u
#define XHCI_ENDPOINTS 32u
#define XHCI_RING_COUNT 64u
#define XHCI_EVENT_COUNT 256u
#define XHCI_BOUNCE_BYTES 65536u

struct xhci_trb { uint32_t lo, hi, status, control; };
struct xhci_ring {
    volatile struct xhci_trb *trbs;
    uint64_t dma;
    uint32_t count, producer, consumer, used;
    uint8_t cycle;
    bool quarantined;
};
struct xhci_pending {
    uint64_t trbs[XHCI_TD_MAX_TRBS];
    uint32_t trb_lengths[XHCI_TD_MAX_TRBS];
    uint32_t length;
    uint8_t count, event_type, slot, endpoint;
};
struct xhci_completion {
    struct xhci_pending pending;
    uint32_t actual;
    int result;
    bool control, input, short_seen, done, recovery;
};
struct xhci_protocol {
    uint16_t valid_speeds;
    uint8_t major, minor, slot_type, speeds[16], psi_count;
    uint64_t rates[16];
    /* Exact capability dwords, in hardware order; no dwords for PSIC=0. */
    uint32_t psi[15];
};
struct xhci_stop_state { bool failed, stopped, registered; };
enum xhci_port_rejection {
    XHCI_REJECT_NONE, XHCI_REJECT_SPEED, XHCI_REJECT_HUB,
    XHCI_REJECT_CONFIG, XHCI_REJECT_INTERFACE,
};
enum xhci_port_phase { XHCI_PORT_READY, XHCI_PORT_RETRY, XHCI_PORT_REJECTED, XHCI_PORT_EXHAUSTED };
struct xhci_port_state { uint64_t failed_at; uint8_t phase, attempts; };
struct xhci_stop_ops {
    int (*unregister)(void *);
    void (*halt_request)(void *);
    int (*wait_halt)(void *);
    int (*disable_master)(void *);
};
struct xhci_dma {
    uint64_t physical;
    uint32_t pages;
    void *cpu;
};
struct xhci_controller;
struct xhci_device;
struct xhci_endpoint_state {
    struct xhci_dma dma, replacement;
    struct xhci_ring ring;
    struct xhci_completion completion;
    struct usb_endpoint endpoint;
    struct usb_hid_state hid_state;
    uint8_t hid_protocol, interface_number;
    bool active, configured, hid, needs_recovery;
};
struct xhci_device {
    struct usb_device usb;
    struct xhci_device *next;
    struct xhci_controller *controller;
    struct xhci_dma input, output;
    struct xhci_endpoint_state endpoints[XHCI_ENDPOINTS];
    struct usb_storage *storage[USB_MAX_INTERFACES];
    uint8_t slot_id, speed_id, storage_count;
    bool disabled;
};
struct xhci_controller {
    struct xhci_controller *next;
    struct pci_device pci;
    uint64_t bar_phys, dma_mask;
    volatile uint8_t *mapping;
    uint32_t mapping_bytes, operational, runtime, doorbells;
    uint8_t ports, slots, stride, event_cycle;
    uint32_t event_index, busy, port_work[8], collect_ticks;
    struct xhci_protocol protocol[XHCI_MAX_PORTS];
    struct xhci_port_state port_state[XHCI_MAX_PORTS];
    struct xhci_dma dcbaa, commands, events, erst, bounce, scratch_array, scratch_pages;
    struct xhci_ring command_ring;
    struct xhci_pending command_pending;
    struct xhci_trb command_event;
    bool command_active, command_done, owned, per_port_power, collect_pending, failure_notified;
    struct kernel_spinlock event_lock;
    struct xhci_stop_state stop;
    struct usb_host *host;
    struct xhci_device *devices;
};
struct xhci_timer { uint32_t last, mask; uint64_t ticks; };
/* Validated mandatory register ranges, all relative to BAR0 and half-open. */
struct xhci_geometry {
    uint32_t operational, port_end, doorbells, doorbell_end, runtime, runtime_end, limit;
};

int xhci_ring_init(struct xhci_ring *ring, volatile struct xhci_trb *trbs,
                   uint64_t dma, uint32_t count);
int xhci_ring_submit(struct xhci_ring *ring, const struct xhci_trb *trbs,
                     unsigned count, struct xhci_pending *pending);
int xhci_ring_reclaim(struct xhci_ring *ring, unsigned count, bool stopped);
void xhci_ring_quarantine(struct xhci_ring *ring);
bool xhci_event_match(const struct xhci_pending *pending, const struct xhci_trb *event);
bool xhci_complete_event(struct xhci_completion *completion, const struct xhci_trb *event);
int xhci_build_control(const uint8_t *setup, uint64_t data_dma, uint16_t length,
                       struct xhci_trb *trbs, unsigned *count);
int xhci_build_normal(uint64_t data_dma, uint32_t length, bool input, struct xhci_trb *trb);
uint32_t xhci_boundary_chunk(uint64_t address, uint32_t remaining);
int xhci_context_address(uint32_t *context, uint32_t bytes, unsigned stride,
                         uint8_t speed_id, uint8_t root_port, uint16_t packet,
                         uint64_t endpoint_ring);
int xhci_context_endpoint(uint32_t *context, uint32_t bytes, unsigned stride,
                          uint8_t speed, const struct usb_endpoint *endpoint,
                          bool interrupt, uint64_t endpoint_ring);
int xhci_scratchpad_layout(uint64_t *dcbaa, uint64_t *array, uint32_t count,
                           uint64_t array_dma, uint64_t pages_dma, bool ac64);
int xhci_protocol_parse(const volatile uint32_t *registers, uint32_t bytes,
                        uint32_t first, uint8_t max_ports, struct xhci_protocol *ports);
typedef int (*xhci_firmware_reader)(void *context, uint32_t offset, uint32_t *value);
typedef int (*xhci_firmware_writer8)(void *context, uint32_t offset, uint8_t value);
typedef int (*xhci_firmware_writer32)(void *context, uint32_t offset, uint32_t value);
int xhci_firmware_minimum(uint8_t cap_length, uint32_t hcs, uint32_t *limit);
int xhci_firmware_extent(xhci_firmware_reader read, void *context,
                         const struct xhci_geometry *geometry, uint32_t hcc,
                         uint32_t *legacy, uint32_t *extent);
int xhci_firmware_handoff(xhci_firmware_reader read, xhci_firmware_writer8 write8,
                          xhci_firmware_writer32 write32, void *context,
                          uint32_t legacy, bool *acquired);
int xhci_quiesce(struct xhci_stop_state *state, const struct xhci_stop_ops *ops,
                 void *context);
int xhci_classify_interface(const struct usb_config_interface *interface);
int xhci_ep0_packet(uint8_t speed, uint8_t descriptor_value, uint16_t *packet);
int xhci_timer_start(struct xhci_timer *timer);
bool xhci_timer_expired(struct xhci_timer *timer, uint64_t microseconds);
void xhci_delay(unsigned milliseconds);
uint32_t xhci_read(struct xhci_controller *controller, uint32_t offset);
int xhci_mmio_address(volatile uint8_t *mapping, uint32_t offset, uint64_t value, bool ac64);
int xhci_geometry_init(uint32_t cap_length, uint32_t hcs, uint32_t doorbells,
                       uint32_t runtime, uint64_t physical, struct xhci_geometry *geometry);
int xhci_mmio_layout(const struct xhci_geometry *geometry, uint32_t cap_extent, uint32_t *bytes);
void xhci_write(struct xhci_controller *controller, uint32_t offset, uint32_t value);
void xhci_write64(struct xhci_controller *controller, uint32_t offset, uint64_t value);
int xhci_wait(struct xhci_controller *controller, uint32_t offset, uint32_t mask,
              uint32_t value, unsigned milliseconds);
int xhci_dma_alloc(struct xhci_controller *controller, struct xhci_dma *dma, uint32_t pages);
void xhci_dma_release(struct xhci_dma *dma);
int xhci_start_rings(struct xhci_controller *controller,uint16_t pci_command);
void xhci_events(struct xhci_controller *controller);
int xhci_command(struct xhci_controller *controller, struct xhci_trb trb, uint8_t *slot);
void xhci_poll(void *context);
void xhci_service(void *context);
int xhci_device_attach(struct xhci_controller *controller, uint8_t root_port, uint8_t speed_id,
                       enum xhci_port_rejection *rejection);
void xhci_device_disconnect(struct xhci_device *device);
void xhci_devices_collect(struct xhci_controller *controller);
void xhci_devices_release(struct xhci_controller *controller);
void xhci_hid_event(struct xhci_device *device, unsigned dci);
int xhci_hid_arm(struct xhci_device *device, unsigned dci);
int xhci_endpoint_recover(struct xhci_device *device, unsigned dci, bool halted);
extern const struct usb_device_operations xhci_device_operations;
#endif
