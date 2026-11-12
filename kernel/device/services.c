#include <reliefnt/proc_stats.h>
#include <reliefnt/heap.h>
#include <reliefnt/export.h>
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

static bool mouse_visible = true;
static const uint8_t netdev_empty_mac[6];

struct netdev_binding {
    struct netdev_binding *next;
    struct reliefnt_netdev_ops ops;
    void *context;
    uint32_t owner, ifindex;
    bool published;
    uint8_t mac_cache[6];
    struct net_proc_counters counters;
};
static struct netdev_binding *netdev_bindings;
static uint32_t netdev_next_ifindex = 2;
static struct net_proc_counters proc_nic;

/** @brief Adapt a legacy single-instance provider to the context contract.
 * @param context Original immutable callback table. @return Carrier state.
 */
static int legacy_netdev_ready(void *context)
{ return ((const struct reliefos_driver_e1000_ops *)context)->is_ready(); }
/** @brief Borrow the legacy provider's station address.
 * @param context Original callback table. @return Six borrowed MAC bytes.
 */
static const uint8_t *legacy_netdev_mac(void *context)
{ return ((const struct reliefos_driver_e1000_ops *)context)->mac(); }
/** @brief Dispatch a frame to the original single-instance provider.
 * @param context Original callback table. @param frame Borrowed bytes.
 * @param length Frame bytes. @return Provider result.
 */
static int legacy_netdev_send(void *context, const void *frame, uint32_t length)
{ return ((const struct reliefos_driver_e1000_ops *)context)->send(frame, length); }
/** @brief Poll the original single-instance provider.
 * @param context Original table. @param frame Output bytes.
 * @param capacity Output capacity. @param length Output length. @return Provider result.
 */
static int legacy_netdev_poll(void *context, void *frame, uint32_t capacity, uint32_t *length)
{ return ((const struct reliefos_driver_e1000_ops *)context)->poll(frame, capacity, length); }
/** @brief Convert the legacy identity to the private network identity.
 * @param context Original callback table. @param out Caller-owned identity.
 */
static void legacy_netdev_info(void *context, struct e1000_info *out)
{
    struct reliefos_driver_e1000_info source = {0};
    ((const struct reliefos_driver_e1000_ops *)context)->get_info(&source);
    *out = (struct e1000_info){.present=source.present, .active=source.active,
        .vendor_id=source.vendor_id, .device_id=source.device_id,
        .bus=source.bus, .slot=source.slot, .function=source.function};
    driver_memcpy(out->mac, source.mac, sizeof(out->mac));
}
static const struct reliefnt_netdev_ops legacy_netdev_ops = {
    RELIEFNT_NETDEV_OPS_VERSION, sizeof(struct reliefnt_netdev_ops),
    legacy_netdev_ready, legacy_netdev_mac, legacy_netdev_send,
    legacy_netdev_poll, legacy_netdev_info
};
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
 * @brief Adapt the loading driver's legacy singleton Ethernet callbacks.
 * @param ops Validated callbacks retained by the current loading driver slot.
 * @return Zero, -EINVAL, allocation/index error, or -EBUSY when another legacy
 * provider exists. Task init; no callback under the service lock.
 */
int driver_register_e1000(const struct reliefos_driver_e1000_ops *ops)
{
    if (loading_slot < 0 || !ops || !ops->is_ready || !ops->mac || !ops->send ||
        !ops->poll || !ops->get_info) {
        return -22;
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (e1000_ops) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        return -16;
    }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    uint32_t ifindex;
    int ret = driver_register_netdev(&legacy_netdev_ops, (void *)ops, &ifindex);
    if (ret) return ret;
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

/** @brief Register a provider context without exposing it before init succeeds.
 * @param ops Immutable v1 callback table. @param context Driver-owned device.
 * @param out_ifindex Receives the never-reused interface identity.
 * @return Zero or -EINVAL/-ENOMEM/-EOVERFLOW; init task context.
 */
int driver_register_netdev(const struct reliefnt_netdev_ops *ops, void *context,
                           uint32_t *out_ifindex)
{
    if (loading_slot < 0 || (uint32_t)loading_slot >= RELIEFOS_DRIVER_MAX ||
        !out_ifindex || !context || !ops ||
        ops->version != RELIEFNT_NETDEV_OPS_VERSION || ops->struct_size != sizeof(*ops) ||
        !ops->is_ready || !ops->mac || !ops->send || !ops->poll || !ops->get_info)
        return -22;
    struct netdev_binding *binding = kernel_malloc(sizeof(*binding));
    if (!binding) return -12;
    *binding = (struct netdev_binding){.ops=*ops, .context=context, .owner=(uint32_t)loading_slot};
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (netdev_next_ifindex > 0x7fffffffu) {
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        kernel_free(binding);
        return -75;
    }
    binding->ifindex = netdev_next_ifindex++;
    struct netdev_binding **tail = &netdev_bindings;
    while (*tail) tail = &(*tail)->next;
    *tail = binding;
    *out_ifindex = binding->ifindex;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return 0;
}
EXPORT_SYMBOL(driver_register_netdev);

/** @brief Publish one completed owner's NICs before issuing unlocked notifications.
 * @param owner Loaded driver slot. Caller holds network execution ownership.
 */
void driver_netdev_publish_owner(uint32_t owner)
{
    for (;;) {
        uint64_t flags; uint32_t id = 0;
        kernel_spin_lock_irqsave(&manager_service_lock, &flags);
        for (struct netdev_binding *b=netdev_bindings; b; b=b->next)
            if (b->owner == owner && !b->published) { b->published=true; id=b->ifindex; break; }
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        if (!id) break;
        net_device_attached(id);
    }
}

/** @brief Remove a drained owner's NICs and notify after each registry unlink.
 * @param owner Closed and drained driver slot. Caller holds network execution.
 */
void driver_netdev_remove_owner(uint32_t owner)
{
    for (;;) {
        uint64_t flags;
        kernel_spin_lock_irqsave(&manager_service_lock, &flags);
        struct netdev_binding **link=&netdev_bindings;
        while (*link && (*link)->owner != owner) link=&(*link)->next;
        struct netdev_binding *b=*link;
        if (b) *link=b->next;
        kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
        if (!b) break;
        uint32_t id=b->ifindex; bool published=b->published;
        kernel_free(b);
        if (published) net_device_detached(id);
    }
}

/** @brief Pin the exact published NIC and its original module callback owner.
 * @param ifindex Stable interface index. @return Retained binding or NULL;
 * caller releases the owner pin before allowing registry removal.
 */
static struct netdev_binding *netdev_pin(uint32_t ifindex)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    struct netdev_binding *b=netdev_bindings;
    while (b && (!b->published || b->ifindex != ifindex)) b=b->next;
    if (b && !driver_manager_service_try_pin(b->owner)) b=NULL;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return b;
}

/** @brief Find the next published NIC in stable index order.
 * @param after Previous index or zero. @return Next index or zero.
 */
uint32_t netdev_next(uint32_t after)
{
    uint32_t result=0; uint64_t flags;
    kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    for (struct netdev_binding *b=netdev_bindings; b; b=b->next)
        if (b->published && b->ifindex > after && (!result || b->ifindex < result)) result=b->ifindex;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    return result;
}

/** @brief Report the current carrier for one retained NIC.
 * @param ifindex Stable index. @return Nonzero when its provider reports ready.
 */
int netdev_is_ready(uint32_t ifindex)
{
    struct netdev_binding *b=netdev_pin(ifindex);
    if (!b) return 0;
    uint32_t owner=b->owner; int ready=b->ops.is_ready(b->context);
    driver_manager_service_unpin(owner); return ready;
}

/** @brief Copy a pinned NIC's address into core-owned per-NIC storage.
 * @param ifindex Stable index. @return Six bytes stable under network execution.
 */
const uint8_t *netdev_mac(uint32_t ifindex)
{
    struct netdev_binding *b=netdev_pin(ifindex);
    if (!b) return netdev_empty_mac;
    uint32_t owner=b->owner;
    const uint8_t *mac=b->ops.mac(b->context);
    uint64_t flags; kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (mac) driver_memcpy(b->mac_cache, mac, 6); else driver_memzero(b->mac_cache, 6);
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    const uint8_t *result=b->mac_cache;
    driver_manager_service_unpin(owner); return result;
}

/** @brief Send a frame and record its actual interface's transport counters.
 * @param ifindex Stable index. @param frame Borrowed frame bytes.
 * @param length Frame size. @return Provider result or -ENODEV.
 */
int netdev_send(uint32_t ifindex, const void *frame, uint32_t length)
{
    struct netdev_binding *b=netdev_pin(ifindex);
    if (!b) return -19;
    uint32_t owner=b->owner;
    int ret=b->ops.is_ready(b->context) ? b->ops.send(b->context,frame,length) : -19;
    uint64_t flags; kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (!ret) { b->counters.tx_bytes+=length; ++b->counters.tx_packets;
        proc_nic.tx_bytes+=length; ++proc_nic.tx_packets; }
    else { ++b->counters.tx_errors; ++proc_nic.tx_errors; }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    driver_manager_service_unpin(owner); return ret;
}

/** @brief Receive one frame and account it to its actual interface.
 * @param ifindex Stable index. @param frame Writable buffer.
 * @param capacity Buffer bytes. @param out_length Optional returned bytes.
 * @return One frame, zero for empty, or a negative Linux errno.
 */
int netdev_poll(uint32_t ifindex, void *frame, uint32_t capacity, uint32_t *out_length)
{
    if (out_length) *out_length=0;
    struct netdev_binding *b=netdev_pin(ifindex);
    if (!b) return -19;
    uint32_t owner=b->owner, length=0;
    if (!b->ops.is_ready(b->context)) {
        /* No receive descriptor was inspected while carrier is unavailable. */
        driver_manager_service_unpin(owner);
        return -19;
    }
    int ret=b->ops.poll(b->context,frame,capacity,&length);
    uint64_t flags; kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    if (ret>0) { b->counters.rx_bytes+=length; ++b->counters.rx_packets;
        proc_nic.rx_bytes+=length; ++proc_nic.rx_packets; }
    else if (ret<0) { ++b->counters.rx_errors; ++proc_nic.rx_errors;
        if (ret==-90 || ret==-74) { ++b->counters.rx_drops; ++proc_nic.rx_drops; } }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
    if (out_length) *out_length=length;
    driver_manager_service_unpin(owner); return ret;
}

/** @brief Copy one provider's PCI identity and carrier state while pinned.
 * @param ifindex Stable index. @param out Caller-owned nullable output.
 */
void netdev_get_info(uint32_t ifindex, struct e1000_info *out)
{
    if (!out) return;
    *out=(struct e1000_info){0};
    struct netdev_binding *b=netdev_pin(ifindex);
    if (!b) return;
    uint32_t owner=b->owner;
    b->ops.get_info(b->context,out);
    driver_manager_service_unpin(owner);
}

/** @brief Copy counters for a live exact identity without entering provider code.
 * @param ifindex Stable index. @param out Caller-owned nullable output.
 */
void netdev_counters(uint32_t ifindex, struct net_proc_counters *out)
{
    if (!out) return;
    *out=(struct net_proc_counters){0};
    uint64_t flags; kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    for (struct netdev_binding *b=netdev_bindings; b; b=b->next)
        if (b->published && b->ifindex==ifindex) { *out=b->counters; break; }
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
}

/** @brief Reserved startup hook; drivers initialize their own controllers. */
void e1000_init(void) { }
/** @brief Report carrier on the earliest published interface.
 * @return Nonzero while that interface has carrier.
 */
int e1000_is_ready(void) { return netdev_is_ready(netdev_next(0)); }
/** @brief Borrow the earliest published interface's core-owned MAC.
 * @return Six bytes valid while the caller owns network execution.
 */
const uint8_t *e1000_mac(void) { return netdev_mac(netdev_next(0)); }
/** @brief Send on the earliest published interface.
 * @param frame Borrowed bytes. @param len Frame length. @return Provider result.
 */
int e1000_send(const void *frame, uint32_t len) { return netdev_send(netdev_next(0),frame,len); }
/** @brief Poll the earliest published interface.
 * @param frame Writable buffer. @param capacity Buffer bytes. @param out_len Optional length.
 * @return One frame, zero or a negative errno.
 */
int e1000_poll(void *frame, uint32_t capacity, uint32_t *out_len)
{ return netdev_poll(netdev_next(0),frame,capacity,out_len); }
/** @brief Copy identity for the earliest published interface.
 * @param info Caller-owned nullable output.
 */
void e1000_get_info(struct e1000_info *info) { netdev_get_info(netdev_next(0),info); }
/** @brief Copy aggregate core-observed NIC counters.
 * @param out Required caller-owned output.
 */
void net_device_counters(struct net_proc_counters *out)
{
    if (!out) return;
    uint64_t flags; kernel_spin_lock_irqsave(&manager_service_lock, &flags);
    *out=proc_nic;
    kernel_spin_unlock_irqrestore(&manager_service_lock, flags);
}
