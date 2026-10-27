#ifndef RELIEFNT_KERNEL_DEVICE_DRIVER_MANAGER_C_INTERNAL_H
#define RELIEFNT_KERNEL_DEVICE_DRIVER_MANAGER_C_INTERNAL_H

/*
 * ReliefOS kernel driver manager: owns the built-in driver lifecycle.
 * Provides registration, probing, initialization, and device event dispatch.
 * Native module records own descriptors and services through safe teardown.
 */
#include <reliefnt/console.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/e1000.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/input.h>
#include <reliefnt/mm.h>
#include <reliefnt/net.h>
#include <reliefnt/pci.h>
#include <reliefnt/pci_irq.h>
#include <reliefnt/driver_resources.h>
#include <reliefnt/audio.h>
#include <reliefnt/driver_manager_phase.h>
#include <reliefnt/lock.h>
#include <reliefnt/panic.h>
#include <reliefnt/time.h>

#include "../../arch/x86_64/port.h"

#define EARLY_SERIAL_COM1 0x3f8u
#define EARLY_SERIAL_LSR  (EARLY_SERIAL_COM1 + 5u)

/* Kernel diagnostics must be available before the serial driver registers.
 * Keep a tiny COM1 backend here; the driver later replaces it through
 * register_serial(), without changing the public console API. */








struct driver_slot {
    struct reliefos_driver_info info;
    const struct reliefos_driver_module *module;
    int cleanup_error;
    struct module *code_owner;
    uint32_t audio_lease_token;
    struct module *audio_lease_module;
};


extern int early_serial_ready;
extern struct driver_slot driver_slots[RELIEFOS_DRIVER_MAX];
extern int32_t loading_slot;
extern int32_t cleanup_slot;
extern const struct reliefos_driver_mouse_ops *mouse_ops;
extern const struct reliefos_driver_serial_ops *serial_ops;
extern const struct reliefos_driver_e1000_ops *e1000_ops;
extern const struct reliefos_driver_audio_ops *audio_ops;
extern uint32_t mouse_owner;
extern uint32_t serial_owner;
extern uint32_t e1000_owner;
extern uint32_t audio_owner;
extern uint32_t audio_generation, audio_lease_generation;
extern struct mouse_state mouse_cache;
extern struct kernel_spinlock manager_service_lock;

/** @brief Borrow the immutable driver services used by builtin and native modules.
 * @return Permanent kernel-owned service table; no locks or callbacks.
 */
const struct reliefos_driver_kernel_api *driver_kernel_services(void);

void early_serial_write(const char *text);

/**
 * @brief Bypass driver registration and locks when reporting a fatal failure.
 * @param text Bytes to send through the already initialized COM1 UART.
 * @param length Number of bytes, independent of NUL termination.
 */
void serial_write_emergency(const char *text, size_t length);

/**
 * @brief Zero size bytes starting at address.
 */
void driver_memzero(void *address, uint64_t size);

/**
 * @brief Copy size bytes from src to dst.
 */
void driver_memcpy(void *dst, const void *src, uint64_t size);

/**
 * @brief Bind the currently-loading driver's mouse ops as the active mouse backend.
 */
int driver_register_mouse(const struct reliefos_driver_mouse_ops *ops);

/**
 * @brief Bind the currently-loading driver's serial ops as the active serial backend.
 */
int driver_register_serial(const struct reliefos_driver_serial_ops *ops);

/**
 * @brief Bind the currently-loading driver's e1000 ops as the active network backend.
 */
int driver_register_e1000(const struct reliefos_driver_e1000_ops *ops);

/**
 * @brief Bind the loading driver's v1 backend with a non-reused generation.
 * @param ops Borrowed validated callbacks, owned by the loaded image.
 * @return Zero, -EINVAL or -ENOSPC. Task init under manager admission.
 */
int driver_register_audio(const struct reliefos_driver_audio_ops *ops);

/**
 * @brief Capture and pin one published audio callback table with its matching owner.
 * @param generation Exact OSS lease generation, or zero for native compatibility.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
const struct reliefos_driver_audio_ops *driver_audio_service_pin_bound(uint32_t generation,
                                                                             uint32_t *out_owner);

/**
 * @brief Read a byte from an I/O port (exposed to drivers through the ABI).
 */
uint8_t driver_api_inb(uint16_t port);

/**
 * @brief Write a byte to an I/O port (exposed to drivers through the ABI).
 */
void driver_api_outb(uint16_t port, uint8_t value);

/**
 * @brief Read a 32-bit value from an I/O port (exposed to drivers through the ABI).
 */
uint32_t driver_api_inl(uint16_t port);

/**
 * @brief Write a 32-bit value to an I/O port (exposed to drivers through the ABI).
 */
void driver_api_outl(uint16_t port, uint32_t value);

/**
 * @brief Report the framebuffer's width and height, or -2 when no framebuffer is available.
 */
int driver_api_framebuffer_size(uint32_t *width, uint32_t *height);

/**
 * @brief Find a PCI device by vendor/device id and copy its identity into the driver-ABI struct.
 */
int driver_api_pci_find(uint16_t vendor_id, uint16_t device_id,
                               struct reliefos_driver_pci_device *out);

/** @brief Allocate a DMA lease for the loading module.
 * @param pages Requested pages.
 * @param mask Inclusive final-byte DMA mask.
 * @return Owned physical run or zero. Init/task context; core records owner.
 */
uint64_t driver_alloc_dma(uint32_t pages,uint64_t mask);

/** @brief Map a BAR lease for the loading module.
 * @param phys Aligned BAR base of a quiesced function.
 * @param bytes Requested byte length.
 * @return Owned mapping pointer or NULL. Init/task context, execution transaction.
 */
void *driver_map_mmio(uint64_t phys,uint64_t bytes);

/** @brief Register an IRQ lease for the loading module.
 * @param dev Borrowed function identity.
 * @param handler IRQ-safe W1C callback, never sleeping.
 * @param opaque Module-owned context retained through synchronization.
 * @param out_handle Receives generation token.
 * @return 0 or negative errno. Init/task context; no callback under ledger lock.
 */
int driver_request_pci_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *),void *opaque,uint32_t *out_handle);

/** @brief Copy a v2 card contract and bind the currently loading module owner.
 * @param identity Borrowed identity, copied by core.
 * @param ops Borrowed operations, copied by core.
 * @param opaque Context remains module owned until unload synchronization.
 * @param out_id Receives generation card token.
 * @return 0 or negative errno. Task context during init; no registry lock held.
 */
int driver_audio_register_card(const struct audio_card_identity *identity,
    const struct audio_card_ops *ops,void *opaque,uint32_t *out_id);

/** @brief Record the first unsafe teardown result in the serialized fini phase.
 * @param error Negative errno; nonnegative or out-of-phase reports are ignored.
 * @return None. No resource is released and no lock or wait is performed.
 */
void driver_report_teardown_failure(int error);

/**
 * @brief Clear all driver slots and the cached mouse state at startup.
 */
void driver_manager_init(void);

/**
 * @brief Initialize every driver linked into the kernel image.
 * @return None. Boot has no outer execution transaction, so this hook owns a
 * short manager transaction and suspends it only around waitable module phases.
 */
void driver_manager_load_builtin(void);

/**
 * @brief Fill the query with up to capacity driver infos and set the total count.
 */
int driver_manager_list(struct reliefos_driver_list *query);

/**
 * @brief Reject runtime driver control actions; every driver is built into the
 * kernel image, so there is no module to load, unload or disable at runtime.
 */
int driver_manager_control(struct reliefos_driver_control *request);

/**
 * @brief Reserved startup hook; mouse state arrives through the registered driver.
 */
void mouse_init(void);

/**
 * @brief Poll the active mouse driver for new events, if one is registered.
 * @return None. IRQ dispatch may call this wrapper; it pins a consistent local
 * ops/owner snapshot across the callback without holding service locks. The
 * legacy poll callback must remain bounded and IRQ-safe.
 */
void mouse_poll(void);

/**
 * @brief Refresh the manager-owned mouse cache from a pinned driver snapshot.
 * @return Pointer to manager-owned cached state, never into the module image.
 */
const struct mouse_state *mouse_get_state(void);

/**
 * @brief Record whether the mouse cursor should be drawn.
 */
void mouse_set_visible(bool visible);

/**
 * @brief Return the cached cursor visibility flag.
 */
bool mouse_is_visible(void);

/**
 * @brief Report the driver's pending-event counter, or 0 when no driver is registered.
 * @return Copied counter value; the matching ops/owner pin spans get_state.
 */
uint32_t mouse_event_count(void);

/**
 * @brief Report the driver's last PS/2 status byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_status(void);

/**
 * @brief Report the driver's last PS/2 data byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_data(void);

/**
 * @brief Report the driver's last PS/2 ack byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_ack(void);

/**
 * @brief Copy the current cached mouse state into out.
 */
void driver_manager_mouse_state(struct mouse_state *out);

/**
 * @brief Forward mouse_event_count to callers outside this file.
 */
uint32_t driver_manager_mouse_event_count(void);

/**
 * @brief Forward mouse_last_status to callers outside this file.
 */
uint8_t driver_manager_mouse_last_status(void);

/**
 * @brief Forward mouse_last_data to callers outside this file.
 */
uint8_t driver_manager_mouse_last_data(void);

/**
 * @brief Forward mouse_last_ack to callers outside this file.
 */
uint8_t driver_manager_mouse_last_ack(void);

/**
 * @brief Forward mouse_poll to callers outside this file.
 */
void driver_manager_mouse_poll(void);

/**
 * @brief Program the COM1 UART (baud, frame format, FIFO, IRQ) and enable the early serial path.
 */
void serial_init(void);

/**
 * @brief Return 1 when a serial driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; one consistent ops/owner pin
 * spans is_ready, with no manager lock held during the callback.
 */
int serial_is_ready(void);

/**
 * @brief Write text through the serial driver, or the built-in COM1 path before one loads.
 * @param text Kernel-owned NUL-terminated text borrowed through callback return.
 * @return None. The matching local ops/owner snapshot stays pinned across write.
 */
void serial_write(const char *text);

/**
 * @brief Reserved startup hook; the NIC is brought up by its driver's init().
 */
void e1000_init(void);

/**
 * @brief Return 1 when an e1000 driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; the matching ops/owner stays
 * pinned until is_ready returns.
 */
int e1000_is_ready(void);

/**
 * @brief Return a kernel-owned copy of the NIC MAC, or a zeroed placeholder when absent.
 * @return Manager cache pointer; the module's borrowed pointer is copied while its
 * matching ops/owner snapshot remains pinned.
 */
const uint8_t *e1000_mac(void);

/**
 * @brief Send an Ethernet frame through the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned frame borrowed through send callback return.
 * @param len Number of bytes available at frame.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and send.
 */
int e1000_send(const void *frame, uint32_t len);

/**
 * @brief Poll received frames from the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned receive buffer.
 * @param capacity Bytes available at frame.
 * @param out_len Receives the copied frame length when supplied by the driver.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and poll.
 */
int e1000_poll(void *frame, uint32_t capacity, uint32_t *out_len);

/**
 * @brief Copy NIC identity independently of carrier state.
 * @param info Receives kernel-owned identity fields and MAC bytes.
 * @return None. Driver output is copied to a stack snapshot before unpin.
 */
void e1000_get_info(struct e1000_info *info);

/**
 * @brief Configure the audio output format on the active driver, or -19 when absent.
 * @param format Kernel-owned format borrowed through configure callback return.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and configure.
 */
int driver_manager_audio_configure(const struct reliefos_audio_format *format);

/** @brief Configure one generation-pinned v1 audio callback table.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param format Borrowed sample format.
 * @return Driver result or -ENODEV. Task context; no manager lock spans callback.
 */
int driver_manager_audio_configure_bound(uint32_t generation, const struct reliefos_audio_format *format);

/**
 * @brief Write audio samples; returns -19 and a NO_DEVICE status when no driver is ready.
 * @param data Kernel-owned sample bytes borrowed through write callback return.
 * @param length Number of valid sample bytes.
 * @param out_status Optional kernel-owned status destination.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and write.
 */
long driver_manager_audio_write(const void *data, uint32_t length,
                                uint32_t *out_status);

/** @brief Write bytes through the exact acquired backend generation.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param data Borrowed samples. @param length Byte count.
 * @param out_status Optional status output.
 * @return Written bytes or errno. Task context; no manager lock spans callback.
 */
long driver_manager_audio_write_bound(uint32_t generation, const void *data, uint32_t length,
                                      uint32_t *out_status);

/**
 * @brief Copy the audio driver's state into out, zeroing it when no driver is ready.
 * @param out Optional kernel-owned destination.
 * @return None. A consistent local ops/owner pin stays live through get_state.
 */
void driver_manager_audio_get_state(struct reliefos_audio_state *out);

/** @brief Copy state while holding the exact backend's callback pin.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param out Borrowed output, cleared before attempting admission.
 * @return Zero or -ENODEV/-EINVAL. Task context; callback has no manager lock.
 */
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *out);

/** @brief Reserve one OSS description on the current published v1 backend.
 * @param generation Receives a non-reused token on success.
 * @param state Receives real hardware state under a callback pin.
 * @return Zero, -EINVAL, -ENODEV or -EBUSY. Task context; no lock spans callback.
 */
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state);

/** @brief Release only the lease generation held by the closing OSS description.
 * @param generation Nonzero acquired token, possibly stale after unload.
 * @return None. Task context; no callback or hardware access.
 */
void driver_manager_audio_release(uint32_t generation);

#endif
