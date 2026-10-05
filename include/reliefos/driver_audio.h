#ifndef RELIEFOS_DRIVER_AUDIO_H
#define RELIEFOS_DRIVER_AUDIO_H
#include <stdint.h>
/* Ring-0 module contract; never export these pointer-bearing types as UAPI. */
#define AUDIO_CARD_OPS_VERSION 1U
/* Formats are local mask bits, not Linux format numbers. S20/S24_LE use
 * signed samples in 32-bit little-endian containers; S8/U8 use one byte.
 * sample_bits is precision, frame_bytes describes channels times container. */
#define AUDIO_FORMAT_S8      (1ULL << 0)
#define AUDIO_FORMAT_S16_LE  (1ULL << 1)
#define AUDIO_FORMAT_S20_LE  (1ULL << 2)
#define AUDIO_FORMAT_S24_LE  (1ULL << 3)
#define AUDIO_FORMAT_S32_LE  (1ULL << 4)
#define AUDIO_FORMAT_U8      (1ULL << 5)
#define AUDIO_RATE_8000     (1ULL << 0)
#define AUDIO_RATE_11025    (1ULL << 1)
#define AUDIO_RATE_16000    (1ULL << 2)
#define AUDIO_RATE_22050    (1ULL << 3)
#define AUDIO_RATE_32000    (1ULL << 4)
#define AUDIO_RATE_44100    (1ULL << 5)
#define AUDIO_RATE_48000    (1ULL << 6)
#define AUDIO_RATE_88200    (1ULL << 7)
#define AUDIO_RATE_96000    (1ULL << 8)
#define AUDIO_RATE_176400   (1ULL << 9)
#define AUDIO_RATE_192000   (1ULL << 10)
#define AUDIO_CONTROL_BOOLEAN 1U
#define AUDIO_CONTROL_INTEGER 2U
#define AUDIO_CONTROL_ENUMERATED 3U
#define AUDIO_CONTROL_READ 1U
#define AUDIO_CONTROL_WRITE 2U
#define AUDIO_CONTROL_VOLATILE 4U
#define AUDIO_DEFAULT_PERIOD_FRAMES 512U
#define AUDIO_DEFAULT_BUFFER_FRAMES 2048U
/* Core owns DMA. open/close transfer hw stream ownership; all callbacks borrow
 * opaque/buffers only during invocation. prepare/control may wait only in task
 * context without the execution lock. trigger STOP must synchronously quiesce
 * DMA. pointer/trigger and IRQ service callbacks never sleep or wait for verbs.
 * The optional task_service callback may wait with execution ownership released.
 * Core never holds its registry lock across a module callback. */
enum audio_direction { AUDIO_PLAYBACK, AUDIO_CAPTURE };
enum audio_trigger { AUDIO_START, AUDIO_STOP, AUDIO_PAUSE, AUDIO_UNPAUSE };
struct audio_caps {
    uint64_t formats, rates;
    uint32_t channels_min, channels_max;
    uint32_t period_bytes_min, period_bytes_max, buffer_bytes_max;
};
enum audio_subformat {
    AUDIO_SUBFORMAT_STD = 0,
    AUDIO_SUBFORMAT_MSBITS_20 = 1,
    AUDIO_SUBFORMAT_MSBITS_24 = 2,
    AUDIO_SUBFORMAT_MSBITS_MAX = 3,
};
struct audio_format_caps {
    struct audio_caps pcm;
    uint32_t format_step_bytes;
    uint32_t period_count_min, period_count_max;
    uint32_t format_bits[6];
    uint32_t subformats[6];
};
struct audio_params {
    uint32_t rate, channels, sample_bits, frame_bytes;
    uint32_t period_frames, buffer_frames;
};
struct audio_params_ext {
    struct audio_params pcm;
    uint32_t format;
    uint32_t subformat;
    uint32_t significant_bits;
};
struct audio_dma { void *kernel; uint64_t bus; uint32_t bytes; };
struct audio_hw_stream { uint32_t id; void *driver; };
struct audio_control_info {
    uint32_t id, type, count, access;
    int64_t min, max, step;
    int32_t db_min, db_step;
    char name[44];
    uint32_t items;
    char item_names[16][32];
};
struct audio_control_value { int64_t values[16]; };
struct audio_card_ops {
    uint32_t version, size;
    /** @brief Query hardware constraints; task context, no core lock.
     * @param opaque Borrowed live module context.
     * @param device PCM device index.
     * @param direction Playback/capture selector.
     * @param caps Receives format/rate masks and byte constraints.
     * @return 0 or negative errno; no ownership transfer.
     */
    int (*pcm_caps)(void *opaque, uint32_t device, enum audio_direction direction, struct audio_caps *caps);
    /** @brief Acquire a hardware stream; task context, no core lock.
     * @param opaque Borrowed module context.
     * @param device PCM device index.
     * @param direction Playback/capture selector.
     * @param stream Receives owned hardware lease until close.
     * @return 0 or negative errno.
     */
    int (*open)(void *opaque, uint32_t device, enum audio_direction direction, struct audio_hw_stream *stream);
    /** @brief Configure a stopped stream; task context without execution lock.
     * @param opaque Borrowed module context.
     * @param stream Borrowed open hardware lease.
     * @param params Borrowed validated hardware parameters.
     * @param dma Borrowed core-owned pinned DMA; driver must not free.
     * @return 0 or negative errno.
     */
    int (*prepare)(void *opaque, struct audio_hw_stream *stream, const struct audio_params *params,
                   const struct audio_dma *dma);
    /** @brief Control hardware; IRQ safe, no sleep or codec response wait.
     * @param opaque Borrowed module context.
     * @param stream Borrowed hardware lease.
     * @param trigger START/STOP/PAUSE/UNPAUSE. STOP synchronously stops DMA.
     * @return 0 or negative errno; STOP failure forbids image/buffer release.
     */
    int (*trigger)(void *opaque, struct audio_hw_stream *stream, enum audio_trigger trigger);
    /** @brief Read monotonic frame count; IRQ safe, no waiting.
     * @param opaque Borrowed module context.
     * @param stream Borrowed hardware lease.
     * @param frames Receives completed hardware frames.
     * @return 0 or negative errno.
     */
    int (*pointer)(void *opaque, struct audio_hw_stream *stream, uint64_t *frames);
    /** @brief Release hardware stream after successful STOP; task context.
     * @param opaque Borrowed module context.
     * @param stream Owned hardware lease consumed by callback.
     * @return None; core-owned DMA remains pinned until fd/VMA release.
     */
    void (*close)(void *opaque, struct audio_hw_stream *stream);
    /** @brief Read fixed control-set size; task context, no core lock.
     * @param opaque Borrowed module context.
     * @return Number of actual controls, fixed after card publication.
     */
    uint32_t (*control_count)(void *opaque);
    /** @brief Describe a real control; task context, no core lock.
     * @param opaque Borrowed module context.
     * @param control Control index.
     * @param info Receives copied metadata, at most 16 enumerated items.
     * @return 0 or negative errno; no ownership transfer.
     */
    int (*control_info)(void *opaque, uint32_t control, struct audio_control_info *info);
    /** @brief Read a real control; task context without execution lock.
     * @param opaque Borrowed module context.
     * @param control Control index.
     * @param value Receives values, bounded by info.count <= 16.
     * @return 0 or negative errno.
     */
    int (*control_read)(void *opaque, uint32_t control, struct audio_control_value *value);
    /** @brief Write a real control; task context without execution lock.
     * @param opaque Borrowed module context.
     * @param control Control index.
     * @param value Borrowed validated values, at most 16.
     * @return 0 or negative errno.
     */
    int (*control_write)(void *opaque, uint32_t control, const struct audio_control_value *value);
    /** @brief Query selected memory formats and DMA geometry; task context.
     * @param opaque Borrowed live module context.
     * @param device PCM device index.
     * @param direction Playback/capture selector.
     * @param caps Receives bounded format/subformat capabilities.
     * @return 0 or negative errno; no ownership transfer.
     */
    int (*pcm_format_caps)(void *opaque, uint32_t device,
                           enum audio_direction direction,
                           struct audio_format_caps *caps);
    /** @brief Configure an explicit memory format on a stopped stream.
     * @param opaque Borrowed live module context.
     * @param stream Borrowed open hardware lease.
     * @param params Borrowed selected format and exact significant precision.
     * @param dma Borrowed core-owned pinned DMA buffer.
     * @return 0 or negative errno; task context without execution lock.
     */
    int (*prepare_format)(void *opaque, struct audio_hw_stream *stream,
                          const struct audio_params_ext *params,
                          const struct audio_dma *dma);
    /** @brief Process coalesced tick work in task context; optional ABI suffix.
     * @param opaque Borrowed module context, pinned until the callback returns.
     * @param budget Remaining global work budget; callback must honor this bound.
     * @return Work consumed, no greater than budget. May wait for codec replies
     * without execution ownership or the core registry lock. IRQ service may
     * run concurrently; the callback must serialize its own hardware state.
     */
    uint32_t (*task_service)(void *opaque, uint32_t budget);
};
struct audio_card_identity {
    uint8_t bus, slot, function, codec;
    uint16_t vendor, device;
    uint32_t codec_id, subsystem_id;
    char id[16], name[80];
};
/** @brief Nonblocking IRQ/tick service: consume at most budget completions.
 * @param opaque Borrowed module context, alive until service synchronization.
 * @param budget Remaining global completion budget (at most 32).
 * @return Actual work consumed, no greater than budget. No sleep/verb waits.
 */
typedef uint32_t (*audio_service_fn)(void *opaque, uint32_t budget);
#endif
