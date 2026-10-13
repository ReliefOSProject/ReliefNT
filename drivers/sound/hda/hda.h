#ifndef RELIEFNT_HDA_CONTROLLER_H
#define RELIEFNT_HDA_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>
#include <reliefos/driver.h>
#include <reliefos/driver_audio.h>

/** @brief Expand HDA short- or long-form connection entries into node IDs.
 * @param raw Raw 16-bit entries returned by GET_CONNECTION_LIST_ENTRY.
 * @param count Number of raw entries; at most 256.
 * @param long_form True when entries use 15-bit NIDs and bit 15 range markers.
 * @param first First valid NID in the owning function group's widget range.
 * @param nodes Number of NIDs in that range; the checked end may be 256.
 * @param out Caller-owned output array with @p capacity byte slots.
 * @param capacity Output capacity in NIDs.
 * @return Expanded NID count, -EINVAL for malformed/out-of-range input, or
 * -EOVERFLOW when the output cannot hold the expanded list.
 *
 * @par Context
 * Task-context codec discovery; pure bounded conversion with no hardware I/O.
 * @par Ownership and locks/IRQ
 * Caller retains all buffers; no allocation, ownership transfer, lock, or IRQ action.
 */
int hda_expand_connections(const uint16_t *raw, uint32_t count, bool long_form,
                           uint8_t first, uint16_t nodes, uint8_t *out,
                           uint32_t capacity);

#define HDA_REGISTER_BYTES 0x4000u
#define HDA_CODEC_COUNT 16u
#define HDA_UNSOLICITED_CAPACITY 32u
#define HDA_SERVICE_BUDGET 32u
#define HDA_CODEC_MAX_NIDS 256u
#define HDA_CODEC_MAX_CONNECTIONS 256u
#define HDA_ROUTE_MAX_NODES 256u
#define HDA_ROUTE_GROUP_MAX_MEMBERS 16u
#define HDA_CODEC_MAX_CAD 14u
#define HDA_STREAM_MAX 16u
#define HDA_BDL_MAX 256u
#define HDA_BDL_ENTRY_BYTES 16u
#define HDA_CONTROL_PLAYBACK_VOLUME 0u
#define HDA_CONTROL_PLAYBACK_MUTE 1u
#define HDA_CONTROL_CAPTURE_SOURCE 2u
#define HDA_CONTROL_HEADPHONE_JACK 3u
#define HDA_CONTROL_AUTO_MUTE 4u
#define HDA_PIN_SENSE_PRESENCE (1u << 31)
#define HDA_PIN_SENSE_IMPEDANCE_MASK 0x7fffffffu
#define HDA_UNSOLICITED_ENABLE (1u << 7)
#define HDA_UNSOLICITED_TAG_MASK 0x3fu

enum hda_stream_state {
    HDA_STREAM_FREE = 0,
    HDA_STREAM_OPEN,
    HDA_STREAM_PREPARED,
    HDA_STREAM_RUNNING,
    HDA_STREAM_PAUSED,
    HDA_STREAM_XRUN,
    HDA_STREAM_STOPPED,
};

struct hda_bdl_entry {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
};

#define HDA_BDL_IOC 1u

#define HDA_WCAP_IN_AMP          (1u << 1)
#define HDA_WCAP_OUT_AMP         (1u << 2)
#define HDA_WCAP_STEREO          (1u << 0)
#define HDA_WCAP_AMP_OVERRIDE    (1u << 3)
#define HDA_WCAP_FORMAT_OVERRIDE (1u << 4)
#define HDA_WCAP_UNSOLICITED      (1u << 7)
#define HDA_WCAP_CONN_LIST       (1u << 8)
#define HDA_WCAP_DIGITAL         (1u << 9)
#define HDA_WCAP_POWER           (1u << 10)

#define HDA_PINCAP_EAPD          (1u << 16)
#define HDA_PINCAP_INPUT         (1u << 5)
#define HDA_PINCAP_OUTPUT        (1u << 4)
#define HDA_PINCAP_PRESENCE       (1u << 2)
#define HDA_PINCAP_VREF_80       (1u << 12)

#define HDA_PINCTL_HP_ENABLE     (1u << 7)
#define HDA_PINCTL_OUTPUT_ENABLE (1u << 6)
#define HDA_PINCTL_INPUT_ENABLE  (1u << 5)
#define HDA_PINCTL_VREF_MASK     7u
#define HDA_PINCTL_VREF_80       4u

enum hda_widget_type {
    HDA_WIDGET_AUDIO_OUTPUT = 0,
    HDA_WIDGET_AUDIO_INPUT = 1,
    HDA_WIDGET_PIN = 4,
    HDA_WIDGET_MIXER = 2,
    HDA_WIDGET_SELECTOR = 3,
    HDA_WIDGET_FUNCTION_GROUP = 0x10,
};

enum hda_route_direction {
    HDA_ROUTE_PLAYBACK = 0,
    HDA_ROUTE_CAPTURE = 1,
};

/** @brief Parsed immutable capabilities for one codec node.
 * Connections are stored in the owning codec's connection pool at
 * connection_offset; amp/format fields contain effective local or inherited
 * capabilities as specified by the AFG override bits.
 */
struct hda_codec_node {
    uint32_t widget_caps;
    uint32_t implementation_id;
    uint32_t pin_caps;
    uint32_t pin_default;
    uint32_t pcm_rates;
    uint32_t stream_formats;
    uint32_t connection_offset;
    uint32_t amp_input_caps;
    uint32_t amp_output_caps;
    uint16_t connection_count;
    uint8_t type;
    uint8_t afg_nid;
    uint8_t nid;
    uint8_t present;
    uint8_t reserved[2];
};

/** @brief CPU-only codec discovery cache with exact physical-run ownership.
 * Nodes and expanded connections are aliased through the checked kernel direct
 * map. The controller and kernel API are borrowed and outlive this object.
 */
struct hda_codec {
    struct hda_controller *controller;
    const struct reliefos_driver_kernel_api *api;
    struct hda_codec_node *nodes;
    uint8_t *connections;
    uint64_t nodes_phys;
    uint64_t connections_phys;
    uint32_t nodes_pages;
    uint32_t connections_pages;
    uint32_t vendor_id;
    uint32_t revision_id;
    uint32_t subsystem_id; /**< Compatibility value: first audio AFG IID. */
    uint32_t afg_caps;
    uint32_t afg_pcm_rates;
    uint32_t afg_stream_formats;
    uint32_t afg_amp_input_caps;
    uint32_t afg_amp_output_caps;
    uint32_t playback_pcm_rates;
    uint32_t playback_stream_formats;
    uint32_t capture_pcm_rates;
    uint32_t capture_stream_formats;
    uint32_t route_busy;
    uint32_t active_routes;
    uint32_t route_groups;
    /** Exclusive control-node owners; routes sharing signal widgets are rejected. */
    uint8_t route_node_owner[HDA_CODEC_MAX_NIDS];
    /** First route's original AFG Set state, retained until last safe release. */
    uint8_t afg_lease_saved_power[HDA_CODEC_MAX_NIDS];
    uint8_t afg_lease_valid[HDA_CODEC_MAX_NIDS];
    uint32_t afg_lease_count[HDA_CODEC_MAX_NIDS];
    uint8_t cad;
    uint8_t root_fg_start;
    uint8_t root_fg_count;
    uint8_t has_playback;
    uint8_t has_capture;
    uint8_t probed;
    uint8_t reserved[2];
};

/** @brief One resolved analog path and its reversible control snapshot.
 * The caller owns this object and must keep it live until hda_apply_route(false)
 * restores the pre-route controls and releases its shared AFG lease.
 * path[] is ordered pin-to-DAC for playback and ADC-to-pin for capture;
 * connection_index[i] selects path[i + 1] from node i. Active routes reserve
 * every signal-control node exclusively; a conflicting route is rejected.
 */
struct hda_route {
    struct hda_codec *codec;
    uint8_t path[HDA_ROUTE_MAX_NODES];
    uint8_t connection_index[HDA_ROUTE_MAX_NODES];
    uint8_t saved_flags[HDA_ROUTE_MAX_NODES];
    uint8_t changed_flags[HDA_ROUTE_MAX_NODES];
    uint8_t saved_power[HDA_ROUTE_MAX_NODES];
    uint8_t saved_selector[HDA_ROUTE_MAX_NODES];
    uint8_t saved_pin_control[HDA_ROUTE_MAX_NODES];
    uint8_t saved_eapd[HDA_ROUTE_MAX_NODES];
    uint16_t saved_input_amp[HDA_ROUTE_MAX_NODES][2];
    uint16_t saved_output_amp[HDA_ROUTE_MAX_NODES][2];
    int32_t rollback_error;
    uint8_t saved_afg_power;
    uint16_t path_count;
    uint8_t direction;
    uint8_t pin_nid;
    uint8_t converter_nid;
    uint8_t association;
    uint8_t sequence;
    uint8_t active;
    uint8_t rollback_failed;
    uint8_t snapshots_valid;
    uint8_t afg_lease_acquired;
    uint8_t control_lease_acquired;
    uint8_t channel_start;
    uint8_t channel_count;
    uint8_t stream_tag;
    uint16_t stream_format;
    uint8_t stream_bound;
    uint8_t stream_bind_dirty;
    int32_t stream_bind_error;
};

/** @brief Caller-owned playback association with exact-page route storage.
 * Members are sorted by sparse Pin Default sequence and contain distinct
 * converter paths with contiguous H4 stream-channel ranges. The caller must
 * destroy the inactive group before codec teardown.
 */
struct hda_route_group {
    struct hda_codec *codec;
    struct hda_route *members;
    uint64_t members_phys;
    uint32_t members_pages;
    int32_t rollback_error;
    uint16_t member_count;
    uint8_t association;
    uint8_t total_channels;
    uint8_t active;
    uint8_t rollback_failed;
    uint8_t reserved[2];
};

/** @brief One controller-owned HDA DMA stream and its generation state.
 * Persistent BDL/position pages belong to the controller for its lifetime;
 * the PCM DMA ring is borrowed from the audio core during prepare. Stream
 * callbacks never mutate route snapshots or wait for codec verbs.
 */
struct hda_stream {
    struct hda_controller *controller;
    struct hda_route_group *group;
    struct audio_dma bdl_dma;
    struct audio_dma position_dma;
    volatile uint32_t *position;
    struct hda_bdl_entry *bdl;
    struct audio_dma pcm_dma;
    struct audio_params_ext selected;
    uint32_t id;
    uint32_t index;
    uint32_t card_id;
    uint32_t period_bytes;
    uint32_t period_count;
    uint32_t cbl;
    uint32_t lvi;
    uint32_t bdl_entries;
    uint32_t hw_position;
    uint32_t last_position;
    uint32_t last_period;
    uint64_t position_epoch;
    uint64_t frame_position;
    uint64_t reported_frames;
    uint64_t position_tick;
    uint32_t position_wallclock;
    uint32_t position_lock;
    uint64_t generation;
    uint8_t direction;
    uint8_t tag;
    uint8_t configured;
    uint8_t xrun_reported;
    uint8_t position_valid;
    enum hda_stream_state state;
};

struct hda_unsolicited_response {
    uint8_t cad;
    uint8_t reserved[3];
    uint32_t response;
};

enum hda_transport_recovery_state {
    HDA_RECOVERY_IDLE = 0,
    HDA_RECOVERY_STOP_RINGS,
    HDA_RECOVERY_WAIT_CRST_LOW,
    HDA_RECOVERY_WAIT_CRST_HIGH,
    HDA_RECOVERY_WAIT_STABLE,
    HDA_RECOVERY_RESET_RINGS,
    HDA_RECOVERY_IMMEDIATE_CLEAR,
    HDA_RECOVERY_FAILED,
};

/** @brief Private command transports exposed to the codec/jack consumers.
 * Ring mode supports RIRB unsolicited events; immediate mode supports only
 * serialized solicited commands and requires bounded jack polling.
 */
enum hda_transport_mode {
    HDA_TRANSPORT_UNAVAILABLE = 0,
    HDA_TRANSPORT_RINGS,
    HDA_TRANSPORT_IMMEDIATE,
};

struct hda_controller_lock {
    uint32_t held;
};

struct hda_verb_flight {
    uint64_t ticket;
    uint64_t start_tick;
    uint32_t response;
    int32_t error;
    uint8_t cad;
    uint8_t pending;
    uint8_t complete;
    uint8_t reserved;
};

/** @brief Controller-owned BAR, transport state, and recovery gate.
 * The MMIO mapping, CORB/RIRB pages, and optional MSI token remain owned until
 * destroy proves DMA quiescent. One software flight is shared by synchronous
 * and submit/poll users. Ring mode supports unsolicited RIRB events; restricted
 * immediate mode retains the ticket contract but has no unsolicited events.
 */
struct hda_controller {
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device dev;
    void *mmio;
    uint64_t bar_base;
    uint64_t bar_bytes;
    uint64_t bar_size;
    uint16_t pci_command_original;
    uint8_t pci_command_valid;
    uint8_t corb_size_caps;
    uint8_t rirb_size_caps;
    uint8_t corb_size_select;
    uint8_t rirb_size_select;
    uint8_t rings_started;
    uint8_t immediate_version;
    enum hda_transport_mode transport_mode;
    uint16_t gcap;
    uint16_t codec_mask;
    uint32_t card_id;
    uint32_t irq_handle;
    uint64_t corb_phys;
    uint64_t rirb_phys;
    volatile uint32_t *corb;
    volatile uint64_t *rirb;
    uint32_t corb_pages;
    uint32_t rirb_pages;
    uint16_t corb_entries;
    uint16_t rirb_entries;
    uint16_t corb_write_pointer;
    uint16_t rirb_read_pointer;
    struct hda_controller_lock lock;
    struct hda_verb_flight flight;
    uint64_t next_ticket;
    struct hda_unsolicited_response unsolicited[HDA_UNSOLICITED_CAPACITY];
    uint32_t unsolicited_head;
    uint32_t unsolicited_count;
    uint32_t unsolicited_dropped;
    uint32_t unexpected_responses;
    uint32_t active_streams;
    uint32_t recovery_allowed;
    uint32_t recovery_requires_task;
    uint32_t transport_poisoned;
    uint32_t consecutive_faults;
    uint32_t disconnect_requested;
    uint32_t initialized;
    struct hda_stream *streams;
    uint32_t stream_count;
    uint32_t stream_generation;
    uint16_t stream_tag_mask;
    uint16_t stream_irq_mask;
    uint64_t position_phys;
    volatile uint8_t *position_buffer;
    uint32_t position_pages;
    uint32_t owns_stream_dma;
    uint32_t owns_mmio;
    uint32_t owns_corb;
    uint32_t owns_rirb;
    uint32_t owns_irq;
    enum hda_transport_recovery_state recovery_state;
    uint64_t recovery_start_tick;
    uint32_t recovery_step;
    int32_t recovery_error;
};

/** @brief Task/service-owned mixer and jack policy for one probed codec.
 * The object contains only bounded scalar state and borrows codec/routes. Route
 * and codec lifetimes must outlive it. Synchronous verbs are task-only; service
 * advances at most one nonblocking sense flight per call.
 */
struct hda_controls {
    uint32_t busy; /**< Nonwaiting state transaction gate; no spinlock spans verbs. */
    struct hda_codec *codec;
    struct hda_route *speaker_route;
    struct hda_route *headphone_route;
    struct hda_route *capture_route;
    uint8_t speaker_pin;
    uint8_t playback_amp_nid; /**< First actual output amp along the speaker path. */
    uint8_t headphone_pin;
    uint8_t capture_source_count;
    uint8_t auto_mute;
    uint8_t headphone_present;
    uint8_t playback_mute;
    uint8_t playback_gain[2];
    uint8_t capture_source;
    uint8_t speaker_tag;
    uint8_t headphone_tag;
    uint8_t unsolicited_enabled;
    uint8_t sense_inflight;
    uint8_t sense_pending;
    uint8_t poll_only;
    uint8_t auto_mute_pending;
    uint8_t policy_mute; /**< Speaker amp overlay; cached mute remains the user value. */
    int (*apply_policy)(void *opaque, bool enabled, bool headphone_present);
    void *policy_opaque; /**< Optional task-only stream/group owner; borrows card. */
    uint64_t sense_ticket;
    uint64_t next_poll_tick;
    uint32_t jack_events;
    uint32_t unsol_dropped_seen;
    int32_t last_error;
    uint32_t card_id;
    uint32_t control_index[5]; /**< Published index by internal ID; UINT32_MAX hides it. */
};

/** @brief Sleep through the controller's H1 clock wrapper in task context.
 * @param c Initialized controller whose kernel API remains live.
 * @param milliseconds Requested bounded sleep duration.
 * @return None; a missing controller/API/sleep callback is ignored defensively.
 *
 * @par Context
 * Task context only while controller/API lifetime is held; IRQ/service callbacks
 * and global-execution-owned callbacks must not call this wait helper.
 * @par Ownership and locks/IRQ
 * Caller retains controller/API ownership and must hold no controller spinlock.
 * H1 sleep permits IRQ progress and this wrapper restores the caller's entry IF.
 */
void hda_controller_sleep_ms(struct hda_controller *c,
                             uint64_t milliseconds);

/** @brief Pack a codec address, node, verb, and payload into one CORB word.
 * @param cad Codec address in the range 0..15.
 * @param nid Codec node identifier.
 * @param verb Verb field; low 12 bits for long form or low 4 bits for short form.
 * @param payload Payload; low 8 bits for long form or low 16 bits for short form.
 * @param short_verb Selects the 4-bit verb plus 16-bit payload encoding.
 * @return Encoded 32-bit codec command.
 *
 * @par Context
 * Any context; pure value conversion.
 * @par Ownership and locks/IRQ
 * No resource ownership, locks, or IRQ state; safe from IRQ context.
 */
uint32_t hda_encode_verb(uint8_t cad, uint8_t nid, uint16_t verb,
                         uint16_t payload, bool short_verb);

/** @brief Quiesce, map, reset, and start a PCI HDA controller's codec transport.
 * @param c Caller-owned zeroed controller state; failed init may retain unsafe leases.
 * @param api Size-gated H1 driver API with PCI, MMIO, DMA, clock, and IRQ services.
 * @param dev Borrowed PCI function identity; device must be quiescent before BAR probing.
 * @return 0 or negative errno. A failure releases resources after DMA quiescence is
 * proven; otherwise c retains leases and caller must retry destroy.
 *
 * @par Context
 * Task init wait phase after the caller quiesces the PCI function.
 * @par Ownership and locks/IRQ
 * On success c owns mapped MMIO plus rings/optional IRQ in ring mode, or MMIO alone in immediate mode; on an unprovable stop it retains partial ownership for destroy retry. No lock over waits; IRQ progress continues.
 * Ring-init hardware failure may instead produce logged immediate mode after
 * a verified reset; immediate mode supports solicited commands and bounded jack polling only.
 */
int hda_controller_init(struct hda_controller *c,
                        const struct reliefos_driver_kernel_api *api,
                        const struct reliefos_driver_pci_device *dev);

/** @brief Stop controller DMA and release all owned IRQ, ring, and MMIO resources.
 * @param c Controller with resource ownership, including a partially initialized
 * instance retained after init could not prove DMA quiescence; stream users are stopped.
 * @return 0 after safe release, -EBUSY while PCM users remain, or a negative
 * transport error. On an unproven DMA stop, leases remain live for retry.
 * Task context; does not rely on an IRQ after manager cancellation.
 *
 * @par Context
 * Task teardown after every stream owner has stopped/released.
 * @par Ownership and locks/IRQ
 * Success returns all c-owned leases; a quiesce error retains unsafe leases for retry. Waits hold no state lock; IRQ is synchronized before free.
 */
int hda_controller_destroy(struct hda_controller *c);

/** @brief Submit one nonblocking codec command into the shared single-flight slot.
 * @param c Initialized, unpoisoned controller.
 * @param cad Codec address (0..15).
 * @param nid Codec node identifier.
 * @param verb Verb field.
 * @param payload Payload field.
 * @param short_verb Select short 4-bit-verb encoding.
 * @param out_ticket Receives a monotonically increasing, single-consumer ticket.
 * @return 0, -EBUSY for an occupied flight, -EIO while poisoned, -ENOSPC after
 * ticket exhaustion, or another negative validation/transport errno. Ring mode
 * publishes CORB; immediate mode uses ICOI/ICIS after verifying rings are stopped.
 * No wait/sleep.
 *
 * @par Context
 * Task or bounded nonblocking audio-service context; not a hard IRQ handler.
 * @par Ownership and locks/IRQ
 * The returned ticket has one polling owner; c retains mode-specific transport leases. A short local-IRQ-safe lock publishes a CORB or ICOI command; no wait/allocation.
 */
int hda_verb_submit(struct hda_controller *c, uint8_t cad, uint8_t nid,
                    uint16_t verb, uint16_t payload, bool short_verb,
                    uint64_t *out_ticket);

/** @brief Read and consume completion for the ticket that owns the active flight.
 * @param c Initialized controller.
 * @param ticket Ticket returned by hda_verb_submit.
 * @param out_response Receives a solicited codec response on success.
 * @return 0 with response, -EAGAIN while pending, a retained transport errno on
 * completion, or -ENOENT for a stale/non-owner ticket. Polling never waits.
 *
 * @par Context
 * Task or bounded nonblocking audio-service context.
 * @par Ownership and locks/IRQ
 * Ticket owner consumes the result once; c retains controller leases. Short state lock only; no wait/allocation.
 */
int hda_verb_poll(struct hda_controller *c, uint64_t ticket,
                  uint32_t *out_response);

/** @brief Execute one serialized codec command while allowing IRQ/service progress.
 * @param c Initialized controller.
 * @param cad Codec address (0..15).
 * @param nid Codec node identifier.
 * @param verb Verb field.
 * @param payload Payload field.
 * @param short_verb Select short 4-bit-verb encoding.
 * @param out_response Receives the solicited response.
 * @return 0 or a negative errno. Task context without execution ownership; lock is
 * released before every service, clock wait, and possible transport recovery.
 *
 * @par Context
 * Task context only, outside execution ownership.
 * @par Ownership and locks/IRQ
 * The controller retains transport resources; caller owns a temporary ticket. Lock is dropped before wait/service/recovery, with IRQ progress allowed.
 */
int hda_exec_verb(struct hda_controller *c, uint8_t cad, uint8_t nid,
                  uint16_t verb, uint16_t payload, bool short_verb,
                  uint32_t *out_response);

/** @brief Consume transport work within the caller's response budget.
 * @param c Initialized controller.
 * @param budget Maximum RIRB entries to consume; clamped to HDA_SERVICE_BUDGET.
 * A positive immediate-mode budget permits one bounded ICIS/ICII observation.
 * @return Number of RIRB entries consumed (zero in immediate mode). IRQ/tick safe, no allocation, sleep,
 * execution transaction, or synchronous verb wait; recovery advances by state.
 *
 * @par Context
 * Task/audio-service/tick or hard IRQ context.
 * @par Ownership and locks/IRQ
 * Controller retains MMIO/ring ownership; helper uses only its short state lock and is allocation-free/nonblocking.
 */
uint32_t hda_controller_service_budget(struct hda_controller *c, uint32_t budget);

/** @brief Run one default bounded controller-service pass.
 * @param c Initialized controller.
 * @return Number of RIRB entries consumed, at most HDA_SERVICE_BUDGET (zero in
 * immediate mode). IRQ-safe; immediate tickets still progress nonblockingly.
 *
 * @par Context
 * Task/audio-service/tick or hard IRQ context.
 * @par Ownership and locks/IRQ
 * Controller retains MMIO/ring ownership; helper is bounded, lock-protected, allocation-free, and nonblocking.
 */
uint32_t hda_controller_service(struct hda_controller *c);

/** @brief Remove the oldest unsolicited codec response from the fixed queue.
 * @param c Initialized controller.
 * @param out Receives the codec address and response.
 * @return 0 or -EAGAIN when no event is queued. Task context; lock held only to copy.
 *
 * @par Context
 * Task/audio-service context.
 * @par Ownership and locks/IRQ
 * Copies an event from the controller-owned queue; short state lock and no wait/IRQ action.
 */
int hda_unsolicited_pop(struct hda_controller *c,
                        struct hda_unsolicited_response *out);

/** @brief Remove the oldest event for one codec without consuming other CADs.
 * @param c Initialized controller owning the shared unsolicited queue.
 * @param cad Codec address in 0..15 whose event may be removed.
 * @param out Non-NULL caller storage receiving a copied event on success only.
 * @return 0, -EAGAIN when this CAD has no event, -EINVAL for invalid arguments,
 * or -ENODEV when the controller is not initialized.
 * @par Context Task or bounded audio-service context; no allocation or wait.
 * @par Ownership and locks/IRQ A fixed-capacity scan/compaction holds only the
 * IRQ-safe controller state lock; other codecs keep their events in FIFO order.
 */
int hda_unsolicited_pop_for_codec(struct hda_controller *c, uint8_t cad,
                                  struct hda_unsolicited_response *out);

/** @brief Read the monotonically increasing unsolicited overflow count.
 * @param c Initialized controller whose unsolicited queue is live.
 * @return Dropped-response count, or zero for an invalid/uninitialized controller.
 * @par Context Task or bounded audio-service context; this helper never waits.
 * @par Ownership and locks/IRQ Controller ownership remains unchanged; only a
 * short state lock is taken and no MMIO, allocation, or verb is issued.
 */
uint32_t hda_unsolicited_dropped_count(struct hda_controller *c);

/** @brief Record that a PCM stream has acquired this controller.
 * @param c Initialized controller.
 * @return 0 or -EIO if transport is poisoned. Task context; increments the
 * active-stream count and disables automatic full-controller recovery.
 *
 * @par Context
 * Task context before a PCM owner starts using the controller.
 * @par Ownership and locks/IRQ
 * Tracks one stream-gate lease without transferring hardware ownership; short state lock only.
 */
int hda_controller_stream_acquire(struct hda_controller *c);

/** @brief Record that a PCM stream has been stopped and released.
 * @param c Initialized controller with a previously acquired stream.
 * @return 0 or -EINVAL if no stream was acquired. Task context; does not recover.
 *
 * @par Context
 * Task context after the PCM DMA engine is stopped.
 * @par Ownership and locks/IRQ
 * Returns one tracked gate lease; controller DMA ownership remains. Short state lock, no implicit recovery.
 */
int hda_controller_stream_release(struct hda_controller *c);

/** @brief Encode an Intel HDA stream format word from rate, precision and channels.
 * @param rate Integer sample rate supported by the HDA descriptor encoding.
 * @param bits Converter precision (8, 16, 20, 24, or 32).
 * @param channels Channel count in the range 1..16.
 * @param out Receives the format word.
 * @return 0 or -EINVAL/-ERANGE for unsupported combinations. Pure, IRQ-safe.
 */
int hda_format_encode(uint32_t rate, uint32_t bits, uint32_t channels,
                      uint16_t *out);

/** @brief Allocate controller-owned BDL and position DMA for each stream slot.
 * @param c Initialized controller with H1 DMA callbacks.
 * @param table Caller-owned stream metadata array with @p count entries.
 * @param count Number of stream slots, 1..HDA_STREAM_MAX.
 * @return 0 or a negative errno; failure frees every exact DMA run allocated.
 * Task context only; no stream may be open.
 */
int hda_stream_controller_init(struct hda_controller *c,
                               struct hda_stream *table, uint32_t count);

/** @brief Release controller-owned stream DMA after every stream is stopped.
 * @param c Controller whose stream table was initialized.
 * @return 0 or -EBUSY/-EIO; failed stop retains all DMA ownership for retry.
 * Task context only, never from IRQ or callback paths.
 */
int hda_stream_controller_destroy(struct hda_controller *c);

/** @brief Query HDA-native selected formats and one-period BDL geometry.
 * @param opaque Controller context.
 * @param device Stream slot/index.
 * @param direction Playback/capture selector.
 * @param caps Receives format/subformat and alignment bounds.
 * @return 0 or a negative errno. Task context with no active PCM callback.
 */
int hda_pcm_format_caps(void *opaque, uint32_t device,
                        enum audio_direction direction,
                        struct audio_format_caps *caps);

/** @brief Query the legacy PCM capability prefix for an HDA stream.
 * @param opaque Controller context.
 * @param device Stream slot/index.
 * @param direction Playback/capture selector.
 * @param caps Receives format/rate/channel and byte bounds.
 * @return 0 or a negative errno. Task context only.
 */
int hda_pcm_caps(void *opaque, uint32_t device, enum audio_direction direction,
                 struct audio_caps *caps);

/** @brief Acquire a stopped stream and return its generation-tagged hardware lease.
 * @param opaque Controller context.
 * @param device Stream slot/index.
 * @param direction Playback/capture selector.
 * @param stream Receives a lease retained until successful STOP and close.
 * @return 0 or a negative errno. Task context; controller gate is short-lived.
 */
int hda_pcm_open(void *opaque, uint32_t device, enum audio_direction direction,
                 struct audio_hw_stream *stream);

/** @brief Prepare an unambiguous legacy S16 stream using the explicit path.
 * @param opaque Controller context.
 * @param stream Borrowed open stream lease.
 * @param params Legacy validated S16 parameters.
 * @param dma Borrowed core-owned PCM DMA ring.
 * @return 0 or a negative errno; does not set RUN.
 */
int hda_pcm_prepare(void *opaque, struct audio_hw_stream *stream,
                    const struct audio_params *params,
                    const struct audio_dma *dma);

/** @brief Prepare a selected memory format and converter precision.
 * @param opaque Controller context.
 * @param stream Borrowed open stream lease.
 * @param params Selected format/subformat/significant bits by value.
 * @param dma Borrowed core-owned PCM DMA ring.
 * @return 0 or a negative errno; task context only and never starts DMA.
 */
int hda_pcm_prepare_format(void *opaque, struct audio_hw_stream *stream,
                           const struct audio_params_ext *params,
                           const struct audio_dma *dma);

/** @brief Start, stop, pause, or resume a stream while preserving STOP retention.
 * @param opaque Controller context.
 * @param stream Borrowed stream lease.
 * @param trigger Trigger operation; STOP must synchronously clear RUN.
 * @return 0 or a negative errno; IRQ-safe, allocation-free, and nonblocking.
 */
int hda_pcm_trigger(void *opaque, struct audio_hw_stream *stream,
                    enum audio_trigger trigger);

/** @brief Read a monotonic frame position from the position buffer/LPIB fallback.
 * @param opaque Controller context.
 * @param stream Borrowed stream lease.
 * @param frames Receives completed frames.
 * @return 0 or -EPIPE for an unprovable wrap/XRUN. IRQ-safe, no waiting.
 */
int hda_pcm_pointer(void *opaque, struct audio_hw_stream *stream,
                    uint64_t *frames);

/** @brief Release a stopped stream lease while retaining controller DMA pages.
 * @param opaque Controller context.
 * @param stream Borrowed stream lease consumed by close.
 * @return None; no wait or codec operation.
 */
void hda_pcm_close(void *opaque, struct audio_hw_stream *stream);

/** @brief Service bounded stream INTSTS/SDSTS work while controller lock is held.
 * @param c Initialized controller.
 * @param budget Remaining global completion budget.
 * @return Number of period/error completions consumed, bounded by budget.
 * IRQ-safe, allocation-free, and callback publication only.
 */
uint32_t hda_stream_service_locked(struct hda_controller *c, uint32_t budget);

/** @brief Forward stream completion through the size-checked module API.
 * @param card Core card generation token.
 * @param stream Hardware stream generation token.
 * @param frames Completed frame count.
 * @param error Negative stream error or zero.
 * @return None. Bounded IRQ callback.
 */
void hda_audio_period_elapsed(uint32_t card, uint32_t stream, uint64_t frames, int error);

uint8_t hda_mmio_read8(const struct hda_controller *c, uint32_t offset);
uint16_t hda_mmio_read16(const struct hda_controller *c, uint32_t offset);
uint32_t hda_mmio_read32(const struct hda_controller *c, uint32_t offset);
void hda_mmio_write8(struct hda_controller *c, uint32_t offset, uint8_t value);
void hda_mmio_write16(struct hda_controller *c, uint32_t offset, uint16_t value);
void hda_mmio_write32(struct hda_controller *c, uint32_t offset, uint32_t value);

/** @brief Bind an accepted H3 route group to a stopped stream.
 * @param stream Open stream lease.
 * @param group Valid group whose members remain live until close.
 * @return 0 or -EINVAL/-EBUSY/-ERANGE. Task context; no route mutation.
 */
int hda_stream_bind_group(struct hda_stream *stream,
                          struct hda_route_group *group);

/** @brief Permit or forbid full transport recovery after the caller quiesces streams.
 * @param c Initialized controller.
 * @param allowed True only after every PCM DMA engine and stream route is stopped.
 * @return 0 or -EBUSY when allowing recovery with active stream leases. Task context.
 *
 * @par Context
 * Task context after all PCM streams/routes are quiesced.
 * @par Ownership and locks/IRQ
 * Changes only caller authorization; no ownership transfer or hardware reset. Short state lock.
 */
int hda_controller_set_recovery_allowed(struct hda_controller *c, bool allowed);

/** @brief Perform an explicit task-context reset after PCM users have stopped.
 * @param c Initialized poisoned controller.
 * @return 0 or negative errno; -EBUSY if recovery is forbidden or any stream remains.
 * Performs actual controller/link reset and mode-specific stale response cleanup before reopening submissions.
 *
 * @par Context
 * Task context only after every stream is stopped and caller permits reset.
 * @par Ownership and locks/IRQ
 * Controller retains ring DMA pages in ring mode or MMIO in immediate mode throughout recovery. Short lock sections; IRQ may progress between bounded phases.
 */
int hda_controller_recover(struct hda_controller *c);

/** @brief Publish a card generation token for later disconnect notification.
 * @param c Initialized controller.
 * @param card Nonzero audio card token returned by audio_register_card.
 * @return 0 or -EINVAL. Task context; no ownership transfer.
 *
 * @par Context
 * Task context after card registration.
 * @par Ownership and locks/IRQ
 * Card token remains core-owned; controller stores a copy under the short state lock.
 */
int hda_controller_set_card_id(struct hda_controller *c, uint32_t card);

/** @brief Consume a repeated-failure disconnect request for the controller.
 * @param c Initialized controller.
 * @param out_card Receives a representative card token after transport faults.
 * @return 0 with token or -EAGAIN when no disconnect was requested. IRQ/task safe.
 *
 * @par Context
 * IRQ or task context; consumer queues teardown and never unregisters in IRQ.
 * @par Ownership and locks/IRQ
 * Returns a copied token while the core retains card ownership; short lock and no IRQ callback/unregister.
 */
int hda_controller_take_disconnect_request(struct hda_controller *c,
                                           uint32_t *out_card);

/** @brief Convert one owned physical page into its checked CPU direct-map alias.
 * @param phys Page-aligned physical address of a page already owned by the caller.
 * @return CPU alias, or NULL for misalignment or a page outside the direct-map window.
 * No mapping or ownership is created by this helper.
 *
 * @par Context
 * Task context while the caller's page lease is live.
 * @par Ownership and locks/IRQ
 * Caller retains allocation and exact free responsibility; no lock, wait, or IRQ action.
 */
void *hda_phys_to_direct_map_page(uint64_t phys);

/** @brief Discover one codec's nodes, effective capabilities, and live routes.
 * @param c Initialized controller used for serialized task-context verbs.
 * @param cad Codec address in the hardware-valid range 0..14 and present in STATESTS.
 * @param codec Caller-owned zero-initialized output cache.
 * @return 0, -EINVAL for invalid topology/CAD, -ENOMEM for CPU metadata failure,
 * or the original transport error. Failure frees every acquired metadata run.
 *
 * @par Context
 * Task context outside global execution ownership; the controller and API outlive the cache.
 * @par Ownership and locks/IRQ
 * On success codec owns exact CPU-only alloc_pages runs until hda_codec_destroy; no DMA mapping is made. No lock is held across verbs.
 */
int hda_codec_probe(struct hda_controller *c, uint8_t cad,
                    struct hda_codec *codec);

/** @brief Release every CPU-only metadata run owned by a codec cache.
 * @param codec Probed or partially initialized cache; active routes must first be restored.
 * @return 0 after exact reclamation, -EBUSY while a route transaction, active
 * route, or discovered route group still references codec metadata.
 *
 * @par Context
 * Task teardown after route users have stopped and relinquished their paths.
 * @par Ownership and locks/IRQ
 * Releases only alloc_pages runs owned by codec; controller/MMIO/DMA resources remain controller-owned.
 */
int hda_codec_destroy(struct hda_codec *codec);

/** @brief Resolve a pin to a converter through its same-AFG connection graph.
 * @param codec Successfully probed immutable codec metadata.
 * @param pin Pin Complex NID requested by the caller.
 * @param direction Playback follows pin-to-DAC edges; capture follows ADC-to-pin edges.
 * @param route Zero-initialized caller-owned route result and future control snapshot storage.
 * @return 0 with a path, -ENODEV when the requested direction has no real route,
 * -EBUSY when this route object already owns an active snapshot, or -EINVAL for invalid arguments or an unsupported pin direction.
 *
 * @par Context
 * Task context; no verbs, waits, allocations, or mutation of the codec cache.
 * @par Ownership and locks/IRQ
 * route borrows codec and owns only its inline path/snapshot storage. Caller zero-initializes it before first use, serializes route-object use and codec teardown, and keeps codec alive.
 */
int hda_find_route(struct hda_codec *codec, uint8_t pin,
                   enum hda_route_direction direction,
                   struct hda_route *route);

/** @brief Apply a resolved analog path or restore its exact prior controls.
 * @param codec Probed codec cache whose route transaction gate is acquired.
 * @param route Route returned by hda_find_route for this codec.
 * @param enable True to snapshot and apply; false to restore a prior application.
 * @return 0 on success, the first transport/readback error, -EBUSY for a concurrent
 * route transaction, or -EINVAL for a stale/mismatched route. A failed rollback
 * leaves route active with rollback_failed set so the caller can retry restoration.
 *
 * @par Context
 * Task context outside global execution ownership; no synchronous verb is issued in IRQ/service context.
 * @par Ownership and locks/IRQ
 * A bounded atomic logical gate serializes route API transactions but is never held as a spinlock across verbs. hda_exec_verb still serializes only one command, so the caller must serialize any other direct codec-control sequence on this codec against route apply/restore. Shared AFG power is leased until the last owner restores it; overlapping signal-node routes are rejected with -EBUSY before route writes. The caller owns route storage, stops PCM before apply/restore, and stops all route searches/users and destroys every group before codec teardown.
 */
int hda_apply_route(struct hda_codec *codec, struct hda_route *route,
                    bool enable);

/** @brief Discover one feasible analog playback association for a seed pin.
 * @param codec Probed immutable codec with live CPU-page allocation services.
 * @param pin Analog output pin whose association is requested.
 * @param group Zero-initialized caller-owned group metadata output.
 * @return 0 with sorted members, -ENODEV for association zero/no feasible
 * analog route, -EINVAL for duplicate sequences or malformed topology,
 * -EOVERFLOW when the member/channel bound is exceeded, or allocation/error.
 * Associations 1..14 include every eligible same-AFG output pin; association
 * 15 is an independent single-pin endpoint.
 *
 * @par Context
 * Task context during probe/stream preparation; graph search performs no verbs.
 * @par Ownership and locks/IRQ
 * Members are CPU-only exact alloc_pages metadata, not DMA memory. The group
 * borrows codec/controller and must be destroyed while codec is alive. The
 * function acquires only the bounded logical route gate and does not wait.
 */
int hda_find_route_group(struct hda_codec *codec, uint8_t pin,
                         struct hda_route_group *group);

/** @brief Atomically activate or restore every member of a playback group.
 * @param codec Probed codec owning @p group and its route gate.
 * @param group Live group returned by hda_find_route_group.
 * @param enable True to apply every path; false to restore/retry active paths.
 * @return 0 after the full operation, -EBUSY for an ownership conflict,
 * -EINVAL for stale metadata, or the first transport/readback/rollback error.
 * Failed members retain active snapshots and shared leases for retry.
 *
 * @par Context
 * Task context with the PCM stream stopped for topology changes; never IRQ or
 * H1 global execution ownership context.
 * @par Ownership and locks/IRQ
 * One nonspinning per-codec logical gate spans all member operations; no
 * spinlock spans a verb or readiness sleep. The caller owns group memory and
 * must keep it until every member is restored and group destroy succeeds.
 */
int hda_apply_route_group(struct hda_codec *codec,
                          struct hda_route_group *group, bool enable);

/** @brief Release inactive group member pages and its codec lifetime reference.
 * @param group Group previously returned by hda_find_route_group.
 * @return 0 after exact-page release, -EBUSY while a member remains active, or
 * -EINVAL for invalid ownership state.
 *
 * @par Context
 * Task teardown after every group route has been stopped/restored.
 * @par Ownership and locks/IRQ
 * Frees only group-owned CPU metadata pages and keeps the codec lifetime
 * reference on failure. The caller must not access members after success.
 */
int hda_route_group_destroy(struct hda_route_group *group);

/** @brief Bind or release one complete HDA stream format on every group converter.
 * @param group Active H3 playback group whose codec gate and power lease are held.
 * @param tag Controller stream tag, or zero to release the converter binding.
 * @param format Complete stream format including CHAN, or zero on release.
 * @return 0 after checked codec writes/readbacks, or a transport/capability errno.
 * Task context only; the route gate is held internally and no IRQ path may call it.
 */
int hda_route_group_stream_bind(struct hda_route_group *group, uint8_t tag,
                                uint16_t format);

/** @brief Enter the codec transaction gate for mixer control changes.
 * @param codec Codec cache whose route/control state is serialized.
 * @return 0 when acquired, -EINVAL for NULL, or -EBUSY when already owned.
 * @par Context Task context only; no IRQ or service callback may wait.
 * @par Ownership and locks/IRQ Caller owns the gate until the matching leave;
 * no hardware lock spans synchronous verbs.
 */
int hda_codec_control_gate_enter(struct hda_codec *codec);

/** @brief Leave a previously acquired codec control gate.
 * @param codec Codec cache whose gate is owned by the caller.
 * @return None; NULL is ignored.
 * @par Context Task context after the control transaction.
 * @par Ownership and locks/IRQ No hardware access, wait, allocation, or IRQ change.
 */
void hda_codec_control_gate_leave(struct hda_codec *codec);

/** @brief Read one amplifier channel for task-context controls.
 * @param codec Live codec cache.
 * @param nid Amplifier widget NID.
 * @param input Select input amp when true.
 * @param index Input amp index, or zero for output.
 * @param right Select right channel.
 * @param out_value Receives raw gain/mute bits.
 * @return 0 or a validation/transport error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ No lock spans the verb; caller owns output/codec lifetime.
 */
int hda_codec_control_read_amp(struct hda_codec *codec, uint8_t nid, bool input,
                               uint8_t index, bool right, uint16_t *out_value);

/** @brief Write one amplifier channel and require hardware readback.
 * @param codec Live codec cache.
 * @param nid Amplifier widget NID.
 * @param input Select input amp when true.
 * @param index Input amp index, or zero for output.
 * @param right Select right channel.
 * @param value Raw gain/mute bits.
 * @param amp_caps Effective capability word.
 * @return 0 after readback, or the original validation/transport error.
 * @par Context Task context while the caller owns the codec control gate.
 * @par Ownership and locks/IRQ No lock spans the verb; IRQ progress remains enabled.
 */
int hda_codec_control_write_amp(struct hda_codec *codec, uint8_t nid, bool input,
                                uint8_t index, bool right, uint16_t value,
                                uint32_t amp_caps);

/** @brief Exact amplifier gain conversion in hundredths of a dB. */
int32_t hda_amp_db(uint32_t caps, uint8_t gain);

/** @brief Initialize HDA mixer/jack metadata from real routes and capabilities.
 * @param controls Caller-owned control state.
 * @param codec Live codec cache borrowed for the control lifetime.
 * @param speaker_route Resolved playback route, or NULL with speaker_pin.
 * @param headphone_route Optional resolved headphone route.
 * @param speaker_pin Playback output pin NID used by the control amp.
 * @param headphone_pin Optional jack pin NID.
 * @param capture_route Optional capture route backing the source selector.
 * @param capture_source_count Number of real capture sources, clamped to one.
 * @param auto_mute Initial jack policy.
 * @return 0 after amplifier readback or presence-sensor setup without an amp;
 * otherwise a validation/transport error. Missing amplifiers publish no gain/mute.
 * @par Context Task context during card publication; no core callback lock held.
 * @par Ownership and locks/IRQ Caller retains routes/codec; init takes the
 * codec logical gate only around synchronous amplifier verbs.
 */
int hda_controls_init(struct hda_controls *controls, struct hda_codec *codec,
                      struct hda_route *speaker_route,
                      struct hda_route *headphone_route, uint8_t speaker_pin,
                      uint8_t headphone_pin, struct hda_route *capture_route,
                      uint8_t capture_source_count, bool auto_mute);

/** @brief Destroy a control object without releasing borrowed codec/routes.
 * @param controls Control state to clear, or NULL.
 * @return None; no hardware command is issued.
 * @par Context Task context after card service cancellation and route stop.
 * @par Ownership and locks/IRQ Caller must have stopped users; no lock or IRQ
 * operation occurs and borrowed route ownership remains with the caller.
 */
void hda_controls_destroy(struct hda_controls *controls);

/** @brief Return the stable internal HDA element ID range.
 * @param controls Initialized control state.
 * @return Five internal IDs for a live object, otherwise zero. The module
 * freezes a separate capability-filtered registration set and index mapping.
 * @par Context Any task-context card metadata query; no hardware access.
 * @par Ownership and locks/IRQ Read-only scalar access with no lock or IRQ work.
 */
uint32_t hda_controls_count(const struct hda_controls *controls);

/** @brief Describe one real mixer, source, jack, or policy control.
 * @param controls Initialized control state.
 * @param control Registration-order control identifier.
 * @param info Receives copied type, range, access, and dB metadata.
 * @return 0 or -EINVAL for a stale identifier/argument.
 * @par Context Task context outside the audio-core registry lock.
 * @par Ownership and locks/IRQ Caller owns output storage; no hardware access,
 * allocation, lock, or IRQ operation occurs.
 */
int hda_controls_info(const struct hda_controls *controls, uint32_t control,
                     struct audio_control_info *info);

/** @brief Read a control from hardware or bounded policy state.
 * @param controls Initialized control state.
 * @param control Registration-order control identifier.
 * @param value Receives one or two values according to the control metadata.
 * @return 0 or a validation/transport error; mute-less hardware reads zero,
 * absent amplifier reads -ENODEV, and state contention returns -EBUSY.
 * @par Context Task context without the core registry lock.
 * @par Ownership and locks/IRQ Nonwaiting state and codec gates are internal;
 * no spinlock/core lock spans verbs. Caller owns the output buffer.
 */
int hda_controls_read(struct hda_controls *controls, uint32_t control,
                      struct audio_control_value *value);

/** @brief Atomically write a gain, mute, source, or auto-mute policy control.
 * @param controls Initialized control state.
 * @param control Registration-order control identifier.
 * @param value Caller-owned values already bounded by the card contract.
 * @return 0 after hardware readback, or the original error with rollback done.
 * @par Context Task context only; synchronous verbs are forbidden in IRQ/service.
 * @par Ownership and locks/IRQ A nonwaiting state gate returns -EBUSY on
 * concurrent control/service work. The codec logical gate also serializes
 * route/amp transactions; no spinlock spans verbs. Rollback retains the prior value.
 */
int hda_controls_write(struct hda_controls *controls, uint32_t control,
                       const struct audio_control_value *value);

/** @brief Consume a bounded jack event/poll budget without synchronous waits.
 * @param opaque Borrowed struct hda_controls passed through the card service.
 * @param budget Shared maximum of transport entries, own-CAD events and sense progress.
 * @return Nonblocking work units consumed, bounded by budget.
 * @par Context Audio-core service/tick or task context; never waits for a verb.
 * @par Ownership and locks/IRQ Controller owns the unsolicited queue and ticket;
 * this function only submits/polls one flight and does not sleep or allocate.
 * A busy state gate returns zero and leaves pending sense work intact.
 */
uint32_t hda_controls_service(void *opaque, uint32_t budget);

/** @brief Progress jack sensing and one deferred policy transaction in a task.
 * @param opaque Borrowed live controls, pinned with their route/stream owner.
 * @param budget Shared transport/event/sense/policy work limit.
 * @return Work consumed, at most budget; contention returns zero with work queued.
 * @par Context Waitable task only, outside execution and IRQ ownership.
 * @par Ownership Caller holds any enclosing card gate. No spinlock spans verbs;
 * failed policy remains pending and records its errno in last_error.
 */
uint32_t hda_controls_task_service(void *opaque, uint32_t budget);

/** @brief Run service then apply pending task-context auto-mute routing.
 * @param controls Borrowed live control state.
 * @param budget Nonblocking service budget.
 * @return 0 or the route/amp error from the deferred policy operation. Zero
 * budget preserves work; policy failures remain pending for a later retry.
 * @par Context Task context after the bounded service pass; synchronous verbs
 * are allowed only for this worker phase.
 * @par Ownership and locks/IRQ Caller keeps codec/routes alive; the codec gate
 * is held only around direct amp changes and never across a core lock. The
 * nonwaiting state gate spans service/policy and returns -EBUSY on contention.
 */
int hda_controls_worker(struct hda_controls *controls, uint32_t budget);

/** @brief Deduplicate one raw HDA pin-sense response and publish a jack change.
 * @param controls Borrowed control state.
 * @param sense Raw GET_PIN_SENSE response including the presence bit.
 * @return 1 for a state change, 0 for a duplicate, -EINVAL for NULL, or -EBUSY.
 * @par Context Nonblocking service or task context; no synchronous verb occurs.
 * @par Ownership and locks/IRQ State is scalar control-owned data; route work is
 * deferred to the worker. The bounded state gate does not wait or change IF.
 */
int hda_controls_process_jack_sense(struct hda_controls *controls,
                                    uint32_t sense);

/** @brief Adapt control count to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @return Stable control count, or zero for NULL.
 * @par Context Task context card metadata query; no hardware access.
 * @par Ownership and locks/IRQ No ownership transfer, lock, allocation, or IRQ work.
 */
uint32_t hda_controls_card_count(void *opaque);

/** @brief Adapt control metadata to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Registration-order identifier.
 * @param info Receives copied metadata.
 * @return 0 or the same validation error as hda_controls_info.
 * @par Context Task context outside the core registry lock.
 * @par Ownership and locks/IRQ No ownership transfer or hardware access.
 */
int hda_controls_card_info(void *opaque, uint32_t control,
                           struct audio_control_info *info);

/** @brief Adapt control reads to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Registration-order identifier.
 * @param value Receives the bounded control value.
 * @return 0 or the same validation/transport error as hda_controls_read.
 * @par Context Task context without the core registry lock.
 * @par Ownership and locks/IRQ Codec gate ownership remains internal to the control read.
 */
int hda_controls_card_read(void *opaque, uint32_t control,
                           struct audio_control_value *value);

/** @brief Adapt control writes to the audio core opaque callback ABI.
 * @param opaque Borrowed struct hda_controls.
 * @param control Registration-order identifier.
 * @param value Caller-owned bounded control value.
 * @return 0 or the same validation/rollback error as hda_controls_write.
 * @par Context Task context without the core registry lock.
 * @par Ownership and locks/IRQ Synchronous codec work remains inside the control transaction.
 */
int hda_controls_card_write(void *opaque, uint32_t control,
                            const struct audio_control_value *value);

/** @brief Emit codec identity and discovered node capabilities without addresses.
 * @param codec Successfully probed immutable metadata.
 * @return None; emits only numeric codec/NID/capability/default/connection data.
 *
 * @par Context
 * Task diagnostics while codec metadata remains alive.
 * @par Ownership and locks/IRQ
 * Reads immutable CPU metadata and borrows api->console_write; does not expose pointers, lock, or issue verbs.
 */
void hda_codec_dump(const struct hda_codec *codec);

#endif
