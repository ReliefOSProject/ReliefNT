#ifndef RELIEFNT_DRIVERS_SOUND_HDA_CODEC_C_INTERNAL_H
#define RELIEFNT_DRIVERS_SOUND_HDA_CODEC_C_INTERNAL_H

#include "hda.h"

#include <linux/errno.h>

#define HDA_CODEC_PAGE_BYTES 4096u
#define HDA_CODEC_CONNECTION_BYTES (HDA_CODEC_MAX_NIDS * HDA_CODEC_MAX_CONNECTIONS)
#define HDA_ROOT_NID 0u

#define HDA_VERB_GET_PARAMETER 0xf00u
#define HDA_VERB_GET_CONNECTION_LIST 0xf02u
#define HDA_VERB_GET_POWER_STATE 0xf05u
#define HDA_VERB_GET_CONNECTION_SELECT 0xf01u
#define HDA_VERB_GET_PIN_CONTROL 0xf07u
#define HDA_VERB_GET_EAPD 0xf0cu
#define HDA_VERB_GET_AMP 0x0bu
#define HDA_VERB_GET_CONFIG_DEFAULT 0xf1cu
#define HDA_VERB_GET_IMPLEMENTATION_ID 0xf20u
#define HDA_VERB_SET_POWER_STATE 0x705u
#define HDA_VERB_SET_CONNECTION_SELECT 0x701u
#define HDA_VERB_SET_PIN_CONTROL 0x707u
#define HDA_VERB_SET_EAPD 0x70cu
#define HDA_VERB_SET_AMP 0x03u
#define HDA_VERB_GET_STREAM_CHANNEL 0xf06u
#define HDA_VERB_SET_STREAM_CHANNEL 0x706u
#define HDA_VERB_GET_CONVERTER_FORMAT 0xa00u
#define HDA_VERB_SET_CONVERTER_FORMAT 0x02u

#define HDA_PARAM_VENDOR_ID 0x00u
#define HDA_PARAM_REVISION_ID 0x02u
#define HDA_PARAM_NODE_COUNT 0x04u
#define HDA_PARAM_FUNCTION_TYPE 0x05u
#define HDA_PARAM_AFG_CAPS 0x08u
#define HDA_PARAM_WIDGET_CAPS 0x09u
#define HDA_PARAM_PCM_RATES 0x0au
#define HDA_PARAM_STREAM_FORMATS 0x0bu
#define HDA_PARAM_PIN_CAPS 0x0cu
#define HDA_PARAM_AMP_INPUT_CAPS 0x0du
#define HDA_PARAM_CONNECTION_LIST_LENGTH 0x0eu
#define HDA_PARAM_AMP_OUTPUT_CAPS 0x12u

#define HDA_FUNCTION_GROUP_AUDIO 1u
#define HDA_CONNECTION_LONG_FORM 0x80u
#define HDA_CONNECTION_LENGTH_MASK 0x7fu
#define HDA_CONNECTION_RANGE_SHORT 0x80u
#define HDA_CONNECTION_MASK_SHORT 0x7fu
#define HDA_CONNECTION_RANGE_LONG 0x8000u
#define HDA_CONNECTION_MASK_LONG 0x7fffu

#define HDA_AMP_CAP_MUTE (1u << 31)
#define HDA_AMP_CAP_OFFSET_MASK 0x7fu
#define HDA_AMP_CAP_NUM_STEPS_SHIFT 8u
#define HDA_AMP_CAP_NUM_STEPS_MASK (0x7fu << HDA_AMP_CAP_NUM_STEPS_SHIFT)
#define HDA_AMP_SET_MUTE (1u << 7)
#define HDA_AMP_GAIN_MASK 0x7fu
#define HDA_AMP_GET_OUTPUT (1u << 15)
#define HDA_AMP_SET_OUTPUT (1u << 15)
#define HDA_AMP_SET_INPUT (1u << 14)
#define HDA_AMP_GET_LEFT (1u << 13)
#define HDA_AMP_SET_LEFT (1u << 13)
#define HDA_AMP_SET_RIGHT (1u << 12)
#define HDA_AMP_SET_INDEX_SHIFT 8u
#define HDA_AMP_GET_INDEX_MASK 0x0fu

#define HDA_POWER_ERROR (1u << 8)
#define HDA_POWER_ACT_SHIFT 4u
#define HDA_POWER_STATE_MASK 0x0fu
#define HDA_POWER_TRANSITION_TIMEOUT_TICKS 10u
#define HDA_POWER_TRANSITION_MAX_POLLS 16u

#define HDA_ROUTE_CHANGED_POWER (1u << 0)
#define HDA_ROUTE_CHANGED_SELECTOR (1u << 1)
#define HDA_ROUTE_CHANGED_PIN (1u << 2)
#define HDA_ROUTE_CHANGED_EAPD (1u << 3)
#define HDA_ROUTE_CHANGED_INPUT_AMP (1u << 4)
#define HDA_ROUTE_CHANGED_OUTPUT_AMP (1u << 5)




/** @brief Clear a bounded caller-owned CPU metadata region without libc imports.
 * @param storage Writable byte range; caller guarantees its size and lifetime.
 * @param byte_count Number of bytes to clear.
 * @return None; every byte in the requested range is set to zero.
 *
 * @par Context
 * Task-context metadata initialization and bounded route scratch setup.
 * @par Ownership and locks/IRQ
 * Caller retains the storage lease; this helper allocates nothing and takes no lock.
 */
void hda_codec_zero(void *storage, uint64_t byte_count);

/** @brief Map a whole caller-owned metadata run only when every page is in the direct map.
 * @param phys Physical base returned by alloc_pages; must be nonzero and page aligned.
 * @param pages Exact page count of the already-owned contiguous CPU metadata run.
 * @return Base CPU alias, or NULL for invalid arithmetic, a missing page alias, or a noncontiguous direct-map result.
 *
 * @par Context
 * Task-context codec allocation before metadata is published.
 * @par Ownership and locks/IRQ
 * The codec remains owner of phys/pages and must call free_pages with the exact original pair. This helper creates no DMA mapping or allocation and holds no lock.
 */
void *hda_codec_map_metadata_run(uint64_t phys, uint32_t pages);

/** @brief Expand bounded HDA connection entries into their individual NIDs.
 * @param raw Raw short- or long-form connection list entries.
 * @param count Number of raw entries, at most 256.
 * @param long_form True for 15-bit NIDs with bit 15 as the range marker.
 * @param first First NID in the owning function group's declared widget range.
 * @param nodes Number of NIDs in that range; the exclusive end may be 256.
 * @param out Caller-owned output byte array.
 * @param capacity Number of bytes available in @p out.
 * @return Expanded entry count, -EINVAL for malformed/out-of-range data, or
 * -EOVERFLOW when the expanded list does not fit.
 *
 * @par Context
 * Task-context codec discovery; pure bounded conversion with no hardware access.
 * @par Ownership and locks/IRQ
 * Caller owns both arrays; no allocation, ownership transfer, lock, or IRQ action.
 */
int hda_expand_connections(const uint16_t *raw, uint32_t count, bool long_form,
                           uint8_t first, uint16_t nodes, uint8_t *out,
                           uint32_t capacity);

/** @brief Discover generic audio function groups and cache each real analog route capability.
 * @param c Initialized controller whose codec mask contains @p cad.
 * @param cad Codec address in the legal range 0..14; 15 is reserved by the link.
 * @param codec Caller-owned zero-initialized metadata output.
 * @return 0, -EINVAL for invalid CAD/topology, -ENOMEM for metadata failure, or
 * the first propagated command error. Every failed probe releases its CPU pages.
 *
 * @par Context
 * Task-context initialization outside global execution ownership; not an IRQ/service operation.
 * @par Ownership and locks/IRQ
 * Success transfers exact alloc_pages runs to codec until destroy; they are CPU metadata only, never device DMA. No controller state lock is held across verbs.
 */
int hda_codec_probe(struct hda_controller *c, uint8_t cad,
                    struct hda_codec *codec);

/** @brief Enter a per-codec route transaction without holding a lock over verbs.
 * @param codec Cache whose atomic logical gate is acquired.
 * @return 0 when acquired or -EBUSY if another full route transaction is active.
 *
 * @par Context
 * Task context before a synchronous route operation.
 * @par Ownership and locks/IRQ
 * Uses one bounded atomic compare/exchange; it never spins, waits, disables IRQs, or holds a spinlock across a verb.
 */
int hda_codec_route_gate_enter(struct hda_codec *codec);

/** @brief Leave the per-codec logical route transaction.
 * @param codec Cache whose route transaction gate is currently owned by caller.
 * @return None; publishes completion to a future transaction.
 *
 * @par Context
 * Task context at every route transaction exit.
 * @par Ownership and locks/IRQ
 * Releases one atomic logical gate; no hardware access or lock wait occurs.
 */
void hda_codec_route_gate_leave(struct hda_codec *codec);

/** @brief Enter the codec transaction gate for a task-context mixer operation.
 * @param codec Codec cache whose route/control state is being serialized.
 * @return 0 when the gate is acquired, -EINVAL for a missing cache, or -EBUSY
 * when another route/control transaction owns it.
 * @par Context Task context only; no IRQ or audio-service callback may wait.
 * @par Ownership and locks/IRQ Caller owns the gate until the matching leave;
 * this is an atomic logical gate and no hardware lock spans codec verbs.
 */
int hda_codec_control_gate_enter(struct hda_codec *codec);

/** @brief Release a mixer/route transaction gate previously acquired by caller.
 * @param codec Codec cache whose gate is currently owned by the caller.
 * @return None; a null cache is ignored defensively.
 * @par Context Task context after the synchronous control operation completes.
 * @par Ownership and locks/IRQ Caller must own the logical gate; no hardware
 * access or wait occurs and IRQ state is unchanged.
 */
void hda_codec_control_gate_leave(struct hda_codec *codec);

/** @brief Release all metadata after confirming no route owns the cache.
 * @param codec Probed or partially initialized cache; no route may be active.
 * @return 0 after exact CPU-page reclamation, -EBUSY during a route transaction or with active routes.
 *
 * @par Context
 * Task teardown after streams have stopped and routes have been restored.
 * @par Ownership and locks/IRQ
 * Uses a bounded atomic gate, frees only codec-owned CPU metadata pages, and leaves controller transport ownership unchanged.
 */
int hda_codec_destroy(struct hda_codec *codec);

/** @brief Validate and return a node's expanded connection slice.
 * @param codec Cache whose pool owns the node's expanded connections.
 * @param node Parsed node with an offset/count pair to validate.
 * @param out Receives the borrowed connection slice (NULL when count is zero).
 * @return 0 or -EINVAL when the slice would escape the fixed codec pool.
 *
 * @par Context
 * Task-context route search over immutable probed metadata.
 * @par Ownership and locks/IRQ
 * Returned bytes are borrowed from codec and remain valid only while it is alive; no lock or allocation.
 */
int hda_codec_connection_slice(const struct hda_codec *codec,
                                     const struct hda_codec_node *node,
                                     const uint8_t **out);

/** @brief Resolve one unconstrained analog route for a pin.
 * @param codec Probed codec with immutable parsed graph metadata.
 * @param pin Requested Pin Complex NID.
 * @param direction Playback searches pin-to-DAC; capture searches ADC-to-pin.
 * @param route Caller-owned output path and later snapshot storage.
 * @return 0 with a real same-AFG analog path or the graph validation error.
 *
 * @par Context
 * Task context; bounded graph traversal performs no verbs or waits.
 * @par Ownership and locks/IRQ
 * route borrows codec, and caller serializes route-object reuse and codec
 * teardown. The function mutates only caller-owned route bytes.
 */
int hda_find_route(struct hda_codec *codec, uint8_t pin,
                   enum hda_route_direction direction,
                   struct hda_route *route);

/** @brief Validate a caller-owned route against the immutable parsed graph.
 * @param codec Probed codec whose route gate is held by the caller.
 * @param route Route path and edge indices to validate before any hardware access.
 * @return 0 for a same-AFG analog path whose edges still match, otherwise -EINVAL.
 *
 * @par Context
 * Task context while the full route transaction gate is owned.
 * @par Ownership and locks/IRQ
 * Reads only codec-owned metadata and caller-owned path bytes; no allocation, mutation, lock, or verb wait.
 */
int hda_route_validate(const struct hda_codec *codec,
                              const struct hda_route *route);

/** @brief Reject paths that would share an already leased signal widget.
 * @param codec Probed cache whose logical route gate is held.
 * @param route Validated path to check before any route write.
 * @return 0 if every node is free, -EBUSY for an active conflicting route.
 *
 * @par Context
 * Task context within a serialized codec route transaction.
 * @par Ownership and locks/IRQ
 * Reads codec-owned ownership bytes only; it performs no allocation or I/O.
 */
int hda_route_check_control_lease(const struct hda_codec *codec,
                                         const struct hda_route *route);

/** @brief Discover and solve one bounded analog playback association.
 * @param codec Probed codec with CPU metadata allocator and immutable graph.
 * @param pin Seed output pin whose association is requested.
 * @param group Zero-initialized caller-owned group result.
 * @return 0 with feasible sorted members, graph/allocation errors, -ENODEV
 * for association zero/no route, -EINVAL for duplicate sequences/shared paths,
 * or -EOVERFLOW when member/channel limits are exceeded.
 *
 * @par Context
 * Task context during stream preparation; graph planning performs no verbs.
 * @par Ownership and locks/IRQ
 * Acquires one bounded logical codec gate, allocates CPU-only exact pages for
 * member snapshots, and transfers them to group on success. Failure frees the
 * exact acquired run. Group borrows codec and must be destroyed first.
 */
int hda_find_route_group(struct hda_codec *codec, uint8_t pin,
                         struct hda_route_group *group);

/** @brief Free an inactive route group's exact CPU metadata run.
 * @param group Caller-owned group to release.
 * @return 0 after the group reference and pages are released, -EBUSY while
 * any member is active, or -EINVAL for inconsistent ownership metadata.
 *
 * @par Context
 * Task teardown after route/group operations have stopped.
 * @par Ownership and locks/IRQ
 * Uses the codec logical route gate; frees only the exact group alloc_pages
 * run, decrements the codec lifetime reference, and zeroes group on success.
 */
int hda_route_group_destroy(struct hda_route_group *group);

/** @brief Bind and verify one complete stream format on every route converter.
 * @param group Active route group whose codec metadata remains pinned.
 * @param tag HDA stream tag, or zero to release stream/channel and format.
 * @param format Complete HDA format word including the full-stream channel count.
 * @return 0 after capability checks and readback, or a negative transport/state errno.
 *
 * @par Context
 * Task context with the codec route gate; no IRQ/service caller is permitted.
 * @par Ownership and locks/IRQ
 * Caller retains the active route group; the route gate serializes codec verbs
 * and no interrupt state is changed.
 */
int hda_route_group_stream_bind(struct hda_route_group *group, uint8_t tag,
                                uint16_t format);

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
                               uint8_t index, bool right, uint16_t *out_value);

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
                                uint32_t amp_caps);

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
                                   struct hda_route *route);

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
                                    struct hda_route *route);

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
                                    struct hda_route *route);

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
                                    struct hda_route *route);

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
                                      struct hda_route *route);

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
                    bool enable);

/** @brief Validate group paths, channel ranges, and nonsharing constraints.
 * @param codec Probed codec that owns the immutable topology.
 * @param group Caller-owned group and its exact-page route members.
 * @return 0 for a complete feasible group, -EINVAL for stale/malformed data.
 *
 * @par Context
 * Task context while the codec logical route gate is held.
 * @par Ownership and locks/IRQ
 * Reads route/group and codec metadata only; no I/O, allocation, or mutation.
 */
int hda_route_group_validate(const struct hda_codec *codec,
                                    const struct hda_route_group *group);

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
                          struct hda_route_group *group, bool enable);

/** @brief Emit codec identity, NID capabilities/defaults, and expanded graph edges.
 * @param codec Successfully probed immutable codec metadata.
 * @return None; no output is emitted for a missing cache or console callback.
 *
 * @par Context
 * Task diagnostics while probe metadata is alive; this does not query hardware.
 * @par Ownership and locks/IRQ
 * Reads CPU metadata only and borrows console_log. Output contains numeric codec/NID values and never a kernel address.
 */
void hda_codec_dump(const struct hda_codec *codec);

#endif
