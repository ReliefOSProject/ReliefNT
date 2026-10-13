#include "hda.h"

/* H4 supplies the strong implementation when the stream object is linked;
 * controller-only host fixtures retain a harmless no-stream fallback. */
#if !defined(HDA_STREAM_TESTING)
__attribute__((weak)) uint32_t hda_stream_service_locked(struct hda_controller *c,
                                                         uint32_t budget)
{
    (void)c;
    (void)budget;
    return 0u;
}
#endif

#include <linux/errno.h>
#include <reliefnt/paging.h>

#define HDA_REG_GCAP       0x00u
#define HDA_REG_GCTL       0x08u
#define HDA_REG_WAKEEN     0x0cu
#define HDA_REG_STATESTS   0x0eu
#define HDA_REG_INTCTL     0x20u
#define HDA_REG_INTSTS     0x24u
#define HDA_REG_WALCLK     0x30u
#define HDA_REG_CORBLBASE  0x40u
#define HDA_REG_CORBUBASE  0x44u
#define HDA_REG_CORBWP     0x48u
#define HDA_REG_CORBRP     0x4au
#define HDA_REG_CORBCTL    0x4cu
#define HDA_REG_CORBSTS    0x4du
#define HDA_REG_CORBSIZE   0x4eu
#define HDA_REG_RIRBLBASE  0x50u
#define HDA_REG_RIRBUBASE  0x54u
#define HDA_REG_RIRBWP     0x58u
#define HDA_REG_RINTCNT    0x5au
#define HDA_REG_RIRBCTL    0x5cu
#define HDA_REG_RIRBSTS    0x5du
#define HDA_REG_RIRBSIZE   0x5eu
#define HDA_REG_DPLBASE    0x70u
#define HDA_REG_DPUBASE    0x74u
#define HDA_REG_ICOI       0x60u
#define HDA_REG_ICII       0x64u
#define HDA_REG_ICIS       0x68u
#define HDA_STREAM_BASE    0x80u
#define HDA_STREAM_STRIDE  0x20u

#define HDA_GCTL_CRST              (1u << 0)
#define HDA_GCTL_UNSOL             (1u << 8)
#define HDA_ICIS_ICB               (1u << 0)
#define HDA_ICIS_IRV               (1u << 1)
#define HDA_ICIS_ICVER             (1u << 2)
#define HDA_ICIS_IRRUNSOL          (1u << 3)
#define HDA_ICIS_IRRADD_MASK       (0x0fu << 4)
#define HDA_GCAP_64OK              (1u << 0)
#define HDA_CORB_MEMORY_ERROR_IRQ  (1u << 0)
#define HDA_CORB_RUN               (1u << 1)
#define HDA_CORB_RP_RESET          (1u << 15)
#define HDA_CORB_STATUS_ERROR      (1u << 0)
#define HDA_RIRB_DMA_ENABLE        (1u << 1)
#define HDA_RIRB_RESPONSE_IRQ      (1u << 0)
#define HDA_RIRB_OVERRUN_IRQ       (1u << 2)
#define HDA_RIRB_WP_RESET          (1u << 15)
#define HDA_RIRB_STATUS_RESPONSE   (1u << 0)
#define HDA_RIRB_STATUS_OVERRUN    (1u << 2)
#define HDA_RIRB_RESPONSE_UNSOL    (1ull << 4)
#define HDA_RIRB_CODEC_MASK        0x0fu
#define HDA_INTCTL_CONTROLLER      (1u << 30)
#define HDA_INTCTL_GLOBAL          (1u << 31)
#define HDA_PCI_COMMAND            0x04u
#define HDA_PCI_BAR0               0x10u
#define HDA_PCI_COMMAND_IO         (1u << 0)
#define HDA_PCI_COMMAND_MEMORY     (1u << 1)
#define HDA_PCI_COMMAND_MASTER     (1u << 2)
#define HDA_PCI_COMMAND_DECODE     (HDA_PCI_COMMAND_IO | HDA_PCI_COMMAND_MEMORY | HDA_PCI_COMMAND_MASTER)
#define HDA_DMA_PAGE_BYTES         4096u
#define HDA_RESET_TIMEOUT_TICKS    10u
#define HDA_COMMAND_TIMEOUT_TICKS  10u
#define HDA_RECOVERY_STABLE_TICKS  2u
#define HDA_TIMEOUT_FAILURE_LIMIT  3u
#define HDA_LONG_STABILIZE_MS      21u

#ifdef HDA_TESTING
extern uint8_t hda_test_mmio_read8(void *base, uint32_t offset);
extern uint16_t hda_test_mmio_read16(void *base, uint32_t offset);
extern uint32_t hda_test_mmio_read32(void *base, uint32_t offset);
extern void hda_test_mmio_write8(void *base, uint32_t offset, uint8_t value);
extern void hda_test_mmio_write16(void *base, uint32_t offset, uint16_t value);
extern void hda_test_mmio_write32(void *base, uint32_t offset, uint32_t value);
extern void *hda_test_dma_map(uint64_t phys);
extern uint64_t hda_test_irq_save(void);
extern void hda_test_irq_restore(uint64_t flags);
extern void hda_test_before_sleep(const struct hda_controller *c);
#endif

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
static uint8_t hda_read8(const struct hda_controller *c, uint32_t offset)
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
static uint16_t hda_read16(const struct hda_controller *c, uint32_t offset)
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
static uint32_t hda_read32(const struct hda_controller *c, uint32_t offset)
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
static void hda_write8(struct hda_controller *c, uint32_t offset, uint8_t value)
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
static void hda_write16(struct hda_controller *c, uint32_t offset, uint16_t value)
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
static void hda_write32(struct hda_controller *c, uint32_t offset, uint32_t value)
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
    if ((phys & (HDA_DMA_PAGE_BYTES - 1u)) ||
        phys > RELIEFNT_KERNEL_DIRECT_MAP_SIZE - HDA_DMA_PAGE_BYTES) return NULL;
    return (void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + phys);
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
static uint64_t hda_lock(struct hda_controller *c)
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
static void hda_unlock(struct hda_controller *c, uint64_t flags)
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
static void hda_sleep_ms(struct hda_controller *c, uint64_t milliseconds)
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
static bool hda_ticks_elapsed(const struct hda_controller *c, uint64_t start,
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
static int hda_wait_register(struct hda_controller *c, uint32_t offset,
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
static void hda_pci_write_command(struct hda_controller *c, uint16_t command)
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
static int hda_find_bar(struct hda_controller *c, uint64_t *out_base,
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
static void hda_zero(void *memory, uint32_t bytes)
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
static int hda_choose_ring_size(uint8_t capability, uint16_t *out_entries,
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

/** @brief Toggle global controller reset and confirm each requested state.
 * @param c Mapped controller with PCI memory decode active.
 * @param released True requests CRST readback set; false requests reset asserted.
 * @return 0 after the requested reset transition or -ETIMEDOUT.
 * Task context; reset polling releases the local controller lock and permits IRQs.
 *
 * @par Context
 * Task context with mapped MMIO and enabled API lifetime.
 * @par Ownership and locks/IRQ
 * Controller owns the mapping; no state lock is held during the bounded poll and IRQ progress continues. Not IRQ-callable.
 */
static int hda_set_crst(struct hda_controller *c, bool released)
{
    uint32_t control = hda_read32(c, HDA_REG_GCTL);
    control = released ? (control | HDA_GCTL_CRST) : (control & ~HDA_GCTL_CRST);
    hda_write32(c, HDA_REG_GCTL, control);
    return hda_wait_register(c, HDA_REG_GCTL, 4u, HDA_GCTL_CRST,
                             released ? HDA_GCTL_CRST : 0u,
                             HDA_RESET_TIMEOUT_TICKS);
}

/** @brief Reset the controller link and wait conservatively for codec startup.
 * @param c Mapped controller with ring DMA stopped.
 * @return 0 when CRST is released and codecs have had time to initialize, else
 * -ETIMEDOUT. Task context; 21 ms spans at least two complete 100 Hz tick edges.
 *
 * @par Context
 * Task init/recovery context with controller DMA stopped.
 * @par Ownership and locks/IRQ
 * Controller owns MMIO and ring leases; no lock is held over stabilization sleep, and IRQ progress continues. Not IRQ-callable.
 */
static int hda_reset_link(struct hda_controller *c)
{
    hda_write16(c, HDA_REG_WAKEEN, 0u);
    int ret = hda_set_crst(c, false);
    if (ret) return ret;
    ret = hda_set_crst(c, true);
    if (ret) return ret;
    hda_sleep_ms(c, HDA_LONG_STABILIZE_MS);
    hda_write16(c, HDA_REG_WAKEEN, 0u);
    uint16_t states = hda_read16(c, HDA_REG_STATESTS);
    c->codec_mask = states;
    if (states) hda_write16(c, HDA_REG_STATESTS, states);
    return 0;
}

/** @brief Disable CORB/RIRB and interrupts, proving the DMA run bits are clear.
 * @param c Mapped controller.
 * @return 0 when both DMA engines are stopped or -ETIMEDOUT.
 * Task context; no IRQ is required to observe the stop handshake.
 *
 * @par Context
 * Task init/teardown/recovery context.
 * @par Ownership and locks/IRQ
 * Controller owns MMIO and ring leases; no lock is held while polling run bits. IRQ delivery is not required; not IRQ-callable.
 */
static int hda_stop_rings(struct hda_controller *c)
{
    hda_write32(c, HDA_REG_INTCTL, 0u);
    hda_write8(c, HDA_REG_CORBCTL,
               (uint8_t)(hda_read8(c, HDA_REG_CORBCTL) & (uint8_t)~HDA_CORB_RUN));
    hda_write8(c, HDA_REG_RIRBCTL,
               (uint8_t)(hda_read8(c, HDA_REG_RIRBCTL) &
                         (uint8_t)~HDA_RIRB_DMA_ENABLE));
    int ret = hda_wait_register(c, HDA_REG_CORBCTL, 1u, HDA_CORB_RUN, 0u,
                                HDA_RESET_TIMEOUT_TICKS);
    if (ret) return ret;
    ret = hda_wait_register(c, HDA_REG_RIRBCTL, 1u, HDA_RIRB_DMA_ENABLE, 0u,
                            HDA_RESET_TIMEOUT_TICKS);
    if (ret) return ret;
    c->rings_started = 0;
    return 0;
}

/** @brief Reset the software-visible CORB read pointer using its hardware handshake.
 * @param c Mapped controller with CORB DMA stopped.
 * @return 0 after observing assert and clear, or -ETIMEDOUT.
 * Task context; ring pointer register uses 16-bit accesses.
 *
 * @par Context
 * Task init/recovery context with CORB DMA stopped.
 * @par Ownership and locks/IRQ
 * Controller owns MMIO/ring leases; no state lock during bounded polling. IRQ progress continues; not IRQ-callable.
 */
static int hda_reset_corb_read_pointer(struct hda_controller *c)
{
    hda_write16(c, HDA_REG_CORBRP, HDA_CORB_RP_RESET);
    int ret = hda_wait_register(c, HDA_REG_CORBRP, 2u, HDA_CORB_RP_RESET,
                                HDA_CORB_RP_RESET, HDA_RESET_TIMEOUT_TICKS);
    if (ret) return ret;
    hda_write16(c, HDA_REG_CORBRP, 0u);
    return hda_wait_register(c, HDA_REG_CORBRP, 2u, HDA_CORB_RP_RESET, 0u,
                             HDA_RESET_TIMEOUT_TICKS);
}

/** @brief Reset the RIRB hardware write pointer while response DMA is stopped.
 * @param c Mapped controller with RIRB DMA stopped.
 * @return 0 when hardware reports pointer zero or -EIO.
 * The reset bit is write-only; its readback is expected to remain clear.
 *
 * @par Context
 * Task init/recovery context with RIRB DMA stopped.
 * @par Ownership and locks/IRQ
 * Controller owns MMIO/ring leases; no state lock is required. IRQ-safe register width but used only in task reset flow.
 */
static int hda_reset_rirb_write_pointer(struct hda_controller *c)
{
    hda_write16(c, HDA_REG_RIRBWP, HDA_RIRB_WP_RESET);
    return (hda_read16(c, HDA_REG_RIRBWP) & 0xffu) == 0u ? 0 : -EIO;
}

/** @brief Publish initialized CORB/RIRB storage and start both ring engines.
 * @param c Controller with owned, initialized ring storage and reset pointers.
 * @return 0 when both engines read back running, otherwise -EIO.
 * The caller must have observed CRST high/stable and both engines stopped.
 *
 * @par Context
 * Task init without the state lock, or bounded recovery with the state lock held.
 * @par Ownership and locks/IRQ
 * Ring leases and MMIO remain owned by c. Performs fixed-width MMIO only, no wait,
 * allocation, callback, or sleep; the recovery caller already has local IRQs disabled.
 */
static int hda_publish_ring_registers(struct hda_controller *c)
{
    hda_write32(c, HDA_REG_INTCTL, 0u);
    hda_write16(c, HDA_REG_CORBWP, 0u);
    hda_write32(c, HDA_REG_CORBLBASE, (uint32_t)c->corb_phys);
    hda_write32(c, HDA_REG_CORBUBASE, (uint32_t)(c->corb_phys >> 32));
    hda_write8(c, HDA_REG_CORBSIZE,
               (uint8_t)(c->corb_size_caps | c->corb_size_select));
    hda_write32(c, HDA_REG_RIRBLBASE, (uint32_t)c->rirb_phys);
    hda_write32(c, HDA_REG_RIRBUBASE, (uint32_t)(c->rirb_phys >> 32));
    hda_write8(c, HDA_REG_RIRBSIZE,
               (uint8_t)(c->rirb_size_caps | c->rirb_size_select));
    hda_write16(c, HDA_REG_RINTCNT, 1u);
    if (c->owns_stream_dma && c->position_phys) {
        hda_write32(c, HDA_REG_DPLBASE,
                    (uint32_t)(c->position_phys & ~UINT64_C(0x7f)) | 1u);
        hda_write32(c, HDA_REG_DPUBASE, (uint32_t)(c->position_phys >> 32));
    } else {
        hda_write32(c, HDA_REG_DPLBASE, 0u);
        hda_write32(c, HDA_REG_DPUBASE, 0u);
    }

    uint8_t corb_status = hda_read8(c, HDA_REG_CORBSTS);
    uint8_t rirb_status = hda_read8(c, HDA_REG_RIRBSTS);
    if (corb_status & HDA_CORB_STATUS_ERROR)
        hda_write8(c, HDA_REG_CORBSTS,
                   (uint8_t)(corb_status & HDA_CORB_STATUS_ERROR));
    if (rirb_status & (HDA_RIRB_STATUS_RESPONSE | HDA_RIRB_STATUS_OVERRUN))
        hda_write8(c, HDA_REG_RIRBSTS,
                   (uint8_t)(rirb_status &
                             (HDA_RIRB_STATUS_RESPONSE | HDA_RIRB_STATUS_OVERRUN)));

    __atomic_thread_fence(__ATOMIC_RELEASE);
    hda_write8(c, HDA_REG_RIRBCTL,
               HDA_RIRB_DMA_ENABLE | HDA_RIRB_RESPONSE_IRQ |
               (c->owns_irq ? HDA_RIRB_OVERRUN_IRQ : 0u));
    /* Response status must be generated even in polling mode: acknowledging
     * it resets RINTCNT (and QEMU's command admission count). INTCTL.GIE/CIE
     * remain clear without an IRQ lease, so no unhandled INTx is enabled. */
    if (!(hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE)) return -EIO;
    hda_write8(c, HDA_REG_CORBCTL,
               (uint8_t)(HDA_CORB_RUN |
                         (c->owns_irq ? HDA_CORB_MEMORY_ERROR_IRQ : 0u)));
    if (!(hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN)) return -EIO;

    c->rings_started = 1u;
    hda_write32(c, HDA_REG_GCTL, hda_read32(c, HDA_REG_GCTL) | HDA_GCTL_UNSOL);
    if (c->owns_irq) {
        /* H2 handles CORB CMEI/RIRB/wake; H4 adds SDIE with a real SDSTS handler. */
        hda_write32(c, HDA_REG_INTCTL,
                    HDA_INTCTL_GLOBAL | HDA_INTCTL_CONTROLLER);
    }
    return 0;
}

/** @brief Program ring sizes, DMA bases, pointer state, status, and run controls.
 * @param c Mapped controller with owned, zeroed ring pages.
 * @return 0 when both ring engines are running, otherwise a negative errno.
 * Task init/recovery context; all register accesses use the documented widths.
 *
 * @par Context
 * Task init/recovery context while ring DMA engines are stopped.
 * @par Ownership and locks/IRQ
 * Controller owns ring pages and MMIO; no controller lock over bounded handshakes. May allocate during init; not IRQ-callable.
 */
static int hda_program_rings(struct hda_controller *c)
{
    uint8_t corb_caps = hda_read8(c, HDA_REG_CORBSIZE);
    uint8_t rirb_caps = hda_read8(c, HDA_REG_RIRBSIZE);
    int ret = hda_choose_ring_size(corb_caps, &c->corb_entries,
                                  &c->corb_size_select);
    if (ret) return ret;
    ret = hda_choose_ring_size(rirb_caps, &c->rirb_entries,
                               &c->rirb_size_select);
    if (ret) return ret;
    c->corb_size_caps = (uint8_t)(corb_caps & 0xf0u);
    c->rirb_size_caps = (uint8_t)(rirb_caps & 0xf0u);

    if (!c->owns_corb) {
        uint64_t mask = (c->gcap & HDA_GCAP_64OK) ? UINT64_MAX : UINT32_MAX;
        c->corb_phys = c->api->alloc_dma(1u, mask);
        if (!c->corb_phys) return -ENOMEM;
        c->owns_corb = 1u;
        c->corb_pages = 1u;
        if ((c->corb_phys & (HDA_DMA_PAGE_BYTES - 1u)) ||
            c->corb_phys > mask - (HDA_DMA_PAGE_BYTES - 1u)) return -ERANGE;
        c->corb = hda_phys_to_direct_map_page(c->corb_phys);
        if (!c->corb) return -ENOMEM;
    }
    if (!c->owns_rirb) {
        uint64_t mask = (c->gcap & HDA_GCAP_64OK) ? UINT64_MAX : UINT32_MAX;
        c->rirb_phys = c->api->alloc_dma(1u, mask);
        if (!c->rirb_phys) return -ENOMEM;
        c->owns_rirb = 1u;
        c->rirb_pages = 1u;
        if ((c->rirb_phys & (HDA_DMA_PAGE_BYTES - 1u)) ||
            c->rirb_phys > mask - (HDA_DMA_PAGE_BYTES - 1u)) return -ERANGE;
        c->rirb = hda_phys_to_direct_map_page(c->rirb_phys);
        if (!c->rirb) return -ENOMEM;
    }

    hda_zero((void *)c->corb, HDA_DMA_PAGE_BYTES);
    hda_zero((void *)c->rirb, HDA_DMA_PAGE_BYTES);
    c->corb_write_pointer = 0u;
    c->rirb_read_pointer = 0u;
    c->unsolicited_head = 0u;
    c->unsolicited_count = 0u;

    hda_write8(c, HDA_REG_CORBCTL,
               (uint8_t)(hda_read8(c, HDA_REG_CORBCTL) & (uint8_t)~HDA_CORB_RUN));
    hda_write8(c, HDA_REG_RIRBCTL,
               (uint8_t)(hda_read8(c, HDA_REG_RIRBCTL) &
                         (uint8_t)~HDA_RIRB_DMA_ENABLE));
    ret = hda_wait_register(c, HDA_REG_CORBCTL, 1u, HDA_CORB_RUN, 0u,
                            HDA_RESET_TIMEOUT_TICKS);
    if (ret) return ret;
    ret = hda_wait_register(c, HDA_REG_RIRBCTL, 1u, HDA_RIRB_DMA_ENABLE, 0u,
                            HDA_RESET_TIMEOUT_TICKS);
    if (ret) return ret;
    ret = hda_reset_corb_read_pointer(c);
    if (ret) return ret;
    ret = hda_reset_rirb_write_pointer(c);
    if (ret) return ret;

    return hda_publish_ring_registers(c);
}

/** @brief Disable PCI bus mastering while preserving the device's other command bits.
 * @param c Controller with a saved PCI identity and valid command service.
 * @return 0 when bus mastering reads back disabled, otherwise -EIO.
 * Task context, after ring engines have stopped or controller reset has completed.
 *
 * @par Context
 * Task teardown context after ring stop or controller reset.
 * @par Ownership and locks/IRQ
 * Controller retains PCI config access; no controller lock. Not IRQ-callable.
 */
static int hda_disable_bus_master(struct hda_controller *c)
{
    uint16_t command = c->api->pci_read16(c->dev.bus, c->dev.slot,
                                          c->dev.function, HDA_PCI_COMMAND);
    hda_pci_write_command(c, (uint16_t)(command & (uint16_t)~HDA_PCI_COMMAND_MASTER));
    command = c->api->pci_read16(c->dev.bus, c->dev.slot,
                                 c->dev.function, HDA_PCI_COMMAND);
    return (command & HDA_PCI_COMMAND_MASTER) ? -EIO : 0;
}

/** @brief Release IRQ synchronization and ring storage after DMA is quiescent.
 * @param c Controller whose ring engines are stopped or held in reset.
 * @return None. Frees optional IRQ and owned RIRB/CORB pages.
 * Task init-fallback or teardown context only.
 *
 * @par Context
 * Task context after ring stop/readback or controller reset proves DMA quiet.
 * @par Ownership and locks/IRQ
 * Caller owns the quiescence proof. IRQ is synchronized before DMA leases return;
 * controller MMIO and PCI configuration leases are retained.
 */
static void hda_release_ring_resources(struct hda_controller *c)
{
    if (c->owns_irq) {
        c->api->free_pci_irq(c->irq_handle);
        c->owns_irq = 0u;
    }
    if (c->owns_rirb) {
        c->api->free_dma(c->rirb_phys, c->rirb_pages);
        c->owns_rirb = 0u;
    }
    if (c->owns_corb) {
        c->api->free_dma(c->corb_phys, c->corb_pages);
        c->owns_corb = 0u;
    }
}

/** @brief Release every controller lease after hardware DMA has been proven quiet.
 * @param c Controller carrying owned IRQ, DMA, MMIO, and PCI command state.
 * @return None. IRQ synchronization precedes freeing ring memory, then MMIO is
 * unmapped and the original PCI command is restored.
 *
 * @par Context
 * Task teardown context after DMA quiescence is proven.
 * @par Ownership and locks/IRQ
 * Releases controller-owned IRQ, DMA, MMIO, and PCI-command leases in order; no controller lock, and never called by an IRQ.
 */
static void hda_release_resources(struct hda_controller *c)
{
    hda_release_ring_resources(c);
    if (c->owns_mmio) {
        c->api->unmap_mmio(c->mmio, HDA_REGISTER_BYTES);
        c->owns_mmio = 0u;
    }
    if (c->pci_command_valid) {
        hda_pci_write_command(c, c->pci_command_original);
        c->pci_command_valid = 0u;
    }
}

/** @brief Stop ring engines or force a controller reset before releasing DMA.
 * @param c Controller with mapped MMIO and a live PCI command service.
 * @return 0 only when run bits are clear or CRST readback proves controller reset;
 * otherwise returns a timeout and the caller must retain all leases.
 * Task context, bounded by reset deadlines and independent of IRQ delivery.
 *
 * @par Context
 * Task init-failure/teardown context.
 * @par Ownership and locks/IRQ
 * Controller retains all leases until run-bit stop or CRST proves quiescence; no lock across waits, IRQ delivery is not required.
 */
static int hda_quiesce_dma(struct hda_controller *c)
{
    if (!c->owns_mmio) return 0;
    hda_write32(c, HDA_REG_INTCTL, 0u);
    int ret = hda_stop_rings(c);
    if (!ret) return hda_disable_bus_master(c);

    int reset_ret = hda_set_crst(c, false);
    if (reset_ret) return reset_ret;
    c->rings_started = 0u;
    return hda_disable_bus_master(c);
}

/** @brief Enter restricted PIO mode after ring-init hardware failure.
 * @param c Partially initialized controller with a live MMIO/PCI lease.
 * @return 0 after a real link reset proves stale ICIS state clear, otherwise a
 * negative stop/reset/status errno; failed proof leaves all uncertain leases owned.
 * Task-init context only; ordinary command timeouts never call this fallback.
 *
 * @par Context
 * Task init after CORB/RIRB setup failed with a hardware capability/handshake error.
 * @par Ownership and locks/IRQ
 * Requires ring stop or reset and disabled PCI bus mastering before freeing IRQ and
 * ring leases. Restores PCI bus mastering for PCM only after the stopped command
 * rings have been released. Retains MMIO/PCI decode; no lock spans reset waits.
 */
static int hda_enter_immediate_mode(struct hda_controller *c)
{
    if (!c->api->console_write) return -EOPNOTSUPP;
    int ret = hda_quiesce_dma(c);
    if (ret) return ret;
    ret = hda_reset_link(c);
    if (ret) return ret;

    hda_write32(c, HDA_REG_INTCTL, 0u);
    hda_write16(c, HDA_REG_WAKEEN, 0u);
    hda_write32(c, HDA_REG_GCTL,
                hda_read32(c, HDA_REG_GCTL) & ~HDA_GCTL_UNSOL);
    if (hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_UNSOL) return -EIO;
    uint16_t state_status = hda_read16(c, HDA_REG_STATESTS);
    if (state_status) hda_write16(c, HDA_REG_STATESTS, state_status);

    uint16_t immediate_status = hda_read16(c, HDA_REG_ICIS);
    c->immediate_version = (immediate_status & HDA_ICIS_ICVER) ? 1u : 0u;
    if (immediate_status & HDA_ICIS_ICB) return -EIO;
    if (immediate_status & HDA_ICIS_IRV) {
        hda_write16(c, HDA_REG_ICIS, HDA_ICIS_IRV);
        immediate_status = hda_read16(c, HDA_REG_ICIS);
    }
    if (immediate_status & (HDA_ICIS_ICB | HDA_ICIS_IRV)) return -EIO;
    if ((hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN) ||
        (hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE)) return -EIO;

    hda_release_ring_resources(c);
    c->corb = NULL;
    c->rirb = NULL;
    c->corb_phys = 0u;
    c->rirb_phys = 0u;
    c->corb_pages = 0u;
    c->rirb_pages = 0u;
    c->corb_entries = 0u;
    c->rirb_entries = 0u;
    c->corb_size_caps = 0u;
    c->rirb_size_caps = 0u;
    c->corb_size_select = 0u;
    c->rirb_size_select = 0u;
    c->corb_write_pointer = 0u;
    c->rirb_read_pointer = 0u;
    c->rings_started = 0u;
    /* Immediate codec commands use PIO, but PCM/position buffers still use DMA.
     * Quiescence disabled bus mastering to release the failed command rings;
     * their engines are now stopped and their leases have been returned. */
    uint16_t command = c->api->pci_read16(c->dev.bus, c->dev.slot,
                                          c->dev.function, HDA_PCI_COMMAND);
    hda_pci_write_command(c, (uint16_t)(command | HDA_PCI_COMMAND_MASTER));
    if (!(c->api->pci_read16(c->dev.bus, c->dev.slot, c->dev.function,
                            HDA_PCI_COMMAND) & HDA_PCI_COMMAND_MASTER)) return -EIO;
    c->transport_mode = HDA_TRANSPORT_IMMEDIATE;
    c->api->console_write(
        "[hda] ring init failed; using immediate command transport (no unsolicited)\n");
    return 0;
}

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

/** @brief Record a transport fault and poison command association until hardware reset.
 * @param c Controller whose state lock is held.
 * @param error Retained error for the current flight, if it is still pending.
 * @return None. Active PCM blocks automatic reset; recovery state is advanced only
 * by bounded service or an explicit task-context recovery call.
 *
 * @par Context
 * Task, bounded-service, or IRQ caller while the controller state lock is held.
 * @par Ownership and locks/IRQ
 * Controller state remains owned; the helper only records poison/fault and schedules recovery. Local IRQs are disabled by the held lock; no wait or callback.
 */
static void hda_poison_locked(struct hda_controller *c, int error)
{
    c->transport_poisoned = 1u;
    if (c->flight.ticket && c->flight.pending) {
        c->flight.pending = 0u;
        c->flight.complete = 1u;
        c->flight.error = error;
    }
    if (c->consecutive_faults != UINT32_MAX) ++c->consecutive_faults;
    /* An active PCM lease prevents transparent recovery. Further submissions
     * are rejected while poisoned, so waiting for three new faults would leave
     * that controller permanently wedged without queuing its task teardown. */
    if ((c->active_streams || c->consecutive_faults >= HDA_TIMEOUT_FAILURE_LIMIT) && c->card_id)
        c->disconnect_requested = 1u;
    if (c->active_streams || !c->recovery_allowed) {
        c->recovery_requires_task = 1u;
        c->recovery_state = HDA_RECOVERY_IDLE;
        return;
    }
    if (c->recovery_state == HDA_RECOVERY_IDLE ||
        c->recovery_state == HDA_RECOVERY_FAILED) {
        c->recovery_state = HDA_RECOVERY_STOP_RINGS;
        c->recovery_start_tick = c->api->ticks();
        c->recovery_step = 0u;
        c->recovery_error = 0;
    }
}

/** @brief Mark a nonrecoverable bounded-service reset failure while holding the lock.
 * @param c Controller whose recovery state is being advanced.
 * @param error Negative errno describing the failed reset handshake.
 * @return None. Poison remains set and ring leases remain owned for task retry.
 *
 * @par Context
 * Task, bounded-service, or IRQ caller while the controller state lock is held.
 * @par Ownership and locks/IRQ
 * DMA leases stay owned by c. Lock is already held with local IRQs disabled; records failure only and never waits.
 */
static void hda_recovery_failed_locked(struct hda_controller *c, int error)
{
    c->recovery_error = error;
    c->recovery_state = HDA_RECOVERY_FAILED;
    c->transport_poisoned = 1u;
    c->rings_started = 0u;
    if(c->card_id)c->disconnect_requested=1u;
}

/** @brief Advance one nonblocking controller/link/ring recovery step under lock.
 * @param c Poisoned controller; caller holds the state lock with local IRQs saved.
 * @return None. No sleep, allocation, callback, or unbounded register polling occurs.
 *
 * @par Context
 * Bounded service context while the controller state lock is held.
 * @par Ownership and locks/IRQ
 * Controller retains MMIO and ring pages. Lock is already held; each call has bounded MMIO work and is IRQ-safe without sleep/allocation/callback.
 */
static void hda_advance_recovery_locked(struct hda_controller *c)
{
    uint32_t control;
    switch (c->recovery_state) {
    case HDA_RECOVERY_STOP_RINGS:
        if (!c->recovery_step) {
            hda_write32(c, HDA_REG_INTCTL, 0u);
            hda_write16(c, HDA_REG_WAKEEN, 0u);
            hda_write8(c, HDA_REG_CORBCTL,
                       (uint8_t)(hda_read8(c, HDA_REG_CORBCTL) &
                                 (uint8_t)~HDA_CORB_RUN));
            hda_write8(c, HDA_REG_RIRBCTL,
                       (uint8_t)(hda_read8(c, HDA_REG_RIRBCTL) &
                                 (uint8_t)~HDA_RIRB_DMA_ENABLE));
            c->recovery_step = 1u;
            c->recovery_start_tick = c->api->ticks();
            return;
        }
        if (hda_ticks_elapsed(c, c->recovery_start_tick,
                              HDA_RESET_TIMEOUT_TICKS)) {
            hda_recovery_failed_locked(c, -ETIMEDOUT);
            return;
        }
        if ((hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN) ||
            (hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE)) return;
        c->rings_started = 0u;
        control = hda_read32(c, HDA_REG_GCTL) & ~HDA_GCTL_CRST;
        hda_write32(c, HDA_REG_GCTL, control);
        c->recovery_state = HDA_RECOVERY_WAIT_CRST_LOW;
        c->recovery_step = 0u;
        c->recovery_start_tick = c->api->ticks();
        return;
    case HDA_RECOVERY_WAIT_CRST_LOW:
        if (hda_ticks_elapsed(c, c->recovery_start_tick,
                              HDA_RESET_TIMEOUT_TICKS)) {
            hda_recovery_failed_locked(c, -ETIMEDOUT);
            return;
        }
        if (hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_CRST) return;
        control = hda_read32(c, HDA_REG_GCTL) | HDA_GCTL_CRST;
        hda_write32(c, HDA_REG_GCTL, control);
        c->recovery_state = HDA_RECOVERY_WAIT_CRST_HIGH;
        c->recovery_start_tick = c->api->ticks();
        return;
    case HDA_RECOVERY_WAIT_CRST_HIGH:
        if (hda_ticks_elapsed(c, c->recovery_start_tick,
                              HDA_RESET_TIMEOUT_TICKS)) {
            hda_recovery_failed_locked(c, -ETIMEDOUT);
            return;
        }
        if (!(hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_CRST)) return;
        c->recovery_state = HDA_RECOVERY_WAIT_STABLE;
        c->recovery_start_tick = c->api->ticks();
        return;
    case HDA_RECOVERY_WAIT_STABLE:
        if (!(hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_CRST)) {
            hda_recovery_failed_locked(c, -EIO);
            return;
        }
        if (!hda_ticks_elapsed(c, c->recovery_start_tick,
                               HDA_RECOVERY_STABLE_TICKS)) return;
        hda_write16(c, HDA_REG_WAKEEN, 0u);
        c->codec_mask = hda_read16(c, HDA_REG_STATESTS);
        if (c->codec_mask)
            hda_write16(c, HDA_REG_STATESTS, c->codec_mask);
        if (c->transport_mode == HDA_TRANSPORT_IMMEDIATE) {
            c->recovery_state = HDA_RECOVERY_IMMEDIATE_CLEAR;
            c->recovery_step = 0u;
            c->recovery_start_tick = c->api->ticks();
            return;
        }
        c->recovery_state = HDA_RECOVERY_RESET_RINGS;
        c->recovery_step = 0u;
        c->recovery_start_tick = c->api->ticks();
        return;
    case HDA_RECOVERY_IMMEDIATE_CLEAR: {
        if (hda_ticks_elapsed(c, c->recovery_start_tick,
                              HDA_RESET_TIMEOUT_TICKS)) {
            hda_recovery_failed_locked(c, -ETIMEDOUT);
            return;
        }
        uint16_t status = hda_read16(c, HDA_REG_ICIS);
        if (((status & HDA_ICIS_ICVER) != 0u) != (c->immediate_version != 0u)) {
            hda_recovery_failed_locked(c, -EIO);
            return;
        }
        if (status & HDA_ICIS_ICB) return;
        if (status & HDA_ICIS_IRV) {
            hda_write16(c, HDA_REG_ICIS, HDA_ICIS_IRV);
            status = hda_read16(c, HDA_REG_ICIS);
            if (status & HDA_ICIS_IRV) return;
        }
        hda_write32(c, HDA_REG_INTCTL, 0u);
        hda_write16(c, HDA_REG_WAKEEN, 0u);
        hda_write32(c, HDA_REG_GCTL,
                    hda_read32(c, HDA_REG_GCTL) & ~HDA_GCTL_UNSOL);
        if ((hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_UNSOL) ||
            (hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN) ||
            (hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE)) {
            hda_recovery_failed_locked(c, -EIO);
            return;
        }
        uint16_t state_status = hda_read16(c, HDA_REG_STATESTS);
        if (state_status) hda_write16(c, HDA_REG_STATESTS, state_status);
        c->transport_poisoned = 0u;
        c->recovery_requires_task = 0u;
        c->recovery_state = HDA_RECOVERY_IDLE;
        c->recovery_step = 0u;
        c->recovery_error = 0;
        return;
    }
    case HDA_RECOVERY_RESET_RINGS:
        if (hda_ticks_elapsed(c, c->recovery_start_tick,
                              HDA_RESET_TIMEOUT_TICKS)) {
            hda_recovery_failed_locked(c, -ETIMEDOUT);
            return;
        }
        if (c->recovery_step == 0u) {
            if ((hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN) ||
                (hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE)) {
                hda_write8(c, HDA_REG_CORBCTL,
                           (uint8_t)(hda_read8(c, HDA_REG_CORBCTL) &
                                     (uint8_t)~HDA_CORB_RUN));
                hda_write8(c, HDA_REG_RIRBCTL,
                           (uint8_t)(hda_read8(c, HDA_REG_RIRBCTL) &
                                     (uint8_t)~HDA_RIRB_DMA_ENABLE));
                return;
            }
            hda_write16(c, HDA_REG_CORBRP, HDA_CORB_RP_RESET);
            c->recovery_step = 1u;
            return;
        }
        if (c->recovery_step == 1u) {
            if (!(hda_read16(c, HDA_REG_CORBRP) & HDA_CORB_RP_RESET)) return;
            hda_write16(c, HDA_REG_CORBRP, 0u);
            c->recovery_step = 2u;
            return;
        }
        if (c->recovery_step == 2u) {
            if (hda_read16(c, HDA_REG_CORBRP) & HDA_CORB_RP_RESET) return;
            hda_write16(c, HDA_REG_RIRBWP, HDA_RIRB_WP_RESET);
            c->recovery_step = 3u;
            return;
        }
        if (hda_read16(c, HDA_REG_RIRBWP) & 0xffu) return;

        c->corb_write_pointer = 0u;
        c->rirb_read_pointer = 0u;
        c->unsolicited_head = 0u;
        c->unsolicited_count = 0u;
        int ret = hda_publish_ring_registers(c);
        if (ret) {
            hda_recovery_failed_locked(c, -EIO);
            return;
        }
        c->transport_poisoned = 0u;
        c->recovery_requires_task = 0u;
        c->recovery_state = HDA_RECOVERY_IDLE;
        c->recovery_step = 0u;
        c->recovery_error = 0;
        return;
    case HDA_RECOVERY_FAILED:
    case HDA_RECOVERY_IDLE:
    default:
        return;
    }
}

/** @brief Queue an unsolicited response, dropping the oldest entry on overflow.
 * @param c Controller whose lock is held.
 * @param cad Codec address from the RIRB extension.
 * @param response Codec response payload.
 * @return None. Fixed storage only; increments the dropped counter on overflow.
 *
 * @par Context
 * Task, bounded-service, or IRQ context while the controller state lock is held.
 * @par Ownership and locks/IRQ
 * Fixed queue remains owned by c; lock is already held with local IRQs disabled. No allocation or callback.
 */
static void hda_queue_unsolicited_locked(struct hda_controller *c, uint8_t cad,
                                         uint32_t response)
{
    if (c->unsolicited_count == HDA_UNSOLICITED_CAPACITY) {
        c->unsolicited_head = (c->unsolicited_head + 1u) % HDA_UNSOLICITED_CAPACITY;
        --c->unsolicited_count;
        if (c->unsolicited_dropped != UINT32_MAX) ++c->unsolicited_dropped;
    }
    uint32_t tail = (c->unsolicited_head + c->unsolicited_count) %
                    HDA_UNSOLICITED_CAPACITY;
    c->unsolicited[tail].cad = cad;
    c->unsolicited[tail].reserved[0] = 0u;
    c->unsolicited[tail].reserved[1] = 0u;
    c->unsolicited[tail].reserved[2] = 0u;
    c->unsolicited[tail].response = response;
    ++c->unsolicited_count;
}

/** @brief Service one immediate-command completion without waiting.
 * @param c Immediate-mode controller with its state lock held.
 * @return None. Reads/clears at most one ICIS response and completes only a
 * solicited response matching the active ticket's CAD when ICVER is implemented.
 *
 * @par Context
 * Bounded task/audio-service context with local IRQs disabled by the state lock.
 * @par Ownership and locks/IRQ
 * No ring leases or command generation are used; c owns the single flight. The
 * ICVER=0 path never inspects reserved CAD/unsolicited fields; no wait/allocation.
 */
static void hda_service_immediate_locked(struct hda_controller *c)
{
    if (c->transport_mode != HDA_TRANSPORT_IMMEDIATE || c->transport_poisoned) return;
    uint16_t status = hda_read16(c, HDA_REG_ICIS);
    if (((status & HDA_ICIS_ICVER) != 0u) != (c->immediate_version != 0u)) {
        hda_poison_locked(c, -EIO);
        return;
    }
    if (!(status & HDA_ICIS_IRV) || (status & HDA_ICIS_ICB)) return;

    uint32_t response = hda_read32(c, HDA_REG_ICII);
    bool matched = c->flight.ticket && c->flight.pending;
    if (matched && c->immediate_version) {
        uint8_t cad = (uint8_t)((status & HDA_ICIS_IRRADD_MASK) >> 4);
        if ((status & HDA_ICIS_IRRUNSOL) || cad != c->flight.cad) matched = false;
    }
    hda_write16(c, HDA_REG_ICIS, HDA_ICIS_IRV);
    status = hda_read16(c, HDA_REG_ICIS);
    if (status & HDA_ICIS_IRV) {
        hda_poison_locked(c, -EIO);
        return;
    }
    if (matched) {
        c->flight.response = response;
        c->flight.error = 0;
        c->flight.pending = 0u;
        c->flight.complete = 1u;
        c->consecutive_faults = 0u;
    } else if (c->unexpected_responses != UINT32_MAX) {
        ++c->unexpected_responses;
    }
}

/** @brief Consume a bounded number of RIRB entries and advance recovery once.
 * @param c Initialized controller with active MMIO and ring or immediate transport.
 * @param budget Maximum entries to consume; values above HDA_SERVICE_BUDGET clamp.
 * @return Number of response entries consumed. IRQ/tick safe, bounded, nonblocking,
 * and serialized against submit/poll state without waiting on hardware.
 *
 * @par Context
 * Task/audio-service/tick or hard IRQ context for an initialized controller.
 * @par Ownership and locks/IRQ
 * Controller retains mode-specific transport ownership; it acquires a short state lock and returns before any wait. IRQ-safe, bounded, allocation-free, and callback-free.
 */
uint32_t hda_controller_service_budget(struct hda_controller *c, uint32_t budget)
{
    if (!c || !c->initialized) return 0u;
    if (budget > HDA_SERVICE_BUDGET) budget = HDA_SERVICE_BUDGET;
    uint64_t flags = hda_lock(c);
    uint32_t consumed = 0u;

    uint8_t corb_status = hda_read8(c, HDA_REG_CORBSTS);
    uint8_t rirb_status = hda_read8(c, HDA_REG_RIRBSTS);
    if (corb_status & HDA_CORB_STATUS_ERROR)
        hda_write8(c, HDA_REG_CORBSTS,
                   (uint8_t)(corb_status & HDA_CORB_STATUS_ERROR));
    if (rirb_status & (HDA_RIRB_STATUS_RESPONSE | HDA_RIRB_STATUS_OVERRUN))
        hda_write8(c, HDA_REG_RIRBSTS,
                   (uint8_t)(rirb_status &
                             (HDA_RIRB_STATUS_RESPONSE | HDA_RIRB_STATUS_OVERRUN)));
    uint16_t state_status = hda_read16(c, HDA_REG_STATESTS);
    if (state_status) hda_write16(c, HDA_REG_STATESTS, state_status);

    if (corb_status & HDA_CORB_STATUS_ERROR) hda_poison_locked(c, -EIO);
    if (rirb_status & HDA_RIRB_STATUS_OVERRUN) hda_poison_locked(c, -EOVERFLOW);
    if (c->flight.ticket && c->flight.pending &&
        hda_ticks_elapsed(c, c->flight.start_tick, HDA_COMMAND_TIMEOUT_TICKS))
        hda_poison_locked(c, -ETIMEDOUT);
    if (!c->transport_poisoned && c->transport_mode == HDA_TRANSPORT_IMMEDIATE && budget)
        hda_service_immediate_locked(c);
    if (!c->transport_poisoned && c->rings_started &&
        !(corb_status & HDA_CORB_STATUS_ERROR) &&
        !(rirb_status & HDA_RIRB_STATUS_OVERRUN) && budget) {
        uint16_t hardware_wp = (uint16_t)(hda_read16(c, HDA_REG_RIRBWP) & 0xffu);
        if (hardware_wp >= c->rirb_entries) {
            hda_poison_locked(c, -EIO);
        } else {
            while (c->rirb_read_pointer != hardware_wp && consumed < budget) {
                c->rirb_read_pointer =
                    (uint16_t)((c->rirb_read_pointer + 1u) % c->rirb_entries);
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                uint64_t entry = c->rirb[c->rirb_read_pointer];
                uint32_t response = (uint32_t)entry;
                uint32_t extension = (uint32_t)(entry >> 32);
                uint8_t cad = (uint8_t)(extension & HDA_RIRB_CODEC_MASK);
                if (extension & HDA_RIRB_RESPONSE_UNSOL) {
                    hda_queue_unsolicited_locked(c, cad, response);
                } else if (c->flight.ticket && c->flight.pending &&
                           c->flight.cad == cad) {
                    c->flight.response = response;
                    c->flight.error = 0;
                    c->flight.pending = 0u;
                    c->flight.complete = 1u;
                    c->consecutive_faults = 0u;
                } else if (c->unexpected_responses != UINT32_MAX) {
                    ++c->unexpected_responses;
                }
                ++consumed;
            }
        }
    }

    if (consumed < budget)
        consumed += hda_stream_service_locked(c, budget - consumed);

    hda_advance_recovery_locked(c);
    hda_unlock(c, flags);
    return consumed;
}

/** @brief Run the standard bounded HDA service pass.
 * @param c Initialized controller.
 * @return Entries consumed, capped by HDA_SERVICE_BUDGET. IRQ-safe and nonblocking.
 *
 * @par Context
 * Task/audio-service/tick or hard IRQ context for an initialized controller.
 * @par Ownership and locks/IRQ
 * Controller retains mode-specific transport ownership; the bounded helper owns synchronization. IRQ-safe and nonblocking.
 */
uint32_t hda_controller_service(struct hda_controller *c)
{
    return hda_controller_service_budget(c, HDA_SERVICE_BUDGET);
}

/** @brief Submit a codec command through the controller's single shared flight.
 * @param c Initialized and unpoisoned controller.
 * @param cad Codec address in 0..15.
 * @param nid Codec node identifier.
 * @param verb Verb selector.
 * @param payload Verb payload.
 * @param short_verb True selects 4-bit verb and 16-bit payload form.
 * @param out_ticket Receives a monotonically increasing one-consumer ticket.
 * @return 0 or negative errno including -EBUSY, -EIO, -ENOSPC, and -EINVAL.
 * Task/service context; never sleeps or allocates and releases the lock before return.
 *
 * @par Context
 * Task or bounded nonblocking audio-service context; not the hard IRQ handler.
 * @par Ownership and locks/IRQ
 * Controller retains mode-specific transport leases; returned ticket has one polling owner. A short local-IRQ-safe lock publishes CORB or ICOI state; no wait or allocation.
 */
int hda_verb_submit(struct hda_controller *c, uint8_t cad, uint8_t nid,
                    uint16_t verb, uint16_t payload, bool short_verb,
                    uint64_t *out_ticket)
{
    if (!c || !out_ticket || cad >= HDA_CODEC_COUNT) return -EINVAL;
    if (!c->initialized) return -ENODEV;
    uint64_t flags = hda_lock(c);
    if (c->transport_poisoned) {
        hda_unlock(c, flags);
        return -EIO;
    }
    if (c->transport_mode == HDA_TRANSPORT_RINGS &&
        (!c->rings_started || !c->corb_entries)) {
        hda_unlock(c, flags);
        return -EIO;
    }
    if (c->transport_mode == HDA_TRANSPORT_IMMEDIATE) {
        if (c->rings_started || c->owns_corb || c->owns_rirb ||
            (hda_read8(c, HDA_REG_CORBCTL) & HDA_CORB_RUN) ||
            (hda_read8(c, HDA_REG_RIRBCTL) & HDA_RIRB_DMA_ENABLE) ||
            (hda_read32(c, HDA_REG_GCTL) & HDA_GCTL_UNSOL)) {
            hda_unlock(c, flags);
            return -EIO;
        }
        uint16_t status = hda_read16(c, HDA_REG_ICIS);
        if (((status & HDA_ICIS_ICVER) != 0u) != (c->immediate_version != 0u)) {
            hda_unlock(c, flags);
            return -EIO;
        }
        if (status & HDA_ICIS_ICB) {
            hda_unlock(c, flags);
            return -EBUSY;
        }
        /* IRV may be the still-unclaimed completion owned by this flight. */
        if (c->flight.ticket) {
            hda_unlock(c, flags);
            return -EBUSY;
        }
        if (status & HDA_ICIS_IRV) {
            hda_write16(c, HDA_REG_ICIS, HDA_ICIS_IRV);
            status = hda_read16(c, HDA_REG_ICIS);
            if ((status & (HDA_ICIS_IRV | HDA_ICIS_ICB)) ||
                (((status & HDA_ICIS_ICVER) != 0u) != (c->immediate_version != 0u))) {
                hda_unlock(c, flags);
                return (status & HDA_ICIS_ICB) ? -EBUSY : -EIO;
            }
        }
    } else if (c->transport_mode != HDA_TRANSPORT_RINGS) {
        hda_unlock(c, flags);
        return -EIO;
    }
    if (c->flight.ticket) {
        hda_unlock(c, flags);
        return -EBUSY;
    }
    if (!c->next_ticket) {
        hda_unlock(c, flags);
        return -ENOSPC;
    }
    uint16_t next_wp = 0u;
    if (c->transport_mode == HDA_TRANSPORT_RINGS) {
        next_wp = (uint16_t)((c->corb_write_pointer + 1u) % c->corb_entries);
        uint16_t hardware_rp = (uint16_t)(hda_read16(c, HDA_REG_CORBRP) & 0xffu);
        if (hardware_rp >= c->corb_entries || next_wp == hardware_rp) {
            hda_unlock(c, flags);
            return -EBUSY;
        }
    }

    uint64_t ticket = c->next_ticket;
    c->next_ticket = ticket == UINT64_MAX ? 0u : ticket + 1u;
    c->flight.ticket = ticket;
    c->flight.start_tick = c->api->ticks();
    c->flight.response = 0u;
    c->flight.error = 0;
    c->flight.cad = cad;
    c->flight.pending = 1u;
    c->flight.complete = 0u;
    uint32_t encoded = hda_encode_verb(cad, nid, verb, payload, short_verb);
    if (c->transport_mode == HDA_TRANSPORT_RINGS) {
        c->corb[next_wp] = encoded;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        hda_write16(c, HDA_REG_CORBWP, next_wp);
        c->corb_write_pointer = next_wp;
    } else {
        hda_write32(c, HDA_REG_ICOI, encoded);
        __atomic_thread_fence(__ATOMIC_RELEASE);
        hda_write16(c, HDA_REG_ICIS, HDA_ICIS_ICB);
    }
    *out_ticket = ticket;
    hda_unlock(c, flags);
    return 0;
}

/** @brief Poll and consume the completion owned by one command ticket.
 * @param c Initialized controller.
 * @param ticket Nonzero ticket previously returned by submit.
 * @param out_response Receives the successful codec response.
 * @return 0, -EAGAIN, the retained command/transport error, or -ENOENT for a stale ticket.
 * Task/service context; one poll consumes a completed ticket and its error exactly once.
 *
 * @par Context
 * Task or bounded nonblocking audio-service context.
 * @par Ownership and locks/IRQ
 * Ticket owner consumes completion/error exactly once; controller retains its own leases. Short state lock only; no wait, allocation, or IRQ dependence.
 */
int hda_verb_poll(struct hda_controller *c, uint64_t ticket,
                  uint32_t *out_response)
{
    if (!c || !ticket || !out_response) return -EINVAL;
    if (!c->initialized) return -ENODEV;
    uint64_t flags = hda_lock(c);
    if (c->flight.ticket != ticket) {
        hda_unlock(c, flags);
        return -ENOENT;
    }
    if (c->flight.pending &&
        hda_ticks_elapsed(c, c->flight.start_tick, HDA_COMMAND_TIMEOUT_TICKS))
        hda_poison_locked(c, -ETIMEDOUT);
    if (c->flight.pending) {
        hda_unlock(c, flags);
        return -EAGAIN;
    }
    int result = c->flight.error;
    if (!result) *out_response = c->flight.response;
    c->flight.ticket = 0u;
    c->flight.start_tick = 0u;
    c->flight.response = 0u;
    c->flight.error = 0;
    c->flight.cad = 0u;
    c->flight.pending = 0u;
    c->flight.complete = 0u;
    hda_unlock(c, flags);
    return result;
}

/** @brief Execute one serialized codec command, servicing and sleeping only unlocked.
 * @param c Initialized controller.
 * @param cad Codec address in 0..15.
 * @param nid Codec node identifier.
 * @param verb Verb selector.
 * @param payload Verb payload.
 * @param short_verb True selects the short form.
 * @param out_response Receives the solicited response.
 * @return 0 or negative errno. A failed command is consumed once; task recovery is
 * attempted after a transport fault only when the stream gate permits it.
 * Task context without execution ownership; lock is dropped for every wait/service.
 *
 * @par Context
 * Task context only, outside execution ownership.
 * @par Ownership and locks/IRQ
 * Controller retains transport resources; caller owns only the temporary ticket until completion. Locks are dropped before service/sleep/recovery; IRQ progress remains enabled.
 */
int hda_exec_verb(struct hda_controller *c, uint8_t cad, uint8_t nid,
                  uint16_t verb, uint16_t payload, bool short_verb,
                  uint32_t *out_response)
{
    if (!c || !out_response) return -EINVAL;
    uint64_t wait_start = c->api ? c->api->ticks() : 0u;
    uint64_t ticket = 0u;
    for (;;) {
        int ret = hda_verb_submit(c, cad, nid, verb, payload, short_verb, &ticket);
        if (!ret) break;
        if (ret != -EBUSY) return ret;
        if (hda_ticks_elapsed(c, wait_start, HDA_COMMAND_TIMEOUT_TICKS)) return -ETIMEDOUT;
        (void)hda_controller_service(c);
        hda_sleep_ms(c, 1u);
    }

    for (;;) {
        (void)hda_controller_service(c);
        int ret = hda_verb_poll(c, ticket, out_response);
        if (ret != -EAGAIN) {
            if (ret == -ETIMEDOUT || ret == -EOVERFLOW || ret == -EIO)
                (void)hda_controller_recover(c);
            return ret;
        }
        hda_sleep_ms(c, 1u);
    }
}

/** @brief Pop the oldest unsolicited response from the bounded fixed-size queue.
 * @param c Initialized controller.
 * @param out Receives the codec address and 32-bit response.
 * @return 0 or -EAGAIN when empty. Task context; no allocation and short lock only.
 *
 * @par Context
 * Task/audio-service context for an initialized controller.
 * @par Ownership and locks/IRQ
 * Controller owns the fixed queue; the caller receives a copied event only. Short state lock; no IRQ manipulation or wait.
 */
int hda_unsolicited_pop(struct hda_controller *c,
                        struct hda_unsolicited_response *out)
{
    if (!c || !out) return -EINVAL;
    if (!c->initialized) return -ENODEV;
    uint64_t flags = hda_lock(c);
    if (!c->unsolicited_count) {
        hda_unlock(c, flags);
        return -EAGAIN;
    }
    *out = c->unsolicited[c->unsolicited_head];
    c->unsolicited_head = (c->unsolicited_head + 1u) % HDA_UNSOLICITED_CAPACITY;
    --c->unsolicited_count;
    hda_unlock(c, flags);
    return 0;
}

/** @brief Remove the oldest event owned by one codec, preserving other CADs.
 * @param c Initialized controller owning the shared unsolicited queue.
 * @param cad Codec address in 0..15 whose oldest event may be removed.
 * @param out Non-NULL caller storage receiving one copied event on success only.
 * @return 0, -EAGAIN when this CAD has no event, -EINVAL for invalid arguments,
 * or -ENODEV when the controller is not initialized.
 * @par Context Task or bounded audio-service context; never allocates or waits.
 * @par Ownership and locks/IRQ Takes the IRQ-safe controller state lock for a
 * fixed-capacity scan/compaction. Other codecs retain their events in FIFO order.
 */
int hda_unsolicited_pop_for_codec(struct hda_controller *c, uint8_t cad,
                                  struct hda_unsolicited_response *out)
{
    if (!c || !out || cad > 15u) return -EINVAL;
    if (!c->initialized) return -ENODEV;
    uint64_t flags = hda_lock(c);
    for (uint32_t i = 0u; i < c->unsolicited_count; ++i) {
        uint32_t index = (c->unsolicited_head + i) % HDA_UNSOLICITED_CAPACITY;
        if (c->unsolicited[index].cad != cad) continue;
        *out = c->unsolicited[index];
        for (uint32_t j = i + 1u; j < c->unsolicited_count; ++j) {
            uint32_t next = (c->unsolicited_head + j) % HDA_UNSOLICITED_CAPACITY;
            c->unsolicited[index] = c->unsolicited[next];
            index = next;
        }
        --c->unsolicited_count;
        hda_unlock(c, flags);
        return 0;
    }
    hda_unlock(c, flags);
    return -EAGAIN;
}

/** @brief Read the bounded unsolicited-response overflow count.
 * @param c Initialized controller whose unsolicited queue is live.
 * @return Monotonic dropped-response count, or zero for an invalid/uninitialized controller.
 *
 * @par Context
 * Task or bounded audio-service context; the value is sampled without waiting.
 * @par Ownership and locks/IRQ
 * Controller retains the queue; this helper holds only the short state lock and
 * performs no allocation, MMIO, or synchronous verb.
 */
uint32_t hda_unsolicited_dropped_count(struct hda_controller *c)
{
    if (!c || !c->initialized) return 0u;
    uint64_t flags = hda_lock(c);
    uint32_t dropped = c->unsolicited_dropped;
    hda_unlock(c, flags);
    return dropped;
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

/** @brief Set the explicit full-controller recovery permission after stream quiesce.
 * @param c Initialized controller.
 * @param allowed True only after every PCM DMA engine/route has stopped.
 * @return 0 or -EBUSY when allowing recovery with live stream leases.
 * Task context; this changes only the gate and never resets hardware itself.
 *
 * @par Context
 * Task context after stream quiescence is known.
 * @par Ownership and locks/IRQ
 * No resource ownership transfer; caller authorizes a controller-wide reset. Short state lock; no reset or IRQ callback.
 */
int hda_controller_set_recovery_allowed(struct hda_controller *c, bool allowed)
{
    if (!c || !c->initialized) return -EINVAL;
    uint64_t flags = hda_lock(c);
    if (allowed && c->active_streams) {
        hda_unlock(c, flags);
        return -EBUSY;
    }
    c->recovery_allowed = allowed ? 1u : 0u;
    if (allowed && c->transport_poisoned &&
        c->recovery_state == HDA_RECOVERY_IDLE) {
        c->recovery_requires_task = 1u;
    }
    hda_unlock(c, flags);
    return 0;
}

/** @brief Drive an actual link and ring reset after the stream owner permits it.
 * @param c Initialized poisoned controller with no active stream users.
 * @return 0 after hardware CRST/ring reset, -EBUSY when the gate forbids recovery,
 * or the bounded reset error. Task context only; waits through bounded service ticks.
 *
 * @par Context
 * Task context after all stream users have stopped and recovery is explicitly allowed.
 * @par Ownership and locks/IRQ
 * Controller retains its DMA pages throughout reset. Short lock sections only; bounded service advances each phase while IRQ progress is permitted.
 */
int hda_controller_recover(struct hda_controller *c)
{
    if (!c || !c->initialized) return -EINVAL;
    uint64_t flags = hda_lock(c);
    if (c->active_streams || !c->recovery_allowed) {
        hda_unlock(c, flags);
        return -EBUSY;
    }
    if (!c->transport_poisoned) {
        hda_unlock(c, flags);
        return 0;
    }
    if (c->recovery_state == HDA_RECOVERY_FAILED ||
        c->recovery_state == HDA_RECOVERY_IDLE) {
        c->recovery_state = HDA_RECOVERY_STOP_RINGS;
        c->recovery_start_tick = c->api->ticks();
        c->recovery_step = 0u;
        c->recovery_error = 0;
        c->recovery_requires_task = 1u;
    }
    uint64_t start = c->api->ticks();
    hda_unlock(c, flags);

    while (!hda_ticks_elapsed(c, start, HDA_RESET_TIMEOUT_TICKS * 3u)) {
        (void)hda_controller_service(c);
        flags = hda_lock(c);
        bool recovered = !c->transport_poisoned &&
                         c->recovery_state == HDA_RECOVERY_IDLE;
        int error = c->recovery_error;
        bool failed = c->recovery_state == HDA_RECOVERY_FAILED;
        hda_unlock(c, flags);
        if (recovered) return 0;
        if (failed) return error ? error : -EIO;
        hda_sleep_ms(c, 1u);
    }
    return -ETIMEDOUT;
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
