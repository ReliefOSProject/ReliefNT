#include "manager_internal.h"

/** @brief Resume manager ownership after a phase that may wait or sleep.
 * @param scope Active manager wait scope whose token must be consumed.
 * @return None. A lost transaction is a kernel invariant failure; no slot state
 * or module resources may be reclaimed without the resumed transaction.
 */
static void driver_manager_wait_end_or_panic(struct driver_manager_wait_scope *scope);

/**
 * @brief Copy src into dst up to cap-1 bytes and always NUL-terminate; safe with NULL src or cap 0.
 */
static void driver_copy_text(char *dst, uint32_t cap, const char *src);

/**
 * @brief Mark the slot failed, record the error text, and log it only when status is negative.
 */
static void driver_set_error(struct driver_slot *slot, int status, const char *error);

/** @brief Validate a driver's fixed descriptor header before any module callback.
 * @param module Candidate descriptor supplied by the built-in list or loader.
 * @return True only for the current ABI, exact descriptor size, and a callable
 *         named module; false rejects legacy binaries without running init.
 */
static bool driver_module_abi_valid(const struct reliefos_driver_module *module)
{
    return module && module->magic == RELIEFOS_DRIVER_MODULE_MAGIC &&
           module->abi_version == RELIEFOS_DRIVER_ABI_VERSION &&
           module->struct_size == sizeof(*module) && module->name[0] &&
           module->init;
}

/** @brief Invoke one module fini and collect its append-only failure report.
 * @param slot Owner slot under the manager admission gate, execution suspended.
 * @param module Live descriptor retained throughout this synchronous callback.
 * @return 0 for safe automatic cleanup or the first reported negative errno.
 */
static int driver_run_fini(struct driver_slot *slot,
                            const struct reliefos_driver_module *module);

/**
 * @brief Detach any device ops owned by slot_id and reset their state (notifying net on e1000).
 */
static void driver_clear_services(uint32_t slot_id);

/**
 * @brief Claim the next free driver slot and seed its built-in bookkeeping.
 * @return Slot with id, state and flags set, or NULL when the table is full.
 */
static struct driver_slot *driver_claim_slot(void);

/**
 * @brief Initialize one built-in module and roll back owner resources on failure.
 * @param slot Manager-owned slot under serialized task transaction.
 * @param module Built-in descriptor linked into the kernel image.
 * @return 0 or negative errno. Task context, no registry lock during callbacks;
 * a failed audio STOP or unsafe fini retains the slot with its recorded error.
 */
static int driver_load_builtin(struct driver_slot *slot,
                               const struct reliefos_driver_module *module);

/** @brief Initialize the built-in drivers with manager admission and execution ownership held.
 * @return None. Called only by the public boot hook.
 */
static void driver_manager_load_builtin_admitted(void);

int early_serial_ready;

static const struct reliefos_driver_module *const builtin_modules[] = {
    &serial_driver_module,
    &ac97_driver_module,
    &e1000_driver_module,
    &es1371_driver_module,
    &hda_driver_module,
    &mouse_driver_module,
};
struct driver_slot driver_slots[RELIEFOS_DRIVER_MAX];

int32_t loading_slot = -1;

int32_t cleanup_slot = -1;

const struct reliefos_driver_mouse_ops *mouse_ops;

const struct reliefos_driver_serial_ops *serial_ops;

const struct reliefos_driver_e1000_ops *e1000_ops;

const struct reliefos_driver_audio_ops *audio_ops;

uint32_t mouse_owner;

uint32_t serial_owner;

uint32_t e1000_owner;

uint32_t audio_owner;


uint32_t audio_generation, audio_lease_generation;

struct mouse_state mouse_cache;

struct kernel_spinlock manager_service_lock = KERNEL_SPINLOCK_INIT;

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
    .audio_unregister_card = audio_unregister_card,
    .audio_period_elapsed = audio_period_elapsed,
    .audio_control_changed = audio_control_changed,
    .pci_write32 = pci_config_write32,
    .audio_set_service = audio_card_set_service,
    .report_teardown_failure = driver_report_teardown_failure,
    .audio_request_disconnect = audio_request_controller_disconnect,
    .console_log = console_log,
    .phys_to_virt = mm_phys_to_virt,
};
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
        console_log(status == -19 ? CONSOLE_LOG_INFO : CONSOLE_LOG_ERROR,
                    "[driver] %s failed status=%d: %s\n", slot->info.file,
                       status, slot->info.error);
    }
}

/**
 * @brief Zero size bytes starting at address.
 */
void driver_memzero(void *address, uint64_t size)
{
    uint8_t *bytes = (uint8_t *)address;
    while (size--) {
        *bytes++ = 0;
    }
}

/**
 * @brief Copy size bytes from src to dst.
 */
void driver_memcpy(void *dst, const void *src, uint64_t size)
{
    uint8_t *out = (uint8_t *)dst;
    const uint8_t *in = (const uint8_t *)src;
    while (size--) {
        *out++ = *in++;
    }
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
    if (!driver_module_abi_valid(module)) {
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
    console_info("[driver] loaded %s abi=%u\n", module->name,
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
    console_info("[driver] manager ready abi=%u slots=%u builtins=%u\n",
                 RELIEFOS_DRIVER_ABI_VERSION, RELIEFOS_DRIVER_MAX,
                 (unsigned)(sizeof(builtin_modules) / sizeof(builtin_modules[0])));
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
            console_warn("[driver] no free slot for builtin %s\n", module->name);
            continue;
        }
        driver_copy_text(slot->info.file, sizeof(slot->info.file), module->name);
        if (driver_load_builtin(slot, module) < 0) {
            if (slot->info.state == RELIEFOS_DRIVER_STATE_LOADED) continue;
            console_warn("[driver] retrying %s\n", slot->info.file);
            /* Second init attempt after the first failed. */
            (void)driver_load_builtin(slot, module);
        }
    }
    uint32_t loaded = 0;
    uint32_t inactive = 0;
    for (uint32_t index = 0; index < RELIEFOS_DRIVER_MAX; ++index) {
        if (!driver_slots[index].info.file[0]) continue;
        if (driver_slots[index].info.state == RELIEFOS_DRIVER_STATE_LOADED) ++loaded;
        else ++inactive;
    }
    console_info("[driver] built-in summary loaded=%u inactive=%u\n", loaded, inactive);
}

/**
 * @brief Initialize every driver linked into the kernel image.
 * @return None. Boot has no outer execution transaction, so this hook owns a
 * short manager transaction and suspends it only around waitable module phases.
 */
void driver_manager_load_builtin(void)
{
    if (driver_manager_phase_try_enter() < 0) {
        console_warn("[driver] builtin load skipped: manager busy\n");
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
