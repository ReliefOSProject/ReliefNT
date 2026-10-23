#include "controller_internal.h"

/** @brief Disable local interrupts while retaining the caller's prior IF state.
 * @return Saved interrupt-enable bit. Safe in task and interrupt context.
 *
 * @par Context
 * Any kernel context. No controller resource ownership is transferred.
 * @par Ownership and locks/IRQ
 * No controller lock is acquired; saves only the caller CPU interrupt-enable state and is IRQ-entry safe.
 */
static uint64_t hda_irq_save(void);

/** @brief Restore only the saved local interrupt-enable state.
 * @param flags Saved IF bit from hda_irq_save.
 * @return None. Does not change the caller's other flag bits.
 *
 * @par Context
 * Any kernel context; caller owns the saved local interrupt token.
 * @par Ownership and locks/IRQ
 * No controller lock is acquired; restores only the saved IF bit and is IRQ-context safe.
 */
static void hda_irq_restore(uint64_t flags);


/** @brief Disable local interrupts while retaining the caller's prior IF state.
 * @return Saved interrupt-enable bit. Safe in task and interrupt context.
 *
 * @par Context
 * Any kernel context. No controller resource ownership is transferred.
 * @par Ownership and locks/IRQ
 * No controller lock is acquired; saves only the caller CPU interrupt-enable state and is IRQ-entry safe.
 */
static uint64_t hda_irq_save(void)
{
#ifdef HDA_TESTING
    return hda_test_irq_save();
#else
    uint64_t flags;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags & (1ull << 9);
#endif
}

/** @brief Restore only the saved local interrupt-enable state.
 * @param flags Saved IF bit from hda_irq_save.
 * @return None. Does not change the caller's other flag bits.
 *
 * @par Context
 * Any kernel context; caller owns the saved local interrupt token.
 * @par Ownership and locks/IRQ
 * No controller lock is acquired; restores only the saved IF bit and is IRQ-context safe.
 */
static void hda_irq_restore(uint64_t flags)
{
#ifdef HDA_TESTING
    hda_test_irq_restore(flags);
#else
    if (flags & (1ull << 9)) __asm__ volatile("sti" : : : "memory");
    else __asm__ volatile("cli" : : : "memory");
#endif
}

/** @brief Read one byte from the mapped controller register window.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte offset inside the 0x4000-byte register window.
 * @return Register value. Caller serializes shared state where required.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking. Width-specific access is IRQ-safe and does not wait.
 */
uint8_t hda_read8(const struct hda_controller *c, uint32_t offset)
{
#ifdef HDA_TESTING
    return hda_test_mmio_read8(c->mmio, offset);
#else
    return *(volatile uint8_t *)((uintptr_t)c->mmio + offset);
#endif
}

/** @brief Read one little-endian 16-bit controller register.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte offset aligned to two bytes.
 * @return Register value.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking. Width-specific access is IRQ-safe and does not wait.
 */
uint16_t hda_read16(const struct hda_controller *c, uint32_t offset)
{
#ifdef HDA_TESTING
    return hda_test_mmio_read16(c->mmio, offset);
#else
    return *(volatile uint16_t *)((uintptr_t)c->mmio + offset);
#endif
}

/** @brief Read one little-endian 32-bit controller register.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte offset aligned to four bytes.
 * @return Register value.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking. Width-specific access is IRQ-safe and does not wait.
 */
uint32_t hda_read32(const struct hda_controller *c, uint32_t offset)
{
#ifdef HDA_TESTING
    return hda_test_mmio_read32(c->mmio, offset);
#else
    return *(volatile uint32_t *)((uintptr_t)c->mmio + offset);
#endif
}

/** @brief Write one byte to a controller register without widening side effects.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte register offset.
 * @param value Byte written, including exact W1C bits when applicable.
 * @return None. Caller supplies serialization and required DMA barriers.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking and exact W1C values. Access is IRQ-safe and does not wait.
 */
void hda_write8(struct hda_controller *c, uint32_t offset, uint8_t value)
{
#ifdef HDA_TESTING
    hda_test_mmio_write8(c->mmio, offset, value);
#else
    *(volatile uint8_t *)((uintptr_t)c->mmio + offset) = value;
#endif
}

/** @brief Write one 16-bit controller register at its specified width.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte offset aligned to two bytes.
 * @param value 16-bit value; W1C registers receive only bits being cleared.
 * @return None.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking and exact W1C values. Access is IRQ-safe and does not wait.
 */
void hda_write16(struct hda_controller *c, uint32_t offset, uint16_t value)
{
#ifdef HDA_TESTING
    hda_test_mmio_write16(c->mmio, offset, value);
#else
    *(volatile uint16_t *)((uintptr_t)c->mmio + offset) = value;
#endif
}

/** @brief Write one 32-bit controller register at its specified width.
 * @param c Controller with an owned MMIO mapping.
 * @param offset Byte offset aligned to four bytes.
 * @param value 32-bit value.
 * @return None.
 *
 * @par Context
 * Task or IRQ context while the controller mapping lease is live.
 * @par Ownership and locks/IRQ
 * Controller retains the MMIO lease; caller supplies shared-state locking and exact W1C values. Access is IRQ-safe and does not wait.
 */
void hda_write32(struct hda_controller *c, uint32_t offset, uint32_t value)
{
#ifdef HDA_TESTING
    hda_test_mmio_write32(c->mmio, offset, value);
#else
    *(volatile uint32_t *)((uintptr_t)c->mmio + offset) = value;
#endif
}

/** @brief Read one stream/controller byte for the H4 stream engine.
 * @param c Initialized controller with live MMIO.
 * @param offset Register offset.
 * @return Register value; no wait or allocation.
 */
uint8_t hda_mmio_read8(const struct hda_controller *c, uint32_t offset)
{
    return hda_read8(c, offset);
}

/** @brief Read one stream/controller 16-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @return Register value; no wait or allocation.
 */
uint16_t hda_mmio_read16(const struct hda_controller *c, uint32_t offset)
{
    return hda_read16(c, offset);
}

/** @brief Read one stream/controller 32-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @return Register value; no wait or allocation.
 */
uint32_t hda_mmio_read32(const struct hda_controller *c, uint32_t offset)
{
    return hda_read32(c, offset);
}

/** @brief Write one stream/controller byte for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Register offset.
 * @param value Exact byte, including W1C bits.
 * @return None; no wait or allocation.
 */
void hda_mmio_write8(struct hda_controller *c, uint32_t offset, uint8_t value)
{
    hda_write8(c, offset, value);
}

/** @brief Write one stream/controller 16-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @param value Exact register value.
 * @return None; no wait or allocation.
 */
void hda_mmio_write16(struct hda_controller *c, uint32_t offset, uint16_t value)
{
    hda_write16(c, offset, value);
}

/** @brief Write one stream/controller 32-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @param value Exact register value.
 * @return None; no wait or allocation.
 */
void hda_mmio_write32(struct hda_controller *c, uint32_t offset, uint32_t value)
{
    hda_write32(c, offset, value);
}

/** @brief Return the checked CPU alias for one caller-owned physical page.
 * @param phys Page-aligned physical page address; this helper does not prove ownership.
 * @return CPU direct-map address or NULL when the page is misaligned/outside the window.
 * The caller must already own the page and retain exact reclamation responsibility.
 *
 * @par Context
 * Task context while the caller's page lease is live.
 * @par Ownership and locks/IRQ
 * Creates no allocation, mapping lease, or ownership transfer; no lock, wait, or IRQ action.
 */
void *hda_phys_to_direct_map_page(uint64_t phys)
{
#ifdef HDA_TESTING
    return hda_test_dma_map(phys);
#else
    const struct reliefos_driver_kernel_api *api = hda_module_kernel_api();
    if ((phys & (HDA_DMA_PAGE_BYTES - 1u)) || !api || !api->phys_to_virt)
        return NULL;
    return api->phys_to_virt(phys);
#endif
}

/** @brief Acquire the controller state lock with local interrupts saved.
 * @param c Controller whose short state critical section is being entered.
 * @return Prior IF bit, passed unchanged to hda_unlock.
 * No task wait or device polling is permitted while held.
 *
 * @par Context
 * Task or IRQ context while controller storage remains alive.
 * @par Ownership and locks/IRQ
 * The caller owns the returned IF token and must pair it with unlock. This acquires the controller spinlock with local IRQs disabled; IRQ-safe, may spin on another CPU.
 */
uint64_t hda_lock(struct hda_controller *c)
{
    uint64_t flags = hda_irq_save();
    while (__atomic_exchange_n(&c->lock.held, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("pause");
    }
    return flags;
}

/** @brief Release the controller state lock and restore local interrupt state.
 * @param c Controller whose lock is held by the caller.
 * @param flags Prior IF bit returned by hda_lock.
 * @return None. Call only after all shared-state changes are published.
 *
 * @par Context
 * Task or IRQ context; caller currently owns the controller spinlock and saved IF token.
 * @par Ownership and locks/IRQ
 * Releases the controller spinlock and restores local IF. No allocation, wait, or callback; IRQ-safe.
 */
void hda_unlock(struct hda_controller *c, uint64_t flags)
{
    __atomic_store_n(&c->lock.held, 0u, __ATOMIC_RELEASE);
    hda_irq_restore(flags);
}

/** @brief Sleep through the real H1 clock API while preserving the caller's IF.
 * @param c Initialized controller with ticks/sleep_ms services.
 * @param milliseconds Minimum requested duration; H1 rounds up to 100 Hz ticks.
 * @return None. Lock must be released; H1's sleep returns with IF clear, so this
 * wrapper restores the entry IF and leaves IRQ progress available during the wait.
 *
 * @par Context
 * Task context only while API/controller lifetime is held.
 * @par Ownership and locks/IRQ
 * Caller retains controller/API ownership; no controller lock may be held. H1 permits IRQ progress during sleep and the wrapper restores the caller IF state.
 */
void hda_sleep_ms(struct hda_controller *c, uint64_t milliseconds)
{
    uint64_t flags = hda_irq_save();
    hda_irq_restore(flags);
#ifdef HDA_TESTING
    hda_test_before_sleep(c);
#endif
    c->api->sleep_ms(milliseconds);
    hda_irq_restore(flags);
}

/** @brief Export the controller's reviewed task-wait wrapper to codec logic.
 * @param c Initialized controller with a live task-context sleep service.
 * @param milliseconds Requested bounded duration.
 * @return None; invalid helper prerequisites are ignored without dereference.
 *
 * @par Context
 * Task context only while controller/API lifetime is held; never IRQ/service
 * or global-execution-owned context.
 * @par Ownership and locks/IRQ
 * Caller retains controller/API ownership and holds no controller spinlock.
 * Reuses hda_sleep_ms, which permits IRQ progress and restores entry IF.
 */
void hda_controller_sleep_ms(struct hda_controller *c, uint64_t milliseconds)
{
    if (!c || !c->api || !c->api->sleep_ms) return;
    hda_sleep_ms(c, milliseconds);
}

/** @brief Return true once a monotonic H1 tick deadline has elapsed.
 * @param c Initialized controller with a monotonic ticks callback.
 * @param start Tick value sampled before the bounded wait.
 * @param duration Number of 100 Hz ticks in the deadline.
 * @return True when unsigned elapsed ticks reach duration.
 *
 * @par Context
 * Any context with a live borrowed ticks callback.
 * @par Ownership and locks/IRQ
 * No resource ownership or lock; bounded read-only tick comparison is IRQ-safe.
 */
bool hda_ticks_elapsed(const struct hda_controller *c, uint64_t start,
                              uint64_t duration)
{
    return c->api->ticks() - start >= duration;
}

/** @brief Poll a width-specific register until its masked state matches.
 * @param c Initialized mapped controller.
 * @param offset Register byte offset.
 * @param width Register access width in bytes (1, 2, or 4).
 * @param mask Bits considered by the comparison.
 * @param expected Expected masked value.
 * @param timeout_ticks Maximum 100 Hz tick delta.
 * @return 0 when matched or -ETIMEDOUT. Task context; every wait sleeps without
 * holding the controller lock and retains IRQ progress.
 *
 * @par Context
 * Task context only with a live mapped controller.
 * @par Ownership and locks/IRQ
 * Caller retains the controller mapping; no controller lock is held while waiting. Sleep permits IRQ progress; not IRQ-callable.
 */
int hda_wait_register(struct hda_controller *c, uint32_t offset,
                             uint32_t width, uint32_t mask, uint32_t expected,
                             uint64_t timeout_ticks)
{
    uint64_t start = c->api->ticks();
    for (;;) {
        uint32_t value = width == 1u ? hda_read8(c, offset) :
                         width == 2u ? hda_read16(c, offset) : hda_read32(c, offset);
        if ((value & mask) == expected) return 0;
        if (hda_ticks_elapsed(c, start, timeout_ticks)) return -ETIMEDOUT;
        hda_sleep_ms(c, 1u);
    }
}

/** @brief Write a PCI config command word through the size-gated H1 service.
 * @param c Controller with a valid PCI identity and API.
 * @param command New command bits; using a 16-bit write avoids adjacent W1C status.
 * @return None.
 *
 * @par Context
 * Task context during PCI controller init or teardown.
 * @par Ownership and locks/IRQ
 * Caller retains the PCI function identity/API. No controller lock; uses a 16-bit config write and is not IRQ-callable.
 */
void hda_pci_write_command(struct hda_controller *c, uint16_t command)
{
    c->api->pci_write16(c->dev.bus, c->dev.slot, c->dev.function,
                        HDA_PCI_COMMAND, command);
}

/** @brief Find and size the first usable memory BAR while the PCI function is quiesced.
 * @param c Controller with saved PCI identity and API.
 * @param out_base Receives the complete 64-bit BAR base without truncation.
 * @param out_size Receives the decoded BAR byte size.
 * @return 0, -ENODEV for no suitable BAR, or -EIO for a failed config transaction.
 * Task init context; disables decode/master before probing and restores every BAR
 * value plus the original 16-bit command on all exits.
 *
 * @par Context
 * Task init context after the caller quiesces the PCI function.
 * @par Ownership and locks/IRQ
 * Controller retains the borrowed PCI identity/API; BAR values and original command are restored before return. No controller lock; not IRQ-callable.
 */
int hda_find_bar(struct hda_controller *c, uint64_t *out_base,
                        uint64_t *out_size)
{
    const struct reliefos_driver_kernel_api *api = c->api;
    uint16_t original = api->pci_read16(c->dev.bus, c->dev.slot, c->dev.function,
                                        HDA_PCI_COMMAND);
    c->pci_command_original = original;
    c->pci_command_valid = 1u;
    hda_pci_write_command(c, (uint16_t)(original & (uint16_t)~HDA_PCI_COMMAND_DECODE));
    if (api->pci_read16(c->dev.bus, c->dev.slot, c->dev.function,
                        HDA_PCI_COMMAND) & HDA_PCI_COMMAND_DECODE) {
        hda_pci_write_command(c, original);
        return -EIO;
    }

    int result = -ENODEV;
    for (uint32_t index = 0; index < 6u; ++index) {
        uint8_t offset = (uint8_t)(HDA_PCI_BAR0 + index * 4u);
        uint32_t low = api->pci_read32(c->dev.bus, c->dev.slot, c->dev.function, offset);
        if (!low) continue;
        if (low & 1u) continue;
        uint32_t type = (low >> 1) & 3u;
        bool is_64 = type == 2u;
        if (type != 0u && !is_64) continue;
        if (is_64 && index == 5u) continue;
        uint32_t high = is_64 ? api->pci_read32(c->dev.bus, c->dev.slot,
                                                 c->dev.function,
                                                 (uint8_t)(offset + 4u)) : 0u;

        api->pci_write32(c->dev.bus, c->dev.slot, c->dev.function, offset, UINT32_MAX);
        if (is_64) api->pci_write32(c->dev.bus, c->dev.slot, c->dev.function,
                                    (uint8_t)(offset + 4u), UINT32_MAX);
        uint32_t mask_low = api->pci_read32(c->dev.bus, c->dev.slot,
                                            c->dev.function, offset);
        uint32_t mask_high = is_64 ? api->pci_read32(c->dev.bus, c->dev.slot,
                                                      c->dev.function,
                                                      (uint8_t)(offset + 4u)) : 0u;
        if (is_64) api->pci_write32(c->dev.bus, c->dev.slot, c->dev.function,
                                    (uint8_t)(offset + 4u), high);
        api->pci_write32(c->dev.bus, c->dev.slot, c->dev.function, offset, low);
        if (!mask_low && !mask_high) {
            if (is_64) ++index;
            continue;
        }

        uint64_t base = ((uint64_t)high << 32) | (uint64_t)(low & ~0x0fu);
        uint64_t mask = ((uint64_t)mask_high << 32) |
                        (uint64_t)(mask_low & ~0x0fu);
        uint64_t size = is_64 ? (~mask + 1u) :
                        (uint64_t)((uint32_t)(~(uint32_t)mask + 1u));
        if (!size || (base & (HDA_DMA_PAGE_BYTES - 1u)) ||
            size < HDA_REGISTER_BYTES || base > UINT64_MAX - size) {
            if (is_64) ++index;
            continue;
        }
        *out_base = base;
        *out_size = size;
        result = 0;
        break;
    }
    hda_pci_write_command(c, original);
    return result;
}

/** @brief Pack codec address and verb fields in the HDA long or short layout.
 * @param cad Codec address.
 * @param nid Codec node identifier.
 * @param verb Verb selector.
 * @param payload Verb payload.
 * @param short_verb True selects the short verb form.
 * @return One 32-bit CORB command.
 *
 * @par Context
 * Any context; pure value conversion.
 * @par Ownership and locks/IRQ
 * No resource ownership, locks, or IRQ state; safe to call from IRQ context.
 */
uint32_t hda_encode_verb(uint8_t cad, uint8_t nid, uint16_t verb,
                         uint16_t payload, bool short_verb)
{
    uint32_t base = ((uint32_t)cad << 28) | ((uint32_t)nid << 20);
    return short_verb ? base | ((uint32_t)(verb & 15u) << 16) | payload :
           base | ((uint32_t)(verb & 4095u) << 8) | (payload & 255u);
}

/** @brief Zero a fixed byte range without importing a hosted libc dependency.
 * @param memory Writable start of the region.
 * @param bytes Exact byte count.
 * @return None. Used only while DMA is stopped or before the ring is published.
 *
 * @par Context
 * Task init/recovery context while the caller owns the writable buffer.
 * @par Ownership and locks/IRQ
 * Caller retains buffer/DMA ownership; no lock or IRQ manipulation. Not used from interrupt context.
 */
void hda_zero(void *memory, uint32_t bytes)
{
    uint8_t *out = memory;
    for (uint32_t i = 0; i < bytes; ++i) out[i] = 0;
}

/** @brief Decode the controller's supported ring sizes and select the largest.
 * @param capability Raw 8-bit CORBSIZE or RIRBSIZE register value.
 * @param out_entries Receives 2, 16, or 256 entries.
 * @param out_select Receives the low-two-bit hardware selector.
 * @return 0 or -ENODEV when no supported size is advertised.
 *
 * @par Context
 * Any context; pure capability decode.
 * @par Ownership and locks/IRQ
 * No resource ownership, locks, or IRQ state; safe from IRQ context.
 */
int hda_choose_ring_size(uint8_t capability, uint16_t *out_entries,
                                uint8_t *out_select)
{
    uint8_t caps = (uint8_t)(capability >> 4);
    if (caps & 4u) {
        *out_entries = 256u;
        *out_select = 2u;
        return 0;
    }
    if (caps & 2u) {
        *out_entries = 16u;
        *out_select = 1u;
        return 0;
    }
    if (caps & 1u) {
        *out_entries = 2u;
        *out_select = 0u;
        return 0;
    }
    return -ENODEV;
}
