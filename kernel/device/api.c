#include <reliefnt/sound_service.h>
#include "manager_internal.h"



/**
 * @brief Read a byte from an I/O port (exposed to drivers through the ABI).
 */
uint8_t driver_api_inb(uint16_t port)
{
    return x86_64_inb(port);
}

/**
 * @brief Write a byte to an I/O port (exposed to drivers through the ABI).
 */
void driver_api_outb(uint16_t port, uint8_t value)
{
    x86_64_outb(value, port);
}

/**
 * @brief Read a 32-bit value from an I/O port (exposed to drivers through the ABI).
 */
uint32_t driver_api_inl(uint16_t port)
{
    return x86_64_inl(port);
}

/**
 * @brief Write a 32-bit value to an I/O port (exposed to drivers through the ABI).
 */
void driver_api_outl(uint16_t port, uint32_t value)
{
    x86_64_outl(value, port);
}

/**
 * @brief Report the framebuffer's width and height, or -2 when no framebuffer is available.
 */
int driver_api_framebuffer_size(uint32_t *width, uint32_t *height)
{
    const struct framebuffer *framebuffer = framebuffer_get();
    if (!framebuffer || !framebuffer->available) {
        return -2;
    }
    if (width) {
        *width = framebuffer->width;
    }
    if (height) {
        *height = framebuffer->height;
    }
    return 0;
}

/**
 * @brief Find a PCI device by vendor/device id and copy its identity into the driver-ABI struct.
 */
int driver_api_pci_find(uint16_t vendor_id, uint16_t device_id,
                               struct reliefos_driver_pci_device *out)
{
    struct pci_device device;
    int ret = pci_find_device(vendor_id, device_id, &device);
    if (ret < 0 || !out) {
        return ret < 0 ? ret : -22;
    }
    *out = (struct reliefos_driver_pci_device){
        .bus = device.bus,
        .slot = device.slot,
        .function = device.function,
        .class_code = device.class_code,
        .vendor_id = device.vendor_id,
        .device_id = device.device_id,
        .subclass = device.subclass,
        .prog_if = device.prog_if,
        .header_type = 0,
        .reserved = 0,
    };
    return 0;
}

/** @brief Allocate a DMA lease for the loading module.
 * @param pages Requested pages.
 * @param mask Inclusive final-byte DMA mask.
 * @return Owned physical run or zero. Init/task context; core records owner.
 */
uint64_t driver_alloc_dma(uint32_t pages,uint64_t mask)
{
    return driver_alloc_dma_owned((uint32_t)loading_slot,pages,mask);
}

/** @brief Map a BAR lease for the loading module.
 * @param phys Aligned BAR base of a quiesced function.
 * @param bytes Requested byte length.
 * @return Owned mapping pointer or NULL. Init/task context, execution transaction.
 */
void *driver_map_mmio(uint64_t phys,uint64_t bytes)
{
    return driver_map_mmio_owned((uint32_t)loading_slot,phys,bytes);
}

/** @brief Register an IRQ lease for the loading module.
 * @param dev Borrowed function identity.
 * @param handler IRQ-safe W1C callback, never sleeping.
 * @param opaque Module-owned context retained through synchronization.
 * @param out_handle Receives generation token.
 * @return 0 or negative errno. Init/task context; no callback under ledger lock.
 */
int driver_request_pci_irq(const struct reliefos_driver_pci_device *dev,
    void (*handler)(void *),void *opaque,uint32_t *out_handle)
{
    return driver_request_pci_irq_owned((uint32_t)loading_slot,dev,handler,opaque,out_handle);
}

/** @brief Copy a v2 card contract and bind the currently loading module owner.
 * @param identity Borrowed identity, copied by core.
 * @param ops Borrowed operations, copied by core.
 * @param opaque Context remains module owned until unload synchronization.
 * @param out_id Receives generation card token.
 * @return 0 or negative errno. Task context during init; no registry lock held.
 */
int driver_audio_register_card(const struct audio_card_identity *identity,
    const struct audio_card_ops *ops,void *opaque,uint32_t *out_id)
{
    if(loading_slot<0)return -22;
    return sound_service_register_card(identity,ops,opaque,(uint32_t)loading_slot,out_id);
}

/** @brief Record the first unsafe teardown result in the serialized fini phase.
 * @param error Negative errno; nonnegative or out-of-phase reports are ignored.
 * @return None. No resource is released and no lock or wait is performed.
 */
void driver_report_teardown_failure(int error)
{
    if (error < 0 && cleanup_slot >= 0 &&
        cleanup_slot < (int32_t)RELIEFOS_DRIVER_MAX &&
        !driver_slots[cleanup_slot].cleanup_error)
        driver_slots[cleanup_slot].cleanup_error = error;
}

static const struct reliefos_driver_kernel_api driver_kernel_api = {
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_kernel_api),
    .inb = driver_api_inb,
    .outb = driver_api_outb,
    .inl = driver_api_inl,
    .outl = driver_api_outl,
    .alloc_pages = mm_alloc_pages,
    .free_pages = mm_free_pages,
    .console_write = console_log_write,
    .input_push_mouse = input_push_mouse,
    .input_push_mouse_wheel = input_push_mouse_wheel,
    .framebuffer_size = driver_api_framebuffer_size,
    .pci_find = driver_api_pci_find,
    .pci_read16 = pci_config_read16,
    .pci_write16 = pci_config_write16,
    .pci_read32 = pci_config_read32,
    .ticks = time_ticks,
    .sleep_ms = time_sleep_ms,
    .register_mouse = driver_register_mouse,
    .register_serial = driver_register_serial,
    .register_e1000 = driver_register_e1000,
    .register_audio = driver_register_audio,
    .pci_enumerate = pci_enumerate,
    .map_mmio = driver_map_mmio,
    .unmap_mmio = driver_unmap_mmio,
    .alloc_dma = driver_alloc_dma,
    .free_dma = driver_free_dma,
    .request_pci_irq = driver_request_pci_irq,
    .free_pci_irq = driver_free_pci_irq,
    .audio_register_card = driver_audio_register_card,
    .audio_unregister_card = sound_service_unregister_card,
    .audio_period_elapsed = sound_service_period_elapsed,
    .audio_control_changed = sound_service_control_changed,
    .pci_write32 = pci_config_write32,
    .audio_set_service = sound_service_set_service,
    .report_teardown_failure = driver_report_teardown_failure,
    .audio_request_disconnect = sound_service_request_disconnect,
    .console_log = console_log,
    .phys_to_virt = mm_phys_to_virt,
};

/** @brief Borrow the immutable native driver services for one retained descriptor.
 * @return Permanent kernel-owned service table; no allocation, locking or callbacks.
 */
const struct reliefos_driver_kernel_api *driver_kernel_services(void)
{
    return &driver_kernel_api;
}
