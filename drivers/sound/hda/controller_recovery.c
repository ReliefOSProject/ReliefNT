#include "controller_internal.h"

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
static void hda_recovery_failed_locked(struct hda_controller *c, int error);


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
void hda_poison_locked(struct hda_controller *c, int error)
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
void hda_advance_recovery_locked(struct hda_controller *c)
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
