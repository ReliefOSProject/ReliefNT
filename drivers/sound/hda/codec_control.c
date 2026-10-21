#include "codec_internal.h"

/** @brief Read one channel of a codec input/output amplifier using the GET index field.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit GET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param out_value Receives raw gain/mute response byte.
 * @return 0 or transport error; -ERANGE if the index cannot be encoded.
 *
 * @par Context
 * Task context during serialized route configuration, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec/controller state; the command flight owns its short lock, never the whole route transaction.
 */
static int hda_codec_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                              uint8_t index, bool right, uint16_t *out_value);

/** @brief Set one amplifier channel and verify the device's gain/mute readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit SET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param value Exact saved or desired gain/mute byte.
 * @param amp_caps Effective inherited or local amplifier capabilities.
 * @return 0 after matching readback, -ERANGE for unsupported index/gain, -EIO for mismatch, or transport errno.
 *
 * @par Context
 * Task context while the per-codec route transaction gate is owned; hda_exec_verb waits outside all route spinlocks.
 * @par Ownership and locks/IRQ
 * Does not retain or transfer amplifier ownership; caller's route snapshot remains the rollback owner.
 */
static int hda_codec_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t value,
                               uint32_t amp_caps);

/** @brief Execute one setter and require an exact masked device readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Node receiving the control update.
 * @param set_verb Long-form SET verb identifier.
 * @param get_verb Long-form GET verb identifier used to read back state.
 * @param value Value sent as the SET payload.
 * @param mask Meaningful bits that must match in the GET response.
 * @return 0 after readback, the original transport error, or -EIO on mismatch.
 *
 * @par Context
 * Task context while the route gate is owned, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec and route hardware state; no spinlock is retained during either verb.
 */
static int hda_codec_set_checked(struct hda_codec *codec, uint8_t nid,
                                 uint16_t set_verb, uint16_t get_verb,
                                 uint16_t value, uint32_t mask);

/** @brief Read checked HDA power status without conflating Act and Set fields.
 * @param codec Probed codec borrowing the initialized controller.
 * @param nid AFG or power-capable widget node.
 * @param out_set Receives PS_Set from response bits 3:0.
 * @param out_act Receives PS_Act from response bits 7:4.
 * @return 0 with both fields or -EIO when the device reports PS_Error.
 *
 * @par Context
 * Task route transaction outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller/codec state; no lock is held over the synchronous verb.
 */
static int hda_codec_read_power_status(struct hda_codec *codec, uint8_t nid,
                                       uint8_t *out_set, uint8_t *out_act);

/** @brief Request a power state and wait boundedly for its actual state.
 * @param codec Probed codec borrowing a live controller and task clock.
 * @param nid AFG or power-capable widget to transition.
 * @param target Requested four-bit HDA power state.
 * @return 0 when PS_Set and PS_Act match target without PS_Error, a transport
 * error, -EIO for rejected/mismatched PS_Set, -ETIMEDOUT for delayed Act, or
 * -ENOTSUP if an asynchronous transition lacks a task clock/sleep service.
 *
 * @par Context
 * Task context only while the codec/controller and H1 clock services are live.
 * @par Ownership and locks/IRQ
 * The codec route gate may remain logically owned but no spinlock is held over
 * verbs or sleep. The private controller wait wrapper permits IRQ progress and
 * restores entry IF; never call from IRQ/service/global execution callbacks.
 */
static int hda_codec_set_power_state(struct hda_codec *codec, uint8_t nid,
                                     uint8_t target);

/** @brief Mute one route amplifier while preserving its prior gain value.
 * @param codec Probed cache borrowing the live controller.
 * @param route Owner of exact pre-route amp values and changed flags.
 * @param path_index Index of the amplifier's widget within route->path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after mute readback, or when mute is unsupported; otherwise the
 * first transport/readback error.
 *
 * @par Context
 * Task route transaction; uses the HDA GET low-nibble index and SET bits 11:8 index encodings.
 * @par Ownership and locks/IRQ
 * Mutates only one hardware amp and the corresponding caller-owned changed bit; no lock spans the verb wait.
 */
static int hda_route_mute_amp(struct hda_codec *codec, struct hda_route *route,
                              uint32_t path_index, bool input, bool right);

/** @brief Identify QEMU's known immutable input/output-only analog pins.
 * @param codec Probed codec identity.
 * @param node Borrowed pin widget metadata.
 * @return True only for six QEMU codec IDs and input/output-only pin caps.
 * Task metadata lookup; no hardware write, allocation, or ownership change.
 */
static bool hda_codec_fixed_qemu_pin(const struct hda_codec *codec,
                                     const struct hda_codec_node *node);

/** @brief Disable mutable route pins and verify known fixed QEMU pin state.
 * @param codec Probed codec borrowing the live controller.
 * @param route Snapshot-complete route whose pin controls are owned by caller.
 * @return 0 after every pin gate reads back, otherwise the first error.
 *
 * @par Context
 * Task route transaction before topology writes and before restoration writes.
 * @par Ownership and locks/IRQ
 * Records pending pin restoration in route->changed_flags before each checked
 * verb; no lock spans synchronous controller I/O.
 */
static int hda_route_gate_pins(struct hda_codec *codec,
                               struct hda_route *route);

/** @brief Restore one amplifier channel to its exact saved gain and mute state.
 * @param codec Probed cache borrowing the live controller.
 * @param route Caller-owned snapshot with a pending changed bit.
 * @param path_index Index of the amplifier widget within the path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after matching readback or transport/range error.
 *
 * @par Context
 * Task route rollback or route stop while the codec transaction gate is owned.
 * @par Ownership and locks/IRQ
 * Reads only caller-owned snapshot values and updates no ownership; no spinlock is held across verbs.
 */
static int hda_route_restore_amp(struct hda_codec *codec, struct hda_route *route,
                                 uint32_t path_index, bool input, bool right);


/** @brief Read one channel of a codec input/output amplifier using the GET index field.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit GET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param out_value Receives raw gain/mute response byte.
 * @return 0 or transport error; -ERANGE if the index cannot be encoded.
 *
 * @par Context
 * Task context during serialized route configuration, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec/controller state; the command flight owns its short lock, never the whole route transaction.
 */
static int hda_codec_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                              uint8_t index, bool right, uint16_t *out_value)
{
    if (!codec || !codec->nodes || !out_value)
        return -EINVAL;
    if (index > 15u) return -ERANGE;
    if (right && !(codec->nodes[nid].widget_caps & HDA_WCAP_STEREO)) {
        *out_value = 0u;
        return 0;
    }
    uint16_t payload = (!input ? HDA_AMP_GET_OUTPUT : 0u) |
        (right ? 0u : HDA_AMP_GET_LEFT) | (input ? index : 0u);
    uint32_t response = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_GET_AMP, payload, true, &response);
    if (!ret) *out_value = (uint16_t)(response & 0xffu);
    return ret;
}

/** @brief Set one amplifier channel and verify the device's gain/mute readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Widget with the amplifier.
 * @param input True for an input amplifier; false for output.
 * @param index Four-bit SET input-amplifier index; output amplifiers use zero.
 * @param right True for right channel; false for left.
 * @param value Exact saved or desired gain/mute byte.
 * @param amp_caps Effective inherited or local amplifier capabilities.
 * @return 0 after matching readback, -ERANGE for unsupported index/gain, -EIO for mismatch, or transport errno.
 *
 * @par Context
 * Task context while the per-codec route transaction gate is owned; hda_exec_verb waits outside all route spinlocks.
 * @par Ownership and locks/IRQ
 * Does not retain or transfer amplifier ownership; caller's route snapshot remains the rollback owner.
 */
static int hda_codec_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t value,
                               uint32_t amp_caps)
{
    if (index > 15u) return -ERANGE;
    if (!codec || !codec->nodes) return -EINVAL;
    if (!input && index) return -ERANGE;
    if (right && !(codec->nodes[nid].widget_caps & HDA_WCAP_STEREO))
        return 0;
    uint32_t max_gain = (amp_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                        HDA_AMP_CAP_NUM_STEPS_SHIFT;
    if ((value & HDA_AMP_GAIN_MASK) > max_gain) return -ERANGE;
    uint16_t channel = right ? HDA_AMP_SET_RIGHT : HDA_AMP_SET_LEFT;
    uint16_t index_payload = input ?
        (uint16_t)((uint16_t)index << HDA_AMP_SET_INDEX_SHIFT) : (uint16_t)0u;
    uint16_t payload = (uint16_t)(
        (input ? HDA_AMP_SET_INPUT : HDA_AMP_SET_OUTPUT) | channel |
        index_payload | (value & (HDA_AMP_SET_MUTE | HDA_AMP_GAIN_MASK)));
    uint32_t ignored = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_SET_AMP, payload, true, &ignored);
    if (ret) return ret;
    uint16_t actual = 0u;
    ret = hda_codec_read_amp(codec, nid, input, index, right, &actual);
    if (ret) return ret;
    uint16_t compare_mask = HDA_AMP_GAIN_MASK;
    if (amp_caps & HDA_AMP_CAP_MUTE) compare_mask |= HDA_AMP_SET_MUTE;
    if ((actual & compare_mask) != (value & compare_mask)) return -EIO;
    return 0;
}

/** @brief Read one task-context mixer amplifier channel with exact wire encoding.
 * @param codec Codec cache with a live controller transport.
 * @param nid Amplifier widget NID.
 * @param input Select input amplifier when true, output when false.
 * @param index Input amplifier index, or zero for output.
 * @param right Select the right channel when true.
 * @param out_value Receives the raw gain/mute value after a checked verb.
 * @return 0 or the original validation/transport error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ Caller owns the output and codec lifetime; this
 * wrapper does not hold a lock across the synchronous verb.
 */
int hda_codec_control_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t *out_value)
{
    return hda_codec_read_amp(codec, nid, input, index, right, out_value);
}

/** @brief Write one mixer amplifier channel and require exact hardware readback.
 * @param codec Codec cache with a live controller transport.
 * @param nid Amplifier widget NID.
 * @param input Select input amplifier when true, output when false.
 * @param index Input amplifier index, or zero for output.
 * @param right Select the right channel when true.
 * @param value Raw gain/mute value to write.
 * @param amp_caps Effective amplifier capability bits used for validation.
 * @return 0 after readback, or the original validation/transport/readback error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ Caller owns the codec lifetime; no lock spans
 * the synchronous verb and IRQ progress remains enabled.
 */
int hda_codec_control_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                                uint8_t index, bool right, uint16_t value,
                                uint32_t amp_caps)
{
    return hda_codec_write_amp(codec, nid, input, index, right, value, amp_caps);
}

/** @brief Save all control values that a route may change before its first write.
 * @param codec Probed codec with a live serialized verb transport.
 * @param route Resolved path whose caller-owned snapshot arrays are filled.
 * @return 0 or the first transport, invalid-state, or unencodable-amp error.
 *
 * @par Context
 * Task context while the codec route gate is held, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Stores only values in caller-owned route metadata; no route writes occur until every snapshot succeeds.
 */
int hda_route_take_snapshot(struct hda_codec *codec,
                                   struct hda_route *route)
{
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    struct hda_codec_node *afg = &codec->nodes[afg_nid];
    if (!afg->present || afg->type != HDA_WIDGET_FUNCTION_GROUP)
        return -EINVAL;
    route->afg_lease_acquired = 0u;
    route->control_lease_acquired = 0u;
    route->saved_afg_power = 0u;
    uint8_t afg_set = 0u;
    uint8_t afg_act = 0u;
    int ret = hda_codec_read_power_status(codec, afg_nid, &afg_set,
                                          &afg_act);
    if (ret) return ret;
    (void)afg_act;
    if (codec->afg_lease_count[afg_nid]) {
        if (!codec->afg_lease_valid[afg_nid]) return -EIO;
        route->saved_afg_power = codec->afg_lease_saved_power[afg_nid];
    } else {
        route->saved_afg_power = afg_set;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        route->saved_flags[i] = 0u;
        route->changed_flags[i] = 0u;
        uint8_t nid = route->path[i];
        struct hda_codec_node *node = &codec->nodes[nid];
        uint32_t response = 0u;
        ret = 0;
        if (node->widget_caps & HDA_WCAP_POWER) {
            uint8_t power_set = 0u;
            uint8_t power_act = 0u;
            ret = hda_codec_read_power_status(codec, nid, &power_set,
                                              &power_act);
            if (ret) return ret;
            (void)power_act;
            route->saved_power[i] = power_set;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_POWER;
        }
        if (node->connection_count > 1u &&
            (node->type == HDA_WIDGET_SELECTOR ||
             node->type == HDA_WIDGET_PIN ||
             node->type == HDA_WIDGET_AUDIO_INPUT)) {
            ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                HDA_VERB_GET_CONNECTION_SELECT, 0u, false,
                                &response);
            if (ret) return ret;
            if (response >= node->connection_count) return -EIO;
            route->saved_selector[i] = (uint8_t)response;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_SELECTOR;
        }
        if (node->type == HDA_WIDGET_PIN) {
            ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                HDA_VERB_GET_PIN_CONTROL, 0u, false, &response);
            if (ret) return ret;
            route->saved_pin_control[i] = (uint8_t)response;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_PIN;
            if (node->pin_caps & HDA_PINCAP_EAPD) {
                ret = hda_exec_verb(codec->controller, codec->cad, nid,
                                    HDA_VERB_GET_EAPD, 0u, false, &response);
                if (ret) return ret;
                route->saved_eapd[i] = (uint8_t)response;
                route->saved_flags[i] |= HDA_ROUTE_CHANGED_EAPD;
            }
        }

        uint8_t input_index = 0u;
        if (node->connection_count && route->connection_index[i] != 0xffu)
            input_index = route->connection_index[i];
        if (node->widget_caps & HDA_WCAP_IN_AMP) {
            if (input_index > 15u) return -ERANGE;
            ret = hda_codec_read_amp(codec, nid, true, input_index, false,
                                     &route->saved_input_amp[i][0]);
            if (ret) return ret;
            route->saved_input_amp[i][1] = 0u;
            if (node->widget_caps & HDA_WCAP_STEREO) {
                ret = hda_codec_read_amp(codec, nid, true, input_index, true,
                                         &route->saved_input_amp[i][1]);
                if (ret) return ret;
            }
            uint32_t max_gain = (node->amp_input_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                                HDA_AMP_CAP_NUM_STEPS_SHIFT;
            if ((route->saved_input_amp[i][0] & HDA_AMP_GAIN_MASK) > max_gain ||
                ((node->widget_caps & HDA_WCAP_STEREO) &&
                 (route->saved_input_amp[i][1] & HDA_AMP_GAIN_MASK) > max_gain))
                return -ERANGE;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_INPUT_AMP;
        }
        if (node->widget_caps & HDA_WCAP_OUT_AMP) {
            ret = hda_codec_read_amp(codec, nid, false, 0u, false,
                                     &route->saved_output_amp[i][0]);
            if (ret) return ret;
            route->saved_output_amp[i][1] = 0u;
            if (node->widget_caps & HDA_WCAP_STEREO) {
                ret = hda_codec_read_amp(codec, nid, false, 0u, true,
                                         &route->saved_output_amp[i][1]);
                if (ret) return ret;
            }
            uint32_t max_gain = (node->amp_output_caps & HDA_AMP_CAP_NUM_STEPS_MASK) >>
                                HDA_AMP_CAP_NUM_STEPS_SHIFT;
            if ((route->saved_output_amp[i][0] & HDA_AMP_GAIN_MASK) > max_gain ||
                ((node->widget_caps & HDA_WCAP_STEREO) &&
                 (route->saved_output_amp[i][1] & HDA_AMP_GAIN_MASK) > max_gain))
                return -ERANGE;
            route->saved_flags[i] |= HDA_ROUTE_CHANGED_OUTPUT_AMP;
        }
    }
    route->snapshots_valid = 1u;
    return 0;
}

/** @brief Execute one setter and require an exact masked device readback.
 * @param codec Probed cache borrowing the live controller.
 * @param nid Node receiving the control update.
 * @param set_verb Long-form SET verb identifier.
 * @param get_verb Long-form GET verb identifier used to read back state.
 * @param value Value sent as the SET payload.
 * @param mask Meaningful bits that must match in the GET response.
 * @return 0 after readback, the original transport error, or -EIO on mismatch.
 *
 * @par Context
 * Task context while the route gate is owned, outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows codec and route hardware state; no spinlock is retained during either verb.
 */
static int hda_codec_set_checked(struct hda_codec *codec, uint8_t nid,
                                 uint16_t set_verb, uint16_t get_verb,
                                 uint16_t value, uint32_t mask)
{
    uint32_t ignored = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid, set_verb,
                            value, false, &ignored);
    if (ret) return ret;
    uint32_t actual = 0u;
    ret = hda_exec_verb(codec->controller, codec->cad, nid, get_verb, 0u,
                        false, &actual);
    if (ret) return ret;
    return (actual & mask) == (value & mask) ? 0 : -EIO;
}

/** @brief Read checked HDA power status without conflating Act and Set fields.
 * @param codec Probed codec borrowing the initialized controller.
 * @param nid AFG or power-capable widget node.
 * @param out_set Receives PS_Set from response bits 3:0.
 * @param out_act Receives PS_Act from response bits 7:4.
 * @return 0 with both fields or -EIO when the device reports PS_Error.
 *
 * @par Context
 * Task route transaction outside global execution ownership.
 * @par Ownership and locks/IRQ
 * Borrows controller/codec state; no lock is held over the synchronous verb.
 */
static int hda_codec_read_power_status(struct hda_codec *codec, uint8_t nid,
                                       uint8_t *out_set, uint8_t *out_act)
{
    if (!codec || !codec->controller || !out_set || !out_act) return -EINVAL;
    uint32_t response = 0u;
    int ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_GET_POWER_STATE, 0u, false, &response);
    if (ret) return ret;
    if (response & HDA_POWER_ERROR) return -EIO;
    *out_set = (uint8_t)(response & HDA_POWER_STATE_MASK);
    *out_act = (uint8_t)((response >> HDA_POWER_ACT_SHIFT) &
                         HDA_POWER_STATE_MASK);
    return 0;
}

/** @brief Request a power state and wait boundedly for its actual state.
 * @param codec Probed codec borrowing a live controller and task clock.
 * @param nid AFG or power-capable widget to transition.
 * @param target Requested four-bit HDA power state.
 * @return 0 when PS_Set and PS_Act match target without PS_Error, a transport
 * error, -EIO for rejected/mismatched PS_Set, -ETIMEDOUT for delayed Act, or
 * -ENOTSUP if an asynchronous transition lacks a task clock/sleep service.
 *
 * @par Context
 * Task context only while the codec/controller and H1 clock services are live.
 * @par Ownership and locks/IRQ
 * The codec route gate may remain logically owned but no spinlock is held over
 * verbs or sleep. The private controller wait wrapper permits IRQ progress and
 * restores entry IF; never call from IRQ/service/global execution callbacks.
 */
static int hda_codec_set_power_state(struct hda_codec *codec, uint8_t nid,
                                     uint8_t target)
{
    if (!codec || !codec->controller || target > HDA_POWER_STATE_MASK)
        return -EINVAL;
    const struct reliefos_driver_kernel_api *api = codec->controller->api;
    uint8_t current_set = 0u;
    uint8_t current_act = 0u;
    int ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
    if (ret) return ret;
    if (current_set != target) {
        uint32_t ignored = 0u;
        ret = hda_exec_verb(codec->controller, codec->cad, nid,
                            HDA_VERB_SET_POWER_STATE, target, false, &ignored);
        if (ret) return ret;
        /* A SET response does not report PS_Error or the applied state. */
        ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
        if (ret) return ret;
        if (current_set != target) return -EIO;
        if (current_act == target) return 0;
    }

    if (current_act == target) return 0;
    if (!api || !api->ticks || !api->sleep_ms) return -EOPNOTSUPP;
    uint64_t start = api->ticks();
    for (uint32_t poll = 0u; poll < HDA_POWER_TRANSITION_MAX_POLLS; ++poll) {
        ret = hda_codec_read_power_status(codec, nid, &current_set,
                                          &current_act);
        if (ret) return ret;
        if (current_set != target) return -EIO;
        if (current_act == target) return 0;
        if (api->ticks() - start >= HDA_POWER_TRANSITION_TIMEOUT_TICKS)
            return -ETIMEDOUT;
        hda_controller_sleep_ms(codec->controller, 1u);
    }
    return -ETIMEDOUT;
}

/** @brief Acquire exclusive signal nodes and one shared AFG power lease.
 * @param codec Probed cache whose route transaction gate is held.
 * @param route Snapshot-complete path acquiring both ownership classes.
 * @return 0 after ownership is recorded, -EBUSY for a signal-node conflict,
 * -EOVERFLOW for a saturated AFG count, or -EIO for inconsistent lease state.
 *
 * @par Context
 * Task context before the first route write.
 * @par Ownership and locks/IRQ
 * Mutates only codec/route metadata under the bounded logical route gate; no
 * verb, wait, allocation, or spinlock is involved.
 */
int hda_route_acquire_leases(struct hda_codec *codec,
                                    struct hda_route *route)
{
    int ret = hda_route_check_control_lease(codec, route);
    if (ret) return ret;
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    uint32_t count = codec->afg_lease_count[afg_nid];
    if (count == UINT32_MAX) return -EOVERFLOW;
    if (count && !codec->afg_lease_valid[afg_nid]) return -EIO;
    if (!count) {
        codec->afg_lease_saved_power[afg_nid] = route->saved_afg_power;
        codec->afg_lease_valid[afg_nid] = 1u;
    } else {
        route->saved_afg_power = codec->afg_lease_saved_power[afg_nid];
    }
    codec->afg_lease_count[afg_nid] = count + 1u;
    route->afg_lease_acquired = 1u;
    for (uint32_t i = 0u; i < route->path_count; ++i)
        codec->route_node_owner[route->path[i]] = 1u;
    route->control_lease_acquired = 1u;
    return 0;
}

/** @brief Release signal nodes and safely drop one shared AFG lease.
 * @param codec Probed cache whose route transaction gate is held.
 * @param route Path releasing ownership after control restoration.
 * @return 0 after release; a failed last-owner power restore retains every
 * ownership record and the original state for a later disable retry.
 *
 * @par Context
 * Task context after local route controls have been restored.
 * @par Ownership and locks/IRQ
 * The logical gate protects metadata while power transition helpers wait
 * without a spinlock. Caller keeps controller/API services alive for retry.
 */
int hda_route_release_leases(struct hda_codec *codec,
                                    struct hda_route *route)
{
    if (!route->afg_lease_acquired || !route->control_lease_acquired)
        return -EINVAL;
    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    uint32_t count = codec->afg_lease_count[afg_nid];
    if (!count || !codec->afg_lease_valid[afg_nid]) return -EIO;
    for (uint32_t i = 0u; i < route->path_count; ++i)
        if (!codec->route_node_owner[route->path[i]]) return -EIO;

    if (count == 1u) {
        int ret = hda_codec_set_power_state(codec, afg_nid,
                            codec->afg_lease_saved_power[afg_nid]);
        if (ret) return ret;
        codec->afg_lease_count[afg_nid] = 0u;
        codec->afg_lease_valid[afg_nid] = 0u;
        codec->afg_lease_saved_power[afg_nid] = 0u;
    } else {
        codec->afg_lease_count[afg_nid] = count - 1u;
    }
    for (uint32_t i = 0u; i < route->path_count; ++i)
        codec->route_node_owner[route->path[i]] = 0u;
    route->afg_lease_acquired = 0u;
    route->control_lease_acquired = 0u;
    return 0;
}

/** @brief Mute one route amplifier while preserving its prior gain value.
 * @param codec Probed cache borrowing the live controller.
 * @param route Owner of exact pre-route amp values and changed flags.
 * @param path_index Index of the amplifier's widget within route->path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after mute readback, or when mute is unsupported; otherwise the
 * first transport/readback error.
 *
 * @par Context
 * Task route transaction; uses the HDA GET low-nibble index and SET bits 11:8 index encodings.
 * @par Ownership and locks/IRQ
 * Mutates only one hardware amp and the corresponding caller-owned changed bit; no lock spans the verb wait.
 */
static int hda_route_mute_amp(struct hda_codec *codec, struct hda_route *route,
                              uint32_t path_index, bool input, bool right)
{
    uint8_t nid = route->path[path_index];
    struct hda_codec_node *node = &codec->nodes[nid];
    uint8_t index = 0u;
    uint16_t prior = 0u;
    uint32_t caps;
    if (input) {
        caps = node->amp_input_caps;
        if (route->connection_index[path_index] != 0xffu)
            index = route->connection_index[path_index];
        prior = route->saved_input_amp[path_index][right ? 1u : 0u];
    } else {
        caps = node->amp_output_caps;
        prior = route->saved_output_amp[path_index][right ? 1u : 0u];
    }
    if (!(caps & HDA_AMP_CAP_MUTE)) return 0;
    route->changed_flags[path_index] |= input ? HDA_ROUTE_CHANGED_INPUT_AMP :
                                                 HDA_ROUTE_CHANGED_OUTPUT_AMP;
    return hda_codec_write_amp(codec, nid, input, index, right,
                               (uint16_t)((prior & HDA_AMP_GAIN_MASK) |
                                          HDA_AMP_SET_MUTE), caps);
}

/** @brief Identify QEMU's known immutable input/output-only analog pins.
 * @param codec Probed codec identity.
 * @param node Borrowed pin widget metadata.
 * @return True only for six QEMU codec IDs and input/output-only pin caps.
 * Task metadata lookup; no hardware write, allocation, or ownership change.
 */
static bool hda_codec_fixed_qemu_pin(const struct hda_codec *codec,
                                     const struct hda_codec_node *node)
{
    if (!codec || !node || node->type != HDA_WIDGET_PIN ||
        (node->pin_caps != HDA_PINCAP_OUTPUT && node->pin_caps != HDA_PINCAP_INPUT))
        return false;
    switch (codec->vendor_id) {
    case 0x1af40011u: case 0x1af40012u:
    case 0x1af40021u: case 0x1af40022u:
    case 0x1af40031u: case 0x1af40032u:
        return true;
    default:
        return false;
    }
}

/** @brief Disable mutable route pins and verify known fixed QEMU pin state.
 * @param codec Probed codec borrowing the live controller.
 * @param route Snapshot-complete route whose pin controls are owned by caller.
 * @return 0 after every pin gate reads back, otherwise the first error.
 *
 * @par Context
 * Task route transaction before topology writes and before restoration writes.
 * @par Ownership and locks/IRQ
 * Records pending pin restoration in route->changed_flags before each checked
 * verb; no lock spans synchronous controller I/O.
 */
static int hda_route_gate_pins(struct hda_codec *codec,
                               struct hda_route *route)
{
    for (uint32_t i = 0u; i < route->path_count; ++i) {
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        if (node->type != HDA_WIDGET_PIN) continue;
        if (hda_codec_fixed_qemu_pin(codec, node)) {
            /* QEMU GET returns a descriptor constant; SET does not change it.
             * Gate the real converter amplifiers instead, and still verify
             * that this fixed pin already carries the requested direction. */
            uint32_t actual = 0u;
            uint8_t expected = route->direction == HDA_ROUTE_PLAYBACK ?
                                HDA_PINCTL_OUTPUT_ENABLE : HDA_PINCTL_INPUT_ENABLE;
            int ret = hda_exec_verb(codec->controller, codec->cad, route->path[i],
                                    HDA_VERB_GET_PIN_CONTROL, 0u, false, &actual);
            if (ret) return ret;
            if (route->saved_pin_control[i] != expected || (actual & 0xffu) != expected)
                return -EIO;
            continue;
        }
        uint8_t gate = route->saved_pin_control[i] &
            (uint8_t)~(HDA_PINCTL_HP_ENABLE | HDA_PINCTL_OUTPUT_ENABLE |
                       HDA_PINCTL_INPUT_ENABLE);
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_PIN;
        int ret = hda_codec_set_checked(codec, route->path[i],
                                        HDA_VERB_SET_PIN_CONTROL,
                                        HDA_VERB_GET_PIN_CONTROL, gate, 0xffu);
        if (ret) return ret;
    }
    return 0;
}

/** @brief Restore one amplifier channel to its exact saved gain and mute state.
 * @param codec Probed cache borrowing the live controller.
 * @param route Caller-owned snapshot with a pending changed bit.
 * @param path_index Index of the amplifier widget within the path.
 * @param input Select input versus output amplifier.
 * @param right Select right versus left channel.
 * @return 0 after matching readback or transport/range error.
 *
 * @par Context
 * Task route rollback or route stop while the codec transaction gate is owned.
 * @par Ownership and locks/IRQ
 * Reads only caller-owned snapshot values and updates no ownership; no spinlock is held across verbs.
 */
static int hda_route_restore_amp(struct hda_codec *codec, struct hda_route *route,
                                 uint32_t path_index, bool input, bool right)
{
    uint8_t nid = route->path[path_index];
    struct hda_codec_node *node = &codec->nodes[nid];
    uint8_t index = 0u;
    uint16_t prior;
    uint32_t caps;
    if (input) {
        caps = node->amp_input_caps;
        if (route->connection_index[path_index] != 0xffu)
            index = route->connection_index[path_index];
        prior = route->saved_input_amp[path_index][right ? 1u : 0u];
    } else {
        caps = node->amp_output_caps;
        prior = route->saved_output_amp[path_index][right ? 1u : 0u];
    }
    return hda_codec_write_amp(codec, nid, input, index, right, prior, caps);
}

/** @brief Apply D0, graph selectors/mixer input amps, pins, and EAPD in safe order.
 * @param codec Probed codec borrowing the live controller.
 * @param route Resolved path with complete pre-write snapshots.
 * @return 0 when every setter reads back, or the first transport/readback error.
 *
 * @par Context
 * Task context while the per-codec logical route gate is owned; converter format/tag remains the prepared H4 stream's responsibility.
 * @par Ownership and locks/IRQ
 * Route storage owns rollback values; hardware controls remain codec-owned. No route-wide spinlock crosses synchronous verb waits.
 */
int hda_route_apply_controls(struct hda_codec *codec,
                                    struct hda_route *route)
{
    int ret;
    ret = hda_route_gate_pins(codec, route);
    if (ret) return ret;

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, true, false);
            if (ret) return ret;
            ret = hda_route_mute_amp(codec, route, i, true, true);
            if (ret) return ret;
        }
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, false, false);
            if (ret) return ret;
            ret = hda_route_mute_amp(codec, route, i, false, true);
            if (ret) return ret;
        }
    }

    uint8_t afg_nid = codec->nodes[route->pin_nid].afg_nid;
    ret = hda_codec_set_power_state(codec, afg_nid, 0u);
    if (ret) return ret;

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (!(route->saved_flags[i] & HDA_ROUTE_CHANGED_POWER)) continue;
        if (route->saved_power[i] != 0u)
            route->changed_flags[i] |= HDA_ROUTE_CHANGED_POWER;
        ret = hda_codec_set_power_state(codec, route->path[i], 0u);
        if (ret) return ret;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (!(route->saved_flags[i] & HDA_ROUTE_CHANGED_SELECTOR)) continue;
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        uint8_t selected = route->connection_index[i];
        if (selected >= node->connection_count) return -EINVAL;
        if (route->saved_selector[i] == selected) continue;
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_SELECTOR;
        ret = hda_codec_set_checked(codec, route->path[i],
                                    HDA_VERB_SET_CONNECTION_SELECT,
                                    HDA_VERB_GET_CONNECTION_SELECT,
                                    selected, 0xffu);
        if (ret) return ret;
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        struct hda_codec_node *node = &codec->nodes[route->path[i]];
        if (node->type != HDA_WIDGET_PIN ||
            !(route->saved_flags[i] & HDA_ROUTE_CHANGED_PIN)) continue;
        uint8_t control = route->saved_pin_control[i];
        control &= (uint8_t)~(HDA_PINCTL_HP_ENABLE | HDA_PINCTL_OUTPUT_ENABLE |
                              HDA_PINCTL_INPUT_ENABLE | HDA_PINCTL_VREF_MASK);
        if (route->direction == HDA_ROUTE_PLAYBACK) {
            control |= HDA_PINCTL_OUTPUT_ENABLE;
            uint8_t device = (uint8_t)((node->pin_default >> 20) & 0x0fu);
            if (device == 2u) control |= HDA_PINCTL_HP_ENABLE;
        } else {
            control |= HDA_PINCTL_INPUT_ENABLE;
            if (node->pin_caps & HDA_PINCAP_VREF_80)
                control |= HDA_PINCTL_VREF_80;
        }
        if (hda_codec_fixed_qemu_pin(codec, node)) {
            if (control != route->saved_pin_control[i]) return -EOPNOTSUPP;
            continue;
        }
        route->changed_flags[i] |= HDA_ROUTE_CHANGED_PIN;
        ret = hda_codec_set_checked(codec, route->path[i],
                                    HDA_VERB_SET_PIN_CONTROL,
                                    HDA_VERB_GET_PIN_CONTROL, control, 0xffu);
        if (ret) return ret;
        if (route->direction == HDA_ROUTE_PLAYBACK &&
            (node->pin_caps & HDA_PINCAP_EAPD) &&
            (route->saved_flags[i] & HDA_ROUTE_CHANGED_EAPD)) {
            uint8_t eapd = route->saved_eapd[i] | 2u;
            if (eapd != route->saved_eapd[i]) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_EAPD;
                ret = hda_codec_set_checked(codec, route->path[i],
                                            HDA_VERB_SET_EAPD,
                                            HDA_VERB_GET_EAPD, eapd, 0xffu);
                if (ret) return ret;
            }
        }
    }

    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            for (uint32_t channel = 0u; channel < 2u; ++channel) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_INPUT_AMP;
                uint32_t caps = codec->nodes[route->path[i]].amp_input_caps;
                uint16_t zero_db = (uint16_t)(caps &
                                               HDA_AMP_CAP_OFFSET_MASK);
                ret = hda_codec_write_amp(codec, route->path[i], true,
                    route->connection_index[i] == 0xffu ? 0u :
                        route->connection_index[i], channel != 0u,
                    zero_db, caps);
                if (ret) return ret;
            }
        }
        if (route->saved_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            for (uint32_t channel = 0u; channel < 2u; ++channel) {
                route->changed_flags[i] |= HDA_ROUTE_CHANGED_OUTPUT_AMP;
                uint32_t caps = codec->nodes[route->path[i]].amp_output_caps;
                uint16_t zero_db = (uint16_t)(caps &
                                               HDA_AMP_CAP_OFFSET_MASK);
                ret = hda_codec_write_amp(codec, route->path[i], false, 0u,
                    channel != 0u,
                    zero_db, caps);
                if (ret) return ret;
            }
        }
    }
    return 0;
}

/** @brief Best-effort exact rollback that retains failed controls for retry.
 * @param codec Probed codec borrowing the live controller.
 * @param route Route with snapshots and zero or more changed control bits.
 * @return 0 when every changed value was restored, otherwise the first error.
 *
 * @par Context
 * Task context after stream stop or an apply failure; caller still owns the transaction gate.
 * @par Ownership and locks/IRQ
 * Continues independent restore attempts after an error, clearing only verified controls. Failed flags/snapshots stay with route for a later retry; no lock spans a verb.
 */
int hda_route_restore_controls(struct hda_codec *codec,
                                      struct hda_route *route)
{
    int first_error = 0;
    int ret;
    ret = hda_route_gate_pins(codec, route);
    if (ret) return ret;
    for (uint32_t i = 0u; i < route->path_count; ++i) {
        if (route->changed_flags[i] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, true, false);
            if (ret && !first_error) first_error = ret;
            ret = hda_route_mute_amp(codec, route, i, true, true);
            if (ret && !first_error) first_error = ret;
        }
        if (route->changed_flags[i] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            ret = hda_route_mute_amp(codec, route, i, false, false);
            if (ret && !first_error) first_error = ret;
            ret = hda_route_mute_amp(codec, route, i, false, true);
            if (ret && !first_error) first_error = ret;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        uint8_t flags = route->changed_flags[index];
        if (flags & HDA_ROUTE_CHANGED_EAPD) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_EAPD, HDA_VERB_GET_EAPD,
                                        route->saved_eapd[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_EAPD;
            else if (!first_error) first_error = ret;
        }
        if (flags & HDA_ROUTE_CHANGED_PIN) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_PIN_CONTROL,
                                        HDA_VERB_GET_PIN_CONTROL,
                                        route->saved_pin_control[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_PIN;
            else if (!first_error) first_error = ret;
        }
        if (flags & HDA_ROUTE_CHANGED_SELECTOR) {
            ret = hda_codec_set_checked(codec, route->path[index],
                                        HDA_VERB_SET_CONNECTION_SELECT,
                                        HDA_VERB_GET_CONNECTION_SELECT,
                                        route->saved_selector[index], 0xffu);
            if (!ret) route->changed_flags[index] &=
                         (uint8_t)~HDA_ROUTE_CHANGED_SELECTOR;
            else if (!first_error) first_error = ret;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        if (route->changed_flags[index] & HDA_ROUTE_CHANGED_INPUT_AMP) {
            int left_ret = hda_route_restore_amp(codec, route, index, true,
                                                false);
            int right_ret = hda_route_restore_amp(codec, route, index, true,
                                                 true);
            if (left_ret && !first_error) first_error = left_ret;
            if (right_ret && !first_error) first_error = right_ret;
            if (!left_ret && !right_ret)
                route->changed_flags[index] &=
                    (uint8_t)~HDA_ROUTE_CHANGED_INPUT_AMP;
        }
        if (route->changed_flags[index] & HDA_ROUTE_CHANGED_OUTPUT_AMP) {
            int left_ret = hda_route_restore_amp(codec, route, index, false,
                                                false);
            int right_ret = hda_route_restore_amp(codec, route, index, false,
                                                 true);
            if (left_ret && !first_error) first_error = left_ret;
            if (right_ret && !first_error) first_error = right_ret;
            if (!left_ret && !right_ret)
                route->changed_flags[index] &=
                    (uint8_t)~HDA_ROUTE_CHANGED_OUTPUT_AMP;
        }
    }

    for (uint32_t i = route->path_count; i > 0u; --i) {
        uint32_t index = i - 1u;
        if (!(route->changed_flags[index] & HDA_ROUTE_CHANGED_POWER)) continue;
        ret = hda_codec_set_power_state(codec, route->path[index],
                                        route->saved_power[index]);
        if (!ret) route->changed_flags[index] &=
                     (uint8_t)~HDA_ROUTE_CHANGED_POWER;
        else if (!first_error) first_error = ret;
    }
    return first_error;
}
