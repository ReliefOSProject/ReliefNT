#include "manager_internal.h"

/**
 * @brief Capture and pin one published mouse callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_mouse_ops *driver_mouse_service_pin(uint32_t *out_owner);

/**
 * @brief Capture and pin one published serial callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_serial_ops *driver_serial_service_pin(uint32_t *out_owner);

/**
 * @brief Capture and pin one published e1000 callback table with its matching owner.
 * @param out_owner Receives the slot whose callback pin must be released.
 * @return Consistent local ops snapshot, or NULL while absent/closing.
 */
static const struct reliefos_driver_e1000_ops *driver_e1000_service_pin(uint32_t *out_owner);

static bool mouse_visible = true;
static uint8_t e1000_empty_mac[6];
static uint8_t e1000_mac_cache[6];
static struct kernel_spinlock e1000_mac_cache_lock = KERNEL_SPINLOCK_INIT;
/**
 * @brief Bind the currently-loading driver's mouse ops as the active mouse backend.
 */
int driver_register_mouse(const struct reliefos_driver_mouse_ops *ops)
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
int driver_register_serial(const struct reliefos_driver_serial_ops *ops)
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
int driver_register_e1000(const struct reliefos_driver_e1000_ops *ops)
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
int driver_register_audio(const struct reliefos_driver_audio_ops *ops)
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
const struct reliefos_driver_audio_ops *driver_audio_service_pin_bound(uint32_t generation,
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
