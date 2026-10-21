#include "controller_internal.h"

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
                                         uint32_t response);

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
static void hda_service_immediate_locked(struct hda_controller *c);


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
