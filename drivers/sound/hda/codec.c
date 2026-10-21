#include "codec_internal.h"

/** @brief Apply one route while the enclosing codec route gate is owned.
 * @param codec Probed metadata whose logical route gate is held.
 * @param route Path resolved for this codec and caller-owned snapshot storage.
 * @param enable True to snapshot/apply; false to retry or complete restoration.
 * @return 0 on success or the first route, transport, or ownership error.
 * Failed rollback retains active state and snapshots for a later retry.
 *
 * @par Context
 * Task context outside global execution ownership and IRQ/service callbacks.
 * @par Ownership and locks/IRQ
 * Caller owns the logical gate; no spinlock spans verbs or readiness waits.
 */
static int hda_apply_route_locked(struct hda_codec *codec,
                                  struct hda_route *route, bool enable);


/** @brief Enter a per-codec route transaction without holding a lock over verbs.
 * @param codec Cache whose atomic logical gate is acquired.
 * @return 0 when acquired or -EBUSY if another full route transaction is active.
 *
 * @par Context
 * Task context before a synchronous route operation.
 * @par Ownership and locks/IRQ
 * Uses one bounded atomic compare/exchange; it never spins, waits, disables IRQs, or holds a spinlock across a verb.
 */
int hda_codec_route_gate_enter(struct hda_codec *codec)
{
    uint32_t expected = 0u;
    return __atomic_compare_exchange_n(&codec->route_busy, &expected, 1u, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ?
           0 : -EBUSY;
}

/** @brief Leave the per-codec logical route transaction.
 * @param codec Cache whose route transaction gate is currently owned by caller.
 * @return None; publishes completion to a future transaction.
 *
 * @par Context
 * Task context at every route transaction exit.
 * @par Ownership and locks/IRQ
 * Releases one atomic logical gate; no hardware access or lock wait occurs.
 */
void hda_codec_route_gate_leave(struct hda_codec *codec)
{
    __atomic_store_n(&codec->route_busy, 0u, __ATOMIC_RELEASE);
}

/** @brief Enter the codec transaction gate for a task-context mixer operation.
 * @param codec Codec cache whose route/control state is being serialized.
 * @return 0 when the gate is acquired, -EINVAL for a missing cache, or -EBUSY
 * when another route/control transaction owns it.
 * @par Context Task context only; no IRQ or audio-service callback may wait.
 * @par Ownership and locks/IRQ Caller owns the gate until the matching leave;
 * this is an atomic logical gate and no hardware lock spans codec verbs.
 */
int hda_codec_control_gate_enter(struct hda_codec *codec)
{
    return codec ? hda_codec_route_gate_enter(codec) : -EINVAL;
}

/** @brief Release a mixer/route transaction gate previously acquired by caller.
 * @param codec Codec cache whose gate is currently owned by the caller.
 * @return None; a null cache is ignored defensively.
 * @par Context Task context after the synchronous control operation completes.
 * @par Ownership and locks/IRQ Caller must own the logical gate; no hardware
 * access or wait occurs and IRQ state is unchanged.
 */
void hda_codec_control_gate_leave(struct hda_codec *codec)
{
    if (codec) hda_codec_route_gate_leave(codec);
}

/** @brief Apply one route while the enclosing codec route gate is owned.
 * @param codec Probed metadata whose logical route gate is held.
 * @param route Path resolved for this codec and caller-owned snapshot storage.
 * @param enable True to snapshot/apply; false to retry or complete restoration.
 * @return 0 on success or the first route, transport, or ownership error.
 * Failed rollback retains active state and snapshots for a later retry.
 *
 * @par Context
 * Task context outside global execution ownership and IRQ/service callbacks.
 * @par Ownership and locks/IRQ
 * Caller owns the logical gate; no spinlock spans verbs or readiness waits.
 */
static int hda_apply_route_locked(struct hda_codec *codec,
                                  struct hda_route *route, bool enable)
{
    int ret = hda_route_validate(codec, route);
    if (ret) return ret;
    if (enable) {
        if (route->active || route->snapshots_valid) return -EBUSY;
        if ((route->direction == HDA_ROUTE_PLAYBACK && !codec->has_playback) ||
            (route->direction == HDA_ROUTE_CAPTURE && !codec->has_capture))
            return -ENODEV;
        ret = hda_route_check_control_lease(codec, route);
        if (ret) return ret;
        ret = hda_route_take_snapshot(codec, route);
        if (ret) return ret;
        ret = hda_route_acquire_leases(codec, route);
        if (ret) {
            route->snapshots_valid = 0u;
            return ret;
        }
        ret = hda_route_apply_controls(codec, route);
        if (ret) {
            int apply_error = ret;
            int rollback = hda_route_restore_controls(codec, route);
            if (!rollback) rollback = hda_route_release_leases(codec, route);
            if (rollback) {
                route->active = 1u;
                route->rollback_failed = 1u;
                route->rollback_error = rollback;
                ++codec->active_routes;
                return rollback;
            }
            route->snapshots_valid = 0u;
            route->rollback_failed = 0u;
            route->rollback_error = 0;
            return apply_error;
        }
        route->active = 1u;
        route->rollback_failed = 0u;
        route->rollback_error = 0;
        ++codec->active_routes;
        return 0;
    }
    if (!route->active || !route->snapshots_valid || !codec->active_routes)
        return -EINVAL;
    ret = hda_route_restore_controls(codec, route);
    if (!ret) ret = hda_route_release_leases(codec, route);
    if (ret) {
        route->rollback_failed = 1u;
        route->rollback_error = ret;
        return ret;
    }
    route->active = 0u;
    route->rollback_failed = 0u;
    route->rollback_error = 0;
    route->snapshots_valid = 0u;
    --codec->active_routes;
    return 0;
}

/** @brief Apply one route or retry exact restoration.
 * @param codec Probed codec whose logical route gate serializes operations.
 * @param route Route returned by hda_find_route and owned by the caller.
 * @param enable True to apply; false to restore after PCM has stopped.
 * @return 0 on success or a route, transport, or ownership error. Failed
 * rollback retains the active route and snapshots for a later disable retry.
 *
 * @par Context
 * Task context outside global execution ownership and IRQ/service callbacks.
 * @par Ownership and locks/IRQ
 * One bounded nonspinning logical gate covers the transaction; it is not a
 * spinlock and no lock spans verbs or sleep. Caller serializes direct codec
 * control operations, stops PCM before topology changes, and keeps route and
 * codec objects alive until restoration completes.
 */
int hda_apply_route(struct hda_codec *codec, struct hda_route *route,
                    bool enable)
{
    if (!codec || !route) return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    ret = hda_apply_route_locked(codec, route, enable);
    hda_codec_route_gate_leave(codec);
    return ret;
}

/** @brief Apply or restore every member under one nonspinning route gate.
 * @param codec Probed metadata owning the group and route transaction gate.
 * @param group Live group returned by hda_find_route_group.
 * @param enable True to activate all members; false to restore/retry them.
 * @return 0 after all members transition or the first route/rollback error.
 * Partial state remains active with snapshots and leases available for retry.
 *
 * @par Context
 * Task context with PCM stopped during topology changes; not IRQ/service or
 * global execution ownership context.
 * @par Ownership and locks/IRQ
 * One bounded nonspinning codec gate covers preflight, all verbs/waits, and
 * rollback; no spinlock spans a verb or sleep. Group and codec lifetimes remain
 * caller-owned until every route is restored and the group is destroyed.
 */
int hda_apply_route_group(struct hda_codec *codec,
                          struct hda_route_group *group, bool enable)
{
    if (!codec || !group) return -EINVAL;
    int ret = hda_codec_route_gate_enter(codec);
    if (ret) return ret;
    ret = hda_route_group_validate(codec, group);
    if (ret) goto done;

    if (enable) {
        if (group->active) {
            ret = -EBUSY;
            goto done;
        }
        /* Reject all conflicts before taking snapshots or issuing any write. */
        for (uint32_t member = 0u; member < group->member_count; ++member) {
            struct hda_route *route = &group->members[member];
            if (route->active || route->snapshots_valid) {
                ret = -EBUSY;
                goto done;
            }
            ret = hda_route_check_control_lease(codec, route);
            if (ret) goto done;
        }
        uint32_t completed = 0u;
        for (; completed < group->member_count; ++completed) {
            ret = hda_apply_route_locked(codec, &group->members[completed],
                                         true);
            if (ret) break;
        }
        if (ret) {
            int original_error = ret;
            int rollback_error = 0;
            uint32_t rollback_end = completed +
                (group->members[completed].active ? 1u : 0u);
            while (rollback_end) {
                struct hda_route *route = &group->members[--rollback_end];
                if (!route->active) continue;
                int rollback = hda_apply_route_locked(codec, route, false);
                if (rollback && !rollback_error) rollback_error = rollback;
            }
            for (uint32_t member = 0u; member < group->member_count; ++member)
                if (group->members[member].active) group->active = 1u;
            group->rollback_failed = group->active;
            group->rollback_error = rollback_error;
            ret = rollback_error ? rollback_error : original_error;
            goto done;
        }
        group->active = 1u;
        group->rollback_failed = 0u;
        group->rollback_error = 0;
        ret = 0;
    } else {
        if (!group->active) {
            ret = -EINVAL;
            goto done;
        }
        int first_error = 0;
        for (uint32_t member = group->member_count; member > 0u; --member) {
            struct hda_route *route = &group->members[member - 1u];
            if (!route->active) continue;
            int stop = hda_apply_route_locked(codec, route, false);
            if (stop && !first_error) first_error = stop;
        }
        group->active = 0u;
        for (uint32_t member = 0u; member < group->member_count; ++member)
            if (group->members[member].active) group->active = 1u;
        group->rollback_failed = group->active;
        group->rollback_error = group->active ? first_error : 0;
        ret = first_error;
    }

done:
    hda_codec_route_gate_leave(codec);
    return ret;
}
