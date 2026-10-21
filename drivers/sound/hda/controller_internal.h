#ifndef RELIEFNT_DRIVERS_SOUND_HDA_CONTROLLER_C_INTERNAL_H
#define RELIEFNT_DRIVERS_SOUND_HDA_CONTROLLER_C_INTERNAL_H

#include "hda.h"

/* H4 supplies the strong implementation when the stream object is linked;
 * controller-only host fixtures retain a harmless no-stream fallback. */

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




__attribute__((weak)) uint32_t hda_stream_service_locked(struct hda_controller *c,
                                                         uint32_t budget);

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
uint8_t hda_read8(const struct hda_controller *c, uint32_t offset);

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
uint16_t hda_read16(const struct hda_controller *c, uint32_t offset);

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
uint32_t hda_read32(const struct hda_controller *c, uint32_t offset);

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
void hda_write8(struct hda_controller *c, uint32_t offset, uint8_t value);

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
void hda_write16(struct hda_controller *c, uint32_t offset, uint16_t value);

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
void hda_write32(struct hda_controller *c, uint32_t offset, uint32_t value);

/** @brief Read one stream/controller byte for the H4 stream engine.
 * @param c Initialized controller with live MMIO.
 * @param offset Register offset.
 * @return Register value; no wait or allocation.
 */
uint8_t hda_mmio_read8(const struct hda_controller *c, uint32_t offset);

/** @brief Read one stream/controller 16-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @return Register value; no wait or allocation.
 */
uint16_t hda_mmio_read16(const struct hda_controller *c, uint32_t offset);

/** @brief Read one stream/controller 32-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @return Register value; no wait or allocation.
 */
uint32_t hda_mmio_read32(const struct hda_controller *c, uint32_t offset);

/** @brief Write one stream/controller byte for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Register offset.
 * @param value Exact byte, including W1C bits.
 * @return None; no wait or allocation.
 */
void hda_mmio_write8(struct hda_controller *c, uint32_t offset, uint8_t value);

/** @brief Write one stream/controller 16-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @param value Exact register value.
 * @return None; no wait or allocation.
 */
void hda_mmio_write16(struct hda_controller *c, uint32_t offset, uint16_t value);

/** @brief Write one stream/controller 32-bit register for H4.
 * @param c Initialized controller with live MMIO.
 * @param offset Aligned register offset.
 * @param value Exact register value.
 * @return None; no wait or allocation.
 */
void hda_mmio_write32(struct hda_controller *c, uint32_t offset, uint32_t value);

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
void *hda_phys_to_direct_map_page(uint64_t phys);

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
uint64_t hda_lock(struct hda_controller *c);

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
void hda_unlock(struct hda_controller *c, uint64_t flags);

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
void hda_sleep_ms(struct hda_controller *c, uint64_t milliseconds);

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
void hda_controller_sleep_ms(struct hda_controller *c, uint64_t milliseconds);

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
                              uint64_t duration);

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
                             uint64_t timeout_ticks);

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
void hda_pci_write_command(struct hda_controller *c, uint16_t command);

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
                        uint64_t *out_size);

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
                         uint16_t payload, bool short_verb);

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
void hda_zero(void *memory, uint32_t bytes);

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
                                uint8_t *out_select);

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
int hda_reset_link(struct hda_controller *c);

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
int hda_stop_rings(struct hda_controller *c);

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
int hda_publish_ring_registers(struct hda_controller *c);

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
int hda_program_rings(struct hda_controller *c);

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
void hda_release_resources(struct hda_controller *c);

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
int hda_quiesce_dma(struct hda_controller *c);

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
int hda_enter_immediate_mode(struct hda_controller *c);

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
                        const struct reliefos_driver_pci_device *dev);

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
int hda_controller_destroy(struct hda_controller *c);

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
void hda_poison_locked(struct hda_controller *c, int error);

/** @brief Advance one nonblocking controller/link/ring recovery step under lock.
 * @param c Poisoned controller; caller holds the state lock with local IRQs saved.
 * @return None. No sleep, allocation, callback, or unbounded register polling occurs.
 *
 * @par Context
 * Bounded service context while the controller state lock is held.
 * @par Ownership and locks/IRQ
 * Controller retains MMIO and ring pages. Lock is already held; each call has bounded MMIO work and is IRQ-safe without sleep/allocation/callback.
 */
void hda_advance_recovery_locked(struct hda_controller *c);

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
uint32_t hda_controller_service_budget(struct hda_controller *c, uint32_t budget);

/** @brief Run the standard bounded HDA service pass.
 * @param c Initialized controller.
 * @return Entries consumed, capped by HDA_SERVICE_BUDGET. IRQ-safe and nonblocking.
 *
 * @par Context
 * Task/audio-service/tick or hard IRQ context for an initialized controller.
 * @par Ownership and locks/IRQ
 * Controller retains mode-specific transport ownership; the bounded helper owns synchronization. IRQ-safe and nonblocking.
 */
uint32_t hda_controller_service(struct hda_controller *c);

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
                    uint64_t *out_ticket);

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
                  uint32_t *out_response);

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
                  uint32_t *out_response);

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
                        struct hda_unsolicited_response *out);

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
                                  struct hda_unsolicited_response *out);

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
uint32_t hda_unsolicited_dropped_count(struct hda_controller *c);

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
int hda_controller_stream_acquire(struct hda_controller *c);

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
int hda_controller_stream_release(struct hda_controller *c);

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
int hda_controller_set_recovery_allowed(struct hda_controller *c, bool allowed);

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
int hda_controller_recover(struct hda_controller *c);

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
int hda_controller_set_card_id(struct hda_controller *c, uint32_t card);

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
                                           uint32_t *out_card);

#endif
