#include "controller_internal.h"

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
static int hda_set_crst(struct hda_controller *c, bool released);

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
static int hda_reset_corb_read_pointer(struct hda_controller *c);

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
static int hda_reset_rirb_write_pointer(struct hda_controller *c);

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
static int hda_disable_bus_master(struct hda_controller *c);

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
static void hda_release_ring_resources(struct hda_controller *c);


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
int hda_reset_link(struct hda_controller *c)
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
int hda_stop_rings(struct hda_controller *c)
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
int hda_publish_ring_registers(struct hda_controller *c)
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
int hda_program_rings(struct hda_controller *c)
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
void hda_release_resources(struct hda_controller *c)
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
int hda_quiesce_dma(struct hda_controller *c)
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
int hda_enter_immediate_mode(struct hda_controller *c)
{
    if (c->api->struct_size < RELIEFOS_DRIVER_LOG_API_SIZE || !c->api->console_log)
        return -EOPNOTSUPP;
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
    c->api->console_log(CONSOLE_LOG_WARN,
        "[hda] ring init failed; using immediate command transport (no unsolicited)\n");
    return 0;
}
