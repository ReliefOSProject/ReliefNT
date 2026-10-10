/*
 * ReliefOS kernel driver manager: owns the built-in driver lifecycle.
 * Provides registration, probing, initialization, and device event dispatch.
 * Every driver is linked into kernel.sys; there is no runtime module loader.
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

#include "../arch/x86_64/port.h"

#define EARLY_SERIAL_COM1 0x3f8u
#define EARLY_SERIAL_LSR  (EARLY_SERIAL_COM1 + 5u)

/* Kernel diagnostics must be available before the serial driver registers.
 * Keep a tiny COM1 backend here; the driver later replaces it through
 * register_serial(), without changing the public console API. */
static int early_serial_ready;

static void early_serial_putc(char ch)
{
    uint32_t attempts = 100000u;
    if (!early_serial_ready) {
        return;
    }
    while (!(x86_64_inb((uint16_t)EARLY_SERIAL_LSR) & 0x20u) && attempts--) {
        __asm__ volatile("pause");
    }
    x86_64_outb((uint8_t)ch, (uint16_t)EARLY_SERIAL_COM1);
}

static void early_serial_write(const char *text)
{
    while (text && *text) {
        if (*text == '\n') {
            early_serial_putc('\r');
        }
        early_serial_putc(*text++);
    }
}

/* Descriptors of the drivers linked into kernel.sys, in load order: serial
 * first so console output moves off the early COM1 path, then alphabetical. */
extern const struct reliefos_driver_module serial_driver_module;
extern const struct reliefos_driver_module ac97_driver_module;
extern const struct reliefos_driver_module e1000_driver_module;
extern const struct reliefos_driver_module es1371_driver_module;
extern const struct reliefos_driver_module hda_driver_module;
extern const struct reliefos_driver_module mouse_driver_module;

static const struct reliefos_driver_module *const builtin_modules[] = {
    &serial_driver_module,
    &ac97_driver_module,
    &e1000_driver_module,
    &es1371_driver_module,
    &hda_driver_module,
    &mouse_driver_module,
};

struct driver_slot {
    struct reliefos_driver_info info;
    const struct reliefos_driver_module *module;
    int cleanup_error;
};

static struct driver_slot driver_slots[RELIEFOS_DRIVER_MAX];
static int32_t loading_slot = -1;
static int32_t cleanup_slot = -1;
static const struct reliefos_driver_mouse_ops *mouse_ops;
static bool mouse_visible = true;
static const struct reliefos_driver_serial_ops *serial_ops;
static const struct reliefos_driver_e1000_ops *e1000_ops;
static const struct reliefos_driver_audio_ops *audio_ops;
static uint32_t mouse_owner;
static uint32_t serial_owner;
static uint32_t e1000_owner;
static uint32_t audio_owner;
static uint32_t audio_generation, audio_lease_generation;
static struct mouse_state mouse_cache;
static uint8_t e1000_empty_mac[6];
static uint8_t e1000_mac_cache[6];
static struct kernel_spinlock manager_service_lock = KERNEL_SPINLOCK_INIT;
static struct kernel_spinlock e1000_mac_cache_lock = KERNEL_SPINLOCK_INIT;

/** @brief Resume manager ownership after a phase that may wait or sleep.
 * @param scope Active manager wait scope whose token must be consumed.
 * @return None. A lost transaction is a kernel invariant failure; no slot state
 * or module resources may be reclaimed without the resumed transaction.
 */
static void driver_manager_wait_end_or_panic(struct driver_manager_wait_scope *scope)
{
    if (driver_manager_wait_end(scope) < 0)
        panic("driver manager failed to resume execution ownership");
}

/**
 * @brief Copy src into dst up to cap-1 bytes and always NUL-terminate; safe with NULL src or cap 0.
 */
static void driver_copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t pos = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[pos] && pos + 1U < cap) {
        dst[pos] = src[pos];
        ++pos;
    }
    dst[pos] = 0;
}

/**
 * @brief Mark the slot failed, record the error text, and log it only when status is negative.
 */
static void driver_set_error(struct driver_slot *slot, int status, const char *error)
{
    if (!slot) {
        return;
    }
    slot->info.state = RELIEFOS_DRIVER_STATE_FAILED;
    driver_copy_text(slot->info.error, sizeof(slot->info.error), error);
    if (status < 0) {
        console_printf("[driver] %s failed status=%d: %s\n", slot->info.file,
                       status, slot->info.error);
    }
}

/**
 * @brief Zero size bytes starting at address.
 */
static void driver_memzero(void *address, uint64_t size)
{
    uint8_t *bytes = (uint8_t *)address;
    while (size--) {
        *bytes++ = 0;
    }
}

/**
 * @brief Copy size bytes from src to dst.
 */
static void driver_memcpy(void *dst, const void *src, uint64_t size)
{
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    while (size--) {
        *out++ = *in++;
    }
}

/**
 * @brief Bind the currently-loading driver's mouse ops as the active mouse backend.
 */
static int driver_register_mouse(const struct reliefos_driver_mouse_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->poll || !ops->get_state) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    mouse_ops = ops;
    mouse_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the currently-loading driver's serial ops as the active serial backend.
 */
static int driver_register_serial(const struct reliefos_driver_serial_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->write) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    serial_ops = ops;
    serial_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the currently-loading driver's e1000 ops as the active network backend.
 */
static int driver_register_e1000(const struct reliefos_driver_e1000_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->mac || !ops->send ||
        !ops->poll || !ops->get_info) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    e1000_ops = ops;
    e1000_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Bind the loading driver's v1 backend with a non-reused generation.
 * @param ops Borrowed validated callbacks, owned by the loaded image.
 * @return Zero, -EINVAL or -ENOSPC. Task init under manager admission.
 */
static int driver_register_audio(const struct reliefos_driver_audio_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->configure ||
        !ops->write || !ops->get_state) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (audio_generation == UINT32_MAX) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        return -28;
    }
    ++audio_generation;
    audio_ops = ops;
    audio_owner = (uint32_t)loading_slot;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}

/**
 * @brief Capture and pin one published mouse callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_mouse_ops *driver_mouse_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_mouse_ops *ops = mouse_ops;
    uint32_t owner = mouse_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published serial callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_serial_ops *driver_serial_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_serial_ops *ops = serial_ops;
    uint32_t owner = serial_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published e1000 callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_e1000_ops *driver_e1000_service_pin(uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_e1000_ops *ops = e1000_ops;
    uint32_t owner = e1000_owner;
    if (!ops || !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Capture and pin one published audio callback table with its matching owner.
 * @param generation Exact OSS lease generation, or zero for native compatibility.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_audio_ops *driver_audio_service_pin_bound(uint32_t generation,
                                                                             uint32_t *out_owner)
{
    if (!out_owner) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_audio_ops *ops = audio_ops;
    uint32_t owner = audio_owner;
    if (!ops || (generation && (generation != audio_generation ||
                                generation != audio_lease_generation)) ||
        !driver_manager_service_try_pin(owner)) ops = NULL;
    else *out_owner = owner;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return ops;
}

/**
 * @brief Read a byte from an I/O port (exposed to drivers through the ABI).
 */
static uint8_t driver_api_inb(uint16_t port)
{
    return x86_64_inb(port);
}

/**
 * @brief Write a byte to an I/O port (exposed to drivers through the ABI).
 */
static void driver_api_outb(uint16_t port, uint8_t value)
{
    x86_64_outb(value, port);
}

/**
 * @brief Read a 32-bit value from an I/O port (exposed to drivers through the ABI).
 */
static uint32_t driver_api_inl(uint16_t port)
{
    return x86_64_inl(port);
}

/**
 * @brief Write a 32-bit value to an I/O port (exposed to drivers through the ABI).
 */
static void driver_api_outl(uint16_t port, uint32_t value)
{
    x86_64_outl(value, port);
}

/**
 * @brief Report the framebuffer's width and height, or -2 when no framebuffer is available.
 */
static int driver_api_framebuffer_size(uint32_t *width, uint32_t *height)
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
static int driver_api_pci_find(uint16_t vendor_id, uint16_t device_id,
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
static uint64_t driver_alloc_dma(uint32_t pages,uint64_t mask)
{
    return driver_alloc_dma_owned((uint32_t)loading_slot,pages,mask);
}
/** @brief Map a BAR lease for the loading module.
 * @param phys Aligned BAR base of a quiesced function.
 * @param bytes Requested byte length.
 * @return Owned mapping pointer or NULL. Init/task context, execution transaction.
 */
static void *driver_map_mmio(uint64_t phys,uint64_t bytes)
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
static int driver_request_pci_irq(const struct reliefos_driver_pci_device *dev,
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
static int driver_audio_register_card(const struct audio_card_identity *identity,
    const struct audio_card_ops *ops,void *opaque,uint32_t *out_id)
{
    if(loading_slot<0)return -22;
    return audio_register_card_owned(identity,ops,opaque,(uint32_t)loading_slot,out_id);
}

/** @brief Record the first unsafe teardown result in the serialized fini phase.
 * @param error Negative errno; nonnegative or out-of-phase reports are ignored.
 * @return None. No resource is released and no lock or wait is performed.
 */
static void driver_report_teardown_failure(int error)
{
    if (error < 0 && cleanup_slot >= 0 &&
        cleanup_slot < (int32_t)RELIEFOS_DRIVER_MAX &&
        !driver_slots[cleanup_slot].cleanup_error)
        driver_slots[cleanup_slot].cleanup_error = error;
}

/** @brief Invoke one module fini and collect its append-only failure report.
 * @param slot Owner slot under the manager admission gate, execution suspended.
 * @param module Live descriptor retained throughout this synchronous callback.
 * @return 0 for safe automatic cleanup or the first reported negative errno.
 */
static int driver_run_fini(struct driver_slot *slot,
                            const struct reliefos_driver_module *module)
{
    slot->cleanup_error = 0;
    cleanup_slot = (int32_t)slot->info.id;
    if (module->fini) module->fini();
    cleanup_slot = -1;
    return slot->cleanup_error;
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
    .console_write = console_write,
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
    .audio_unregister_card = audio_unregister_card,
    .audio_period_elapsed = audio_period_elapsed,
    .audio_control_changed = audio_control_changed,
    .pci_write32 = pci_config_write32,
    .audio_set_service = audio_card_set_service,
    .report_teardown_failure = driver_report_teardown_failure,
    .audio_request_disconnect = audio_request_controller_disconnect,
};

/**
 * @brief Detach any device ops owned by slot_id and reset their state (notifying net on e1000).
 */
static void driver_clear_services(uint32_t slot_id)
{
    bool detach_network = false;
    driver_manager_service_disable(slot_id);
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (mouse_ops && mouse_owner == slot_id) {
        mouse_ops = 0;
        mouse_owner = 0;
        mouse_cache = (struct mouse_state){0};
    }
    if (serial_ops && serial_owner == slot_id) {
        serial_ops = 0;
        serial_owner = 0;
    }
    if (e1000_ops && e1000_owner == slot_id) {
        detach_network = true;
        e1000_ops = 0;
        e1000_owner = 0;
    }
    if (audio_ops && audio_owner == slot_id) {
        audio_ops = 0;
        audio_owner = 0;
    }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    if (detach_network) net_driver_detached();
}

/**
 * @brief Claim the next free driver slot and seed its built-in bookkeeping.
 * @return Slot with id, state and flags set, or NULL when the table is full.
 */
static struct driver_slot *driver_claim_slot(void)
{
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        struct driver_slot *slot = &driver_slots[index];
        if (!slot->info.file[0]) {
            driver_memzero(slot, sizeof(*slot));
            slot->info.id = index;
            slot->info.state = RELIEFOS_DRIVER_STATE_UNLOADED;
            slot->info.flags = RELIEFOS_DRIVER_FLAG_AUTOSTART |
                               RELIEFOS_DRIVER_FLAG_BUILTIN;
            return slot;
        }
    }
    return 0;
}

/**
 * @brief Initialize one built-in module and roll back owner resources on failure.
 * @param slot Manager-owned slot under serialized task transaction.
 * @param module Built-in descriptor linked into the kernel image.
 * @return 0 or negative errno. Task context, no registry lock during callbacks;
 * a failed audio STOP or unsafe fini retains the slot with its recorded error.
 */
static int driver_load_builtin(struct driver_slot *slot,
                               const struct reliefos_driver_module *module)
{
    int ret;
    if (!slot || !module) {
        return -22;
    }
    if (slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) {
        return 0;
    }
    if (module->magic != RELIEFOS_DRIVER_MODULE_MAGIC ||
        module->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        module->struct_size != sizeof(*module) || !module->name[0] || !module->init) {
        ret = -8;
        driver_set_error(slot, ret, "Driver descriptor ABI is invalid");
        return ret;
    }
    slot->module = module;
    slot->info.state = RELIEFOS_DRIVER_STATE_LOADING;
    slot->info.error[0] = 0;
    slot->info.kind = module->kind;
    slot->info.abi_version = module->abi_version;
    slot->info.version = module->version;
    slot->info.load_address = 0;
    slot->info.image_size = 0;
    driver_copy_text(slot->info.name, sizeof(slot->info.name), module->name);
    loading_slot = (int32_t)slot->info.id;
    struct driver_manager_wait_scope init_scope = {0};
    if ((ret = driver_manager_wait_begin(&init_scope)) < 0) {
        loading_slot = -1;
        driver_set_error(slot, ret, "Cannot release execution transaction for init");
        return ret;
    }
    ret = module->init(&driver_kernel_api);
    driver_manager_wait_end_or_panic(&init_scope);
    loading_slot = -1;
    if (ret < 0) {
        struct driver_manager_wait_scope rollback_scope = {0};
        driver_manager_service_close(slot->info.id);
        if (driver_manager_wait_begin(&rollback_scope) < 0)
            panic("driver manager cannot suspend execution for init rollback");
        int disconnect = audio_unregister_owner(slot->info.id, 1);
        if (disconnect) {
            /* A failed STOP forbids reclaiming resources still owned by the
             * device. Keep the slot retained and its services open. */
            slot->module = module;
            driver_set_error(slot, disconnect, "Audio STOP failed; module retained");
            slot->info.state = RELIEFOS_DRIVER_STATE_LOADED;
            driver_manager_wait_end_or_panic(&rollback_scope);
            driver_manager_service_enable(slot->info.id);
            return disconnect;
        }
        driver_manager_service_drain(slot->info.id);
        driver_resources_release(slot->info.id, true);
        int cleanup = driver_run_fini(slot, module);
        driver_manager_wait_end_or_panic(&rollback_scope);
        if (cleanup < 0) {
            slot->module = module;
            driver_set_error(slot, cleanup, "Unsafe teardown; module retained");
            slot->info.state = RELIEFOS_DRIVER_STATE_LOADED;
            return cleanup;
        }
        driver_clear_services(slot->info.id);
        driver_resources_release(slot->info.id, false);
        driver_set_error(slot, ret, "Driver initialization failed");
        slot->module = 0;
        return ret;
    }
    slot->module = module;
    slot->info.state = RELIEFOS_DRIVER_STATE_LOADED;
    slot->info.error[0] = 0;
    driver_manager_service_enable(slot->info.id);
    console_printf("[driver] loaded %s abi=%u\n", module->name,
                   slot->info.abi_version);
    return 0;
}

/**
 * @brief Clear all driver slots and the cached mouse state at startup.
 */
void driver_manager_init(void)
{
    driver_memzero(driver_slots, sizeof(driver_slots));
    mouse_cache = (struct mouse_state){0};
    console_printf("[driver] manager ready abi=%u\n", RELIEFOS_DRIVER_ABI_VERSION);
}

/** @brief Initialize the built-in drivers with manager admission and execution ownership held.
 * @return None. Called only by the public boot hook.
 */
static void driver_manager_load_builtin_admitted(void)
{
    for (uint32_t index = 0;
         index < sizeof(builtin_modules) / sizeof(builtin_modules[0]); ++index) {
        const struct reliefos_driver_module *module = builtin_modules[index];
        struct driver_slot *slot = driver_claim_slot();
        if (!slot) {
            console_printf("[driver] no free slot for builtin %s\n", module->name);
            continue;
        }
        driver_copy_text(slot->info.file, sizeof(slot->info.file), module->name);
        if (driver_load_builtin(slot, module) < 0) {
            if (slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) continue;
            console_printf("[driver] retrying %s\n", slot->info.file);
            /* Second init attempt after the first failed. */
            (void)driver_load_builtin(slot, module);
        }
    }
}

/**
 * @brief Initialize every driver linked into the kernel image.
 * @return None. Boot has no outer execution transaction, so this hook owns a
 * short manager transaction and suspends it only around waitable module phases.
 */
void driver_manager_load_builtin(void)
{
    if (driver_manager_phase_try_enter() < 0) {
        console_printf("[driver] builtin load skipped: manager busy\n");
        return;
    }
    uint64_t execution_flags;
    kernel_execution_lock_irqsave(&execution_flags);
    driver_manager_load_builtin_admitted();
    kernel_execution_unlock_irqrestore(execution_flags);
    driver_manager_phase_leave();
}

/**
 * @brief Fill the query with up to capacity driver infos and set the total count.
 */
int driver_manager_list(struct reliefos_driver_list *query)
{
    uint32_t count = 0;
    if (!query) {
        return -22;
    }
    if (driver_manager_phase_try_enter() < 0) return -16;
    if (query->capacity > RELIEFOS_DRIVER_MAX) {
        query->capacity = RELIEFOS_DRIVER_MAX;
    }
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        if (!driver_slots[index].info.file[0]) {
            continue;
        }
        if (query->drivers && count < query->capacity) {
            query->drivers[count] = driver_slots[index].info;
        }
        ++count;
    }
    query->count = count;
    driver_manager_phase_leave();
    return 0;
}

/**
 * @brief Reject runtime driver control actions; every driver is built into the
 * kernel image, so there is no module to load, unload or disable at runtime.
 */
int driver_manager_control(struct reliefos_driver_control *request)
{
    if (!request) {
        return -22;
    }
    request->status = -95;
    return -95;
}

/**
 * @brief Reserved startup hook; mouse state arrives through the registered driver.
 */
void mouse_init(void)
{
}

/**
 * @brief Poll the active mouse driver for new events, if one is registered.
 * @return None. IRQ dispatch may call this wrapper; it pins a consistent local
 * ops/owner snapshot across the callback without holding service locks. The
 * legacy poll callback must remain bounded and IRQ-safe.
 */
void mouse_poll(void)
{
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (ops && ops->poll) {
        ops->poll();
        driver_manager_service_unpin(owner);
    } else if (ops) {
        driver_manager_service_unpin(owner);
    }
}

/**
 * @brief Refresh the manager-owned mouse cache from a pinned driver snapshot.
 * @return Pointer to manager-owned cached state, never into the module image.
 */
const struct mouse_state *mouse_get_state(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (ops && ops->get_state) {
        state = (struct reliefos_driver_mouse_state){0};
        ops->get_state(&state);
        uint64_t flags;
        kernel_spin_lock_irqsave(&manager_service_lock, &flags);
        mouse_cache = (struct mouse_state){
            .x = state.x,
            .y = state.y,
            .buttons = state.buttons,
            .present = state.present != 0,
            .absolute = state.absolute != 0,
        };
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    }
    if (ops) driver_manager_service_unpin(owner);
    return &mouse_cache;
}

/**
 * @brief Record whether the mouse cursor should be drawn.
 */
void mouse_set_visible(bool visible)
{
    mouse_visible = visible;
}

/**
 * @brief Return the cached cursor visibility flag.
 */
bool mouse_is_visible(void)
{
    return mouse_visible;
}

/**
 * @brief Report the driver's pending-event counter, or 0 when no driver is registered.
 * @return Copied counter value; the matching ops/owner pin spans get_state.
 */
uint32_t mouse_event_count(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.event_count;
}

/**
 * @brief Report the driver's last PS/2 status byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_status(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_status;
}

/**
 * @brief Report the driver's last PS/2 data byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_data(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_data;
}

/**
 * @brief Report the driver's last PS/2 ack byte, or 0 when no driver is registered.
 * @return Copied byte; the matching ops/owner pin spans get_state.
 */
uint8_t mouse_last_ack(void)
{
    struct reliefos_driver_mouse_state state;
    uint32_t owner;
    const struct reliefos_driver_mouse_ops *ops = driver_mouse_service_pin(&owner);
    if (!ops || !ops->get_state) {
        if (ops) driver_manager_service_unpin(owner);
        return 0;
    }
    ops->get_state(&state);
    driver_manager_service_unpin(owner);
    return state.last_ack;
}

/**
 * @brief Copy the current cached mouse state into out.
 */
void driver_manager_mouse_state(struct mouse_state *out)
{
    if (out) {
        *out = *mouse_get_state();
    }
}

/**
 * @brief Forward mouse_event_count to callers outside this file.
 */
uint32_t driver_manager_mouse_event_count(void)
{
    return mouse_event_count();
}

/**
 * @brief Forward mouse_last_status to callers outside this file.
 */
uint8_t driver_manager_mouse_last_status(void)
{
    return mouse_last_status();
}

/**
 * @brief Forward mouse_last_data to callers outside this file.
 */
uint8_t driver_manager_mouse_last_data(void)
{
    return mouse_last_data();
}

/**
 * @brief Forward mouse_last_ack to callers outside this file.
 */
uint8_t driver_manager_mouse_last_ack(void)
{
    return mouse_last_ack();
}

/**
 * @brief Forward mouse_poll to callers outside this file.
 */
void driver_manager_mouse_poll(void)
{
    mouse_poll();
}

/**
 * @brief Program the COM1 UART (baud, frame format, FIFO, IRQ) and enable the early serial path.
 */
void serial_init(void)
{
    x86_64_outb(0x00u, (uint16_t)(EARLY_SERIAL_COM1 + 1u));
    x86_64_outb(0x80u, (uint16_t)(EARLY_SERIAL_COM1 + 3u));
    x86_64_outb(0x03u, (uint16_t)(EARLY_SERIAL_COM1 + 0u));
    x86_64_outb(0x00u, (uint16_t)(EARLY_SERIAL_COM1 + 1u));
    x86_64_outb(0x03u, (uint16_t)(EARLY_SERIAL_COM1 + 3u));
    x86_64_outb(0xc7u, (uint16_t)(EARLY_SERIAL_COM1 + 2u));
    x86_64_outb(0x0bu, (uint16_t)(EARLY_SERIAL_COM1 + 4u));
    early_serial_ready = 1;
}

/**
 * @brief Return 1 when a serial driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; one consistent ops/owner pin
 * spans is_ready, with no manager lock held during the callback.
 */
int serial_is_ready(void)
{
    uint32_t owner;
    const struct reliefos_driver_serial_ops *ops = driver_serial_service_pin(&owner);
    if (!ops) return 0;
    int ready = ops->is_ready ? ops->is_ready() : 0;
    driver_manager_service_unpin(owner);
    return ready;
}

/**
 * @brief Write text through the serial driver, or the built-in COM1 path before one loads.
 * @param text Kernel-owned NUL-terminated text borrowed through callback return.
 * @return None. The matching local ops/owner snapshot stays pinned across write.
 */
void serial_write(const char *text)
{
    uint32_t owner;
    const struct reliefos_driver_serial_ops *ops = driver_serial_service_pin(&owner);
    if (ops && ops->write) {
        ops->write(text);
        driver_manager_service_unpin(owner);
        return;
    }
    if (ops) driver_manager_service_unpin(owner);
    early_serial_write(text);
}

/**
 * @brief Reserved startup hook; the NIC is brought up by its driver's init().
 */
void e1000_init(void)
{
}

/**
 * @brief Return 1 when an e1000 driver is registered and reports ready.
 * @return Driver result or 0 while absent/closing; the matching ops/owner stays
 * pinned until is_ready returns.
 */
int e1000_is_ready(void)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return 0;
    int ready = ops->is_ready ? ops->is_ready() : 0;
    driver_manager_service_unpin(owner);
    return ready;
}

/**
 * @brief Return a kernel-owned copy of the NIC MAC, or a zeroed placeholder when absent.
 * @return Manager cache pointer; the module's borrowed pointer is copied while its
 * matching ops/owner snapshot remains pinned.
 */
const uint8_t *e1000_mac(void)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops || !ops->mac) {
        if (ops) driver_manager_service_unpin(owner);
        return e1000_empty_mac;
    }
    const uint8_t *borrowed = ops->mac();
    uint64_t flags;
    kernel_spin_lock_irqsave(&e1000_mac_cache_lock, &flags);
    if (borrowed) driver_memcpy(e1000_mac_cache, borrowed, sizeof(e1000_mac_cache));
    else driver_memzero(e1000_mac_cache, sizeof(e1000_mac_cache));
    kernel_spin_unlock_irqrestore(&e1000_mac_cache_lock, flags);
    driver_manager_service_unpin(owner);
    return e1000_mac_cache;
}

/**
 * @brief Send an Ethernet frame through the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned frame borrowed through send callback return.
 * @param len Number of bytes available at frame.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and send.
 */
int e1000_send(const void *frame, uint32_t len)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->send && ops->is_ready()
                  ? ops->send(frame, len) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Poll received frames from the driver, or -19 when no driver is ready.
 * @param frame Kernel-owned receive buffer.
 * @param capacity Bytes available at frame.
 * @param out_len Receives the copied frame length when supplied by the driver.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and poll.
 */
int e1000_poll(void *frame, uint32_t capacity, uint32_t *out_len)
{
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->poll && ops->is_ready()
                  ? ops->poll(frame, capacity, out_len) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Copy NIC identity independently of carrier state.
 * @param info Receives kernel-owned identity fields and MAC bytes.
 * @return None. Driver output is copied to a stack snapshot before unpin.
 */
void e1000_get_info(struct e1000_info *info)
{
    struct reliefos_driver_e1000_info source;
    if (!info) {
        return;
    }
    *info = (struct e1000_info){0};
    uint32_t owner;
    const struct reliefos_driver_e1000_ops *ops = driver_e1000_service_pin(&owner);
    if (!ops || !ops->get_info) {
        if (ops) driver_manager_service_unpin(owner);
        return;
    }
    ops->get_info(&source);
    driver_manager_service_unpin(owner);
    info->present = source.present;
    info->active = source.active;
    info->vendor_id = source.vendor_id;
    info->device_id = source.device_id;
    info->bus = source.bus;
    info->slot = source.slot;
    info->function = source.function;
    for (uint32_t index = 0; index < 6; ++index) {
        info->mac[index] = source.mac[index];
    }
}

/**
 * @brief Configure the audio output format on the active driver, or -19 when absent.
 * @param format Kernel-owned format borrowed through configure callback return.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and configure.
 */
int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{ return driver_manager_audio_configure_bound(0, format); }

/** @brief Configure one generation-pinned v1 audio callback table.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param format Borrowed sample format.
 * @return Driver result or -ENODEV. Task context; no manager lock spans callback.
 */
int driver_manager_audio_configure_bound(uint32_t generation, const struct reliefos_audio_format *format)
{
    if (!format) {
        return -19;
    }
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) return -19;
    int ret = ops->is_ready && ops->configure && ops->is_ready()
                  ? ops->configure(format) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Write audio samples; returns -19 and a NO_DEVICE status when no driver is ready.
 * @param data Kernel-owned sample bytes borrowed through write callback return.
 * @param length Number of valid sample bytes.
 * @param out_status Optional kernel-owned status destination.
 * @return Driver result or -19 while absent/closing; one pin spans readiness and write.
 */
long driver_manager_audio_write(const void *data, uint32_t length,
                                uint32_t *out_status)
{ return driver_manager_audio_write_bound(0, data, length, out_status); }

/** @brief Write bytes through the exact acquired backend generation.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param data Borrowed samples. @param length Byte count.
 * @param out_status Optional status output.
 * @return Written bytes or errno. Task context; no manager lock spans callback.
 */
long driver_manager_audio_write_bound(uint32_t generation, const void *data, uint32_t length,
                                      uint32_t *out_status)
{
    if (out_status) {
        *out_status = RELIEFOS_AUDIO_STATUS_NO_DEVICE;
    }
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) {
        return -19;
    }
    long ret = ops->is_ready && ops->write && ops->is_ready()
                   ? ops->write(data, length, out_status) : -19;
    driver_manager_service_unpin(owner);
    return ret;
}

/**
 * @brief Copy the audio driver's state into out, zeroing it when no driver is ready.
 * @param out Optional kernel-owned destination.
 * @return None. A consistent local ops/owner pin stays live through get_state.
 */
void driver_manager_audio_get_state(struct reliefos_audio_state *out)
{ if (out) (void)driver_manager_audio_state_bound(0, out); }

/** @brief Copy state while holding the exact backend's callback pin.
 * @param generation OSS lease generation, or zero for native compatibility.
 * @param out Borrowed output, cleared before attempting admission.
 * @return Zero or -ENODEV/-EINVAL. Task context; callback has no manager lock.
 */
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *out)
{
    if (!out) return -22;
    *out = (struct reliefos_audio_state){0};
    uint32_t owner;
    const struct reliefos_driver_audio_ops *ops = driver_audio_service_pin_bound(generation, &owner);
    if (!ops) return -19;
    if (ops->is_ready && ops->is_ready() && ops->get_state) ops->get_state(out);
    driver_manager_service_unpin(owner);
    return out->present && out->active ? 0 : -19;
}

/** @brief Reserve one OSS description on the current published v1 backend.
 * @param generation Receives a non-reused token on success.
 * @param state Receives real hardware state under a callback pin.
 * @return Zero, -EINVAL, -ENODEV or -EBUSY. Task context; no lock spans callback.
 */
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state)
{
    if (!generation || !state) return -22;
    *state = (struct reliefos_audio_state){0};
    uint64_t flags;uint32_t owner, token;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    const struct reliefos_driver_audio_ops *ops = audio_ops;
    owner = audio_owner;token = audio_generation;
    if (!ops || !driver_manager_service_try_pin(owner)) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);return -19;
    }
    if (audio_lease_generation == token) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        driver_manager_service_unpin(owner);return -16;
    }
    audio_lease_generation = token;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    if (ops->is_ready() && ops->get_state) ops->get_state(state);
    driver_manager_service_unpin(owner);
    if (!state->present || !state->active) {
        driver_manager_audio_release(token);return -19;
    }
    *generation = token;return 0;
}

/** @brief Release only the lease generation held by the closing OSS description.
 * @param generation Nonzero acquired token, possibly stale after unload.
 * @return None. Task context; no callback or hardware access.
 */
void driver_manager_audio_release(uint32_t generation)
{
    uint64_t flags;kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (generation && generation == audio_lease_generation) audio_lease_generation = 0;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
}
