#include "controller_internal.h"

/** @brief Handle a controller interrupt using the bounded nonblocking service path.
 * @param opaque Controller whose MSI token was registered with the H1 API.
 * @return None. IRQ context; clears only observed W1C status and never waits,
 * allocates, calls audio clients, or unregisters a card.
 *
 * @par Context
 * Hard IRQ context for the live registered MSI token.
 * @par Ownership and locks/IRQ
 * Opaque controller lifetime is pinned by the IRQ lease; bounded service takes its short state lock. No sleep, allocation, callback, or unregister.
 */
static void hda_controller_irq(void *opaque);


#if !defined(HDA_STREAM_TESTING)
__attribute__((weak)) uint32_t hda_stream_service_locked(struct hda_controller *c,
                                                         uint32_t budget)
{
    (void)c;
    (void)budget;
    return 0u;
}
#endif

/** @brief Handle a controller interrupt using the bounded nonblocking service path.
 * @param opaque Controller whose MSI token was registered with the H1 API.
 * @return None. IRQ context; clears only observed W1C status and never waits,
 * allocates, calls audio clients, or unregisters a card.
 *
 * @par Context
 * Hard IRQ context for the live registered MSI token.
 * @par Ownership and locks/IRQ
 * Opaque controller lifetime is pinned by the IRQ lease; bounded service takes its short state lock. No sleep, allocation, callback, or unregister.
 */
static void hda_controller_irq(void *opaque)
{
    struct hda_controller *c = opaque;
    if (c && c->initialized) (void)hda_controller_service_budget(c, 1u);
}

/** @brief Initialize the PCI HDA codec transport and its DMA rings.
 * @param c Caller-owned controller storage; it is zeroed before any resource lease.
 * @param api Size-gated H1 API with PCI, MMIO, DMA, clock, and optional MSI services.
 * @param dev Borrowed PCI identity. The caller has quiesced this function before BAR probing.
 * @return 0 on a running ring transport or restricted immediate transport, or a
 * negative errno. ENOMEM/ERANGE/API failures do not fall back. If hardware cannot
 * be proven quiescent after a late failure, ownership remains in c for destroy retry.
 * Task init context; all waits permit IRQ progress and hold no controller lock.
 *
 * @par Context
 * Task init wait phase after caller quiesces the PCI function.
 * @par Ownership and locks/IRQ
 * On success c owns MMIO plus rings/optional IRQ in ring mode, or MMIO alone in immediate mode. If safe quiescence cannot be proven, c retains partial leases for destroy retry. No lock across waits; IRQ progress remains available.
 */
int hda_controller_init(struct hda_controller *c,
                        const struct reliefos_driver_kernel_api *api,
                        const struct reliefos_driver_pci_device *dev)
{
    if (!c || !api || !dev) return -EINVAL;
    hda_zero(c, (uint32_t)sizeof(*c));
    if (api->abi_version != RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size < RELIEFOS_DRIVER_AUDIO_API_SIZE ||
        !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->pci_write32 || !api->ticks || !api->sleep_ms ||
        !api->map_mmio || !api->unmap_mmio || !api->alloc_dma ||
        !api->free_dma) return -EOPNOTSUPP;

    c->api = api;
    c->dev = *dev;
    c->next_ticket = 1u;
    c->recovery_allowed = 1u;

    uint64_t bar_base = 0u;
    uint64_t bar_size = 0u;
    int ret = hda_find_bar(c, &bar_base, &bar_size);
    if (ret) goto fail;
    if (bar_size < HDA_REGISTER_BYTES ||
        bar_base > UINT64_MAX - HDA_REGISTER_BYTES) {
        ret = -ENODEV;
        goto fail;
    }
    c->bar_base = bar_base;
    c->bar_bytes = HDA_REGISTER_BYTES;
    c->bar_size = bar_size;
    c->mmio = api->map_mmio(bar_base, HDA_REGISTER_BYTES);
    if (!c->mmio) {
        ret = -ENOMEM;
        goto fail;
    }
    c->owns_mmio = 1u;

    uint16_t command = api->pci_read16(dev->bus, dev->slot, dev->function,
                                       HDA_PCI_COMMAND);
    hda_pci_write_command(c, (uint16_t)(command | HDA_PCI_COMMAND_MEMORY |
                                        HDA_PCI_COMMAND_MASTER));
    command = api->pci_read16(dev->bus, dev->slot, dev->function,
                              HDA_PCI_COMMAND);
    if ((command & (HDA_PCI_COMMAND_MEMORY | HDA_PCI_COMMAND_MASTER)) !=
        (HDA_PCI_COMMAND_MEMORY | HDA_PCI_COMMAND_MASTER)) {
        ret = -EIO;
        goto fail;
    }

    c->gcap = hda_read16(c, HDA_REG_GCAP);
    hda_write32(c, HDA_REG_INTCTL, 0u);
    ret = hda_stop_rings(c);
    if (ret) goto fail;
    ret = hda_reset_link(c);
    if (ret) goto fail;

    if (api->request_pci_irq && api->free_pci_irq) {
        uint32_t handle = 0u;
        if (api->request_pci_irq(dev, hda_controller_irq, c, &handle) == 0) {
            c->irq_handle = handle;
            c->owns_irq = 1u;
        }
    }

    ret = hda_program_rings(c);
    if (ret) {
        if (ret == -ENODEV || ret == -EIO || ret == -ETIMEDOUT) {
            int fallback = hda_enter_immediate_mode(c);
            if (!fallback) {
                c->initialized = 1u;
                return 0;
            }
            ret = fallback;
        }
        goto fail;
    }
    c->transport_mode = HDA_TRANSPORT_RINGS;
    c->initialized = 1u;
    return 0;

fail:
    if (c->owns_mmio) {
        int quiet = hda_quiesce_dma(c);
        if (quiet) return ret ? ret : quiet;
    }
    hda_release_resources(c);
    hda_zero(c, (uint32_t)sizeof(*c));
    return ret;
}

/** @brief Stop a controller and release DMA and mapping leases in safe order.
 * @param c Controller with resource ownership, including a partial failed-init
 * instance; all stream owners are stopped/released before teardown.
 * @return 0 after release, -EBUSY while streams remain, or an error while the
 * controller cannot prove DMA quiescence; on error all unsafe leases stay owned.
 * Task context; interrupt service is disabled and synchronized before free.
 *
 * @par Context
 * Task teardown context after stream owners stop/release.
 * @par Ownership and locks/IRQ
 * On success all c-owned leases return to H1; on unsafe quiesce failure c retains them for retry. Only short state locking; IRQ is disabled/synchronized before free.
 */
int hda_controller_destroy(struct hda_controller *c)
{
    if (!c || !c->api) return -EINVAL;
    uint64_t flags = hda_lock(c);
    uint32_t was_initialized = c->initialized;
    if (c->active_streams) {
        hda_unlock(c, flags);
        return -EBUSY;
    }
    c->initialized = 0u;
    hda_unlock(c, flags);

    int ret = hda_quiesce_dma(c);
    if (ret) {
        flags = hda_lock(c);
        c->initialized = was_initialized;
        hda_unlock(c, flags);
        return ret;
    }
    hda_release_resources(c);
    hda_zero(c, (uint32_t)sizeof(*c));
    return 0;
}

/** @brief Acquire the controller recovery gate for a PCM stream user.
 * @param c Initialized controller.
 * @return 0 or -EIO if transport is poisoned, -EOVERFLOW if the lease count is full.
 * Task context; no hardware reset occurs and auto-recovery becomes forbidden.
 *
 * @par Context
 * Task context when a PCM owner prepares to use the controller.
 * @par Ownership and locks/IRQ
 * Caller receives one stream-gate lease tracked by c; no hardware resource is transferred. Short state lock; IRQ-safe state change only.
 */
int hda_controller_stream_acquire(struct hda_controller *c)
{
    if (!c || !c->initialized) return -EINVAL;
    uint64_t flags = hda_lock(c);
    if (c->transport_poisoned) {
        hda_unlock(c, flags);
        return -EIO;
    }
    if (c->active_streams == UINT32_MAX) {
        hda_unlock(c, flags);
        return -EOVERFLOW;
    }
    ++c->active_streams;
    c->recovery_allowed = 0u;
    hda_unlock(c, flags);
    return 0;
}

/** @brief Release one previously acquired PCM stream recovery-gate lease.
 * @param c Initialized controller with at least one active stream lease.
 * @return 0 or -EINVAL when no lease exists. Does not implicitly allow recovery.
 * Task context; caller has stopped the stream DMA before releasing its lease.
 *
 * @par Context
 * Task context after the caller has stopped its PCM DMA engine.
 * @par Ownership and locks/IRQ
 * Returns one tracked stream-gate lease; controller DMA ownership remains unchanged. Short state lock and no reset/IRQ manipulation.
 */
int hda_controller_stream_release(struct hda_controller *c)
{
    if (!c || !c->initialized) return -EINVAL;
    uint64_t flags = hda_lock(c);
    if (!c->active_streams) {
        hda_unlock(c, flags);
        return -EINVAL;
    }
    --c->active_streams;
    hda_unlock(c, flags);
    return 0;
}

/** @brief Set the card generation token consumed by later repeated-failure handling.
 * @param c Initialized controller.
 * @param card Nonzero token returned by the audio card registration service.
 * @return 0 or -EINVAL for a zero token. Task context; token ownership stays external.
 *
 * @par Context
 * Task init/runtime context after a card token is registered.
 * @par Ownership and locks/IRQ
 * Card token remains owned by the audio core; c stores a copy. Short state lock; no callbacks or IRQ action.
 */
int hda_controller_set_card_id(struct hda_controller *c, uint32_t card)
{
    if (!c || !c->initialized || !card) return -EINVAL;
    uint64_t flags = hda_lock(c);
    c->card_id = card;
    if (c->consecutive_faults >= HDA_TIMEOUT_FAILURE_LIMIT)
        c->disconnect_requested = 1u;
    hda_unlock(c, flags);
    return 0;
}

/** @brief Consume a deferred disconnect request after repeated transport faults.
 * @param c Initialized controller.
 * @param out_card Receives a representative generation token for the controller.
 * @return 0 with token or -EAGAIN. IRQ/task safe; only consumes the flag.
 *
 * @par Context
 * IRQ or task context; the consumer must only queue task-context teardown.
 * @par Ownership and locks/IRQ
 * Returns a copied card token; core retains card ownership. Short state lock; never unregisters or calls clients in IRQ context.
 */
int hda_controller_take_disconnect_request(struct hda_controller *c,
                                           uint32_t *out_card)
{
    if (!c || !out_card) return -EINVAL;
    if (!c->initialized) return -ENODEV;
    uint64_t flags = hda_lock(c);
    if (!c->disconnect_requested || !c->card_id) {
        hda_unlock(c, flags);
        return -EAGAIN;
    }
    *out_card = c->card_id;
    c->disconnect_requested = 0u;
    hda_unlock(c, flags);
    return 0;
}
