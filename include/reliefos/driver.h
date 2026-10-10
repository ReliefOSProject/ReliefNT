#ifndef RELIEFOS_DRIVER_H
#define RELIEFOS_DRIVER_H

/*
 * Built-in driver (Ring-0) API plus the userland driver-control client
 * API. The control wire types and constants moved to the kernel UAPI
 * (<reliefos/driver_abi.h>); this header re-exports them so existing callers
 * keep working.
 */
#include <stdint.h>
#include <stddef.h>
#include <reliefos/driver_audio.h>
#include <reliefos/audio_abi.h>
#include <reliefos/driver_abi.h>

#define RELIEFOS_DRIVER_ABI_VERSION 1U
#define RELIEFOS_DRIVER_MODULE_MAGIC 0x4c445256U
#define RELIEFOS_DRIVER_MAX 16U


#define RELIEFOS_DRIVER_KIND_INPUT 1U
#define RELIEFOS_DRIVER_KIND_SERIAL 2U
#define RELIEFOS_DRIVER_KIND_NETWORK 3U
#define RELIEFOS_DRIVER_KIND_AUDIO 4U

struct reliefos_driver_mouse_state {
    int32_t x;
    int32_t y;
    uint8_t buttons;
    uint8_t present;
    uint8_t absolute;
    uint8_t reserved;
    uint32_t event_count;
    uint8_t last_status;
    uint8_t last_data;
    uint8_t last_ack;
    uint8_t reserved2;
};

struct reliefos_driver_pci_device {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t class_code;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
    uint8_t reserved;
};

struct reliefos_driver_e1000_info {
    uint32_t present;
    uint32_t active;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t reserved;
    uint8_t mac[6];
    uint8_t reserved2[2];
};

struct reliefos_driver_mouse_ops {
    void (*poll)(void);
    void (*get_state)(struct reliefos_driver_mouse_state *out);
};

struct reliefos_driver_serial_ops {
    int (*is_ready)(void);
    void (*write)(const char *text);
};

struct reliefos_driver_e1000_ops {
    int (*is_ready)(void);
    const uint8_t *(*mac)(void);
    int (*send)(const void *frame, uint32_t len);
    int (*poll)(void *frame, uint32_t capacity, uint32_t *out_len);
    void (*get_info)(struct reliefos_driver_e1000_info *out);
};

struct reliefos_driver_audio_ops {
    int (*is_ready)(void);
    int (*configure)(const struct reliefos_audio_format *format);
    long (*write)(const void *data, uint32_t length, uint32_t *out_status);
    void (*get_state)(struct reliefos_audio_state *out);
};

struct reliefos_driver_kernel_api {
    uint32_t abi_version;
    uint32_t struct_size;
    uint8_t (*inb)(uint16_t port);
    void (*outb)(uint16_t port, uint8_t value);
    uint32_t (*inl)(uint16_t port);
    void (*outl)(uint16_t port, uint32_t value);
    uint64_t (*alloc_pages)(uint32_t page_count);
    void (*free_pages)(uint64_t address, uint32_t page_count);
    void (*console_write)(const char *text);
    void (*input_push_mouse)(int32_t x, int32_t y, int32_t dx, int32_t dy,
                             uint8_t buttons);
    void (*input_push_mouse_wheel)(int32_t x, int32_t y, int32_t wheel,
                                   uint8_t buttons);
    int (*framebuffer_size)(uint32_t *width, uint32_t *height);
    int (*pci_find)(uint16_t vendor_id, uint16_t device_id,
                    struct reliefos_driver_pci_device *out);
    uint16_t (*pci_read16)(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
    void (*pci_write16)(uint8_t bus, uint8_t slot, uint8_t function,
                        uint8_t offset, uint16_t value);
    uint32_t (*pci_read32)(uint8_t bus, uint8_t slot, uint8_t function,
                           uint8_t offset);
    uint64_t (*ticks)(void);
    void (*sleep_ms)(uint64_t ms);
    int (*register_mouse)(const struct reliefos_driver_mouse_ops *ops);
    int (*register_serial)(const struct reliefos_driver_serial_ops *ops);
    int (*register_e1000)(const struct reliefos_driver_e1000_ops *ops);
    int (*register_audio)(const struct reliefos_driver_audio_ops *ops);
    /* v1 ends here (168 bytes on x86_64). Extensions are size gated. */
    /** @brief pci enumerate service.
     * @param index Stable ordinal of present functions.
     * @param out Receives copied PCI identity.
     * @return 0 or -ENODEV/-EINVAL; task context, no ownership.
     */
    int (*pci_enumerate)(uint32_t index, struct reliefos_driver_pci_device *out);
    /** @brief map mmio service.
     * @param phys Aligned memory BAR base, quiesced by driver.
     * @param bytes Requested bytes; whole covering pages must lie within BAR.
     * @return Owned UC/NX pointer or NULL; init task context, with a short execution
     * transaction around page-attribute changes while manager admission stays held.
     */
    void *(*map_mmio)(uint64_t phys, uint64_t bytes);
    /** @brief unmap mmio service.
     * @param ptr Owned matching MMIO lease.
     * @param bytes Original requested bytes.
     * @return None; task context, stopped device, final attributes restored and TLB synchronized.
     */
    void (*unmap_mmio)(void *ptr, uint64_t bytes);
    /** @brief alloc dma service.
     * @param pages Nonzero 4 KiB page count.
     * @param mask Maximum inclusive final bus byte.
     * @return Owned zeroed physical run or 0 (driver reports ENOMEM); init task context.
     */
    uint64_t (*alloc_dma)(uint32_t pages, uint64_t mask);
    /** @brief free dma service.
     * @param phys Owned physical base.
     * @param pages Original page count.
     * @return None; task context after DMA stopped; ignores unmatched/duplicate lease.
     */
    void (*free_dma)(uint64_t phys, uint32_t pages);
    /** @brief request pci irq service.
     * @param dev Borrowed quiesced PCI identity.
     * @param handler IRQ-safe nonblocking W1C callback, acknowledged before EOI.
     * @param opaque Module context alive until free returns.
     * @param out_handle Receives owned generation token.
     * @return 0 or negative errno; init task context; MSI only, INTx returns ENOTSUP.
     */
    int (*request_pci_irq)(const struct reliefos_driver_pci_device *dev,
                          void (*handler)(void *), void *opaque, uint32_t *out_handle);
    /** @brief free pci irq service.
     * @param handle Owned MSI generation token.
     * @return None; task context, never in handler; disables MSI then synchronizes ISR before reuse.
     */
    void (*free_pci_irq)(uint32_t handle);
    /** @brief audio register card service.
     * @param identity Borrowed metadata copied by core.
     * @param ops Borrowed size/version-checked table copied by core.
     * @param opaque Live module context, held until unregister completes.
     * @param out_id Receives card generation token.
     * @return 0 or negative errno; init task context, owner bound to loading module.
     */
    int (*audio_register_card)(const struct audio_card_identity *identity,
                              const struct audio_card_ops *ops, void *opaque, uint32_t *out_id);
    /** @brief audio unregister card service.
     * @param id Owned card generation token.
     * @param force Nonzero disconnects active leases.
     * @return 0 or errno; task context, no callback nesting; STOP failure retains module.
     */
    int (*audio_unregister_card)(uint32_t id, uint32_t force);
    /** @brief audio period elapsed service.
     * @param card Card generation token.
     * @param stream Hardware stream id.
     * @param frames Completed frames.
     * @param error Zero or negative stream error.
     * @return None; IRQ safe, stale generations ignored, no allocation or module callback.
     */
    void (*audio_period_elapsed)(uint32_t card, uint32_t stream, uint64_t frames, int error);
    /** @brief audio control changed service.
     * @param card Card generation token.
     * @param control Hardware control id.
     * @return None; IRQ safe, stale generations ignored, no module callback.
     */
    void (*audio_control_changed)(uint32_t card, uint32_t control);
    /** @brief pci write32 service.
     * @param bus PCI bus.
     * @param slot PCI slot.
     * @param function PCI function.
     * @param offset Aligned config offset.
     * @param value New word.
     * @return None; serialized CF8/CFC, IRQ safe, no ownership.
     */
    void (*pci_write32)(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value);
    /** @brief Register or synchronously cancel a bounded card service.
     * @param card Card generation token.
     * @param callback Nonblocking budget consumer or NULL to cancel.
     * @param opaque Borrowed context pinned through cancellation synchronization.
     * @return 0 or errno; task context; remove before replacing; callbacks never wait for codec responses.
     */
    int (*audio_set_service)(uint32_t card, audio_service_fn callback, void *opaque);
    /** @brief Retain a module whose fini cannot prove hardware DMA has stopped.
     * @param error Negative errno describing the first unsafe teardown failure.
     * @return None. Only a synchronous task-context fini may call this entry;
     * calls outside that phase and nonnegative values are ignored. IRQ is
     * already synchronized. DMA/MMIO/image remain owned until a successful
     * retry of fini; old ABI prefixes and successful cleanup are unchanged.
     */
    void (*report_teardown_failure)(int error);
    /** @brief Queue fatal disconnect for every codec on one owned controller.
     * @param card Live generation token identifying the controller's owner/BDF.
     * @return Zero or -ENODEV. IRQ safe; closes admission and wakes readers,
     * but never waits, calls a module, stops DMA, or frees resources. The
     * manager performs STOP/close later in task context and retains failed
     * STOP leases for retry. Other controller BDFs remain available.
     */
    int (*audio_request_disconnect)(uint32_t card);
};

#define RELIEFOS_DRIVER_AUDIO_API_SIZE \
    (offsetof(struct reliefos_driver_kernel_api, audio_set_service) + \
     sizeof(((struct reliefos_driver_kernel_api *)0)->audio_set_service))
/* A new module must reject api->struct_size < RELIEFOS_DRIVER_AUDIO_API_SIZE. */
#define RELIEFOS_DRIVER_CLEANUP_API_SIZE \
    (offsetof(struct reliefos_driver_kernel_api, report_teardown_failure) + \
     sizeof(((struct reliefos_driver_kernel_api *)0)->report_teardown_failure))
#define RELIEFOS_DRIVER_DISCONNECT_API_SIZE \
    (offsetof(struct reliefos_driver_kernel_api, audio_request_disconnect) + \
     sizeof(((struct reliefos_driver_kernel_api *)0)->audio_request_disconnect))

struct reliefos_driver_module {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t kind;
    char name[RELIEFOS_DRIVER_NAME_LEN];
    uint32_t version;
    uint32_t reserved;
    int (*init)(const struct reliefos_driver_kernel_api *api);
    void (*fini)(void);
};

int reliefos_driver_list(struct reliefos_driver_info *drivers, uint32_t capacity,
                       uint32_t *out_count);
int reliefos_driver_control(uint32_t action, const char *file);

#endif
