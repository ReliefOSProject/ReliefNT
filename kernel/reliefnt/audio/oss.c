#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/soundcard.h>
#include <stdint.h>

#include <reliefnt/audio.h>
#include <reliefnt/futex.h>
#include <reliefnt/driver_manager.h>
#include <reliefnt/sched.h>
#include <reliefnt/usercopy.h>

#define AUDIO_OSS_FILE_MAX 64U
#define AUDIO_OSS_DEFAULT_RATE 48000U
#define AUDIO_OSS_DEFAULT_CHANNELS 2U
#define AUDIO_OSS_DEFAULT_FORMAT AFMT_S16_LE
#define AUDIO_OSS_DEFAULT_FRAGMENT 2048U
#define AUDIO_OSS_DEFAULT_FRAGMENTS 8U
#define AUDIO_OSS_MIN_FRAGMENT 256U
#define AUDIO_OSS_MAX_FRAGMENT 65536U
#define AUDIO_OSS_MAX_FRAGMENTS 64U
#define AUDIO_OSS_FRAME_MAX 32U

struct audio_oss_file {
    uint32_t used;
    uint32_t card;
    uint32_t legacy_generation;
    uint32_t devices[2];
    uint32_t rate;
    uint32_t channels;
    uint32_t format;
    uint32_t fragment_bytes;
    uint32_t fragment_count;
    uint32_t queued_bytes;
    uint64_t written_bytes;
    uint32_t blocks;
    uint32_t trigger;
    uint32_t duplex;
    uint32_t tail_bytes;
    uint8_t tail[AUDIO_OSS_FRAME_MAX];
    struct audio_pcm *playback_pcm;
    struct audio_pcm *capture_pcm;
    uint32_t capture_tail_bytes;
    uint8_t capture_tail[AUDIO_OSS_FRAME_MAX];
    uint64_t read_bytes;
    uint32_t read_blocks;
    uint32_t direction_flags;
    uint64_t last_output_hw_frames;
    uint64_t last_input_hw_frames;
};

static struct audio_oss_file audio_oss_files[AUDIO_OSS_FILE_MAX];

/** @brief Return bytes per OSS sample for one supported format. */
static uint32_t audio_oss_sample_bytes(uint32_t format)
{
    return format == AFMT_S16_LE ? 2U : 1U;
}

/** @brief Translate an OSS sample format into the core capability bit. */
static uint64_t audio_oss_format_bit(uint32_t format)
{
    switch (format) {
    case AFMT_S8: return AUDIO_FORMAT_S8;
    case AFMT_S16_LE: return AUDIO_FORMAT_S16_LE;
    case AFMT_U8: return AUDIO_FORMAT_U8;
    default: return 0;
    }
}

/** @brief Find the first usable hardware PCM for a description's bound card.
 * @param state Borrowed OSS state. @param direction Requested PCM direction.
 * @param caps Receives actual caps with unsupported legacy U8 removed.
 * @return Zero or -ENODEV; task context, pinned callbacks, no stream ownership.
 */
static int audio_oss_caps(struct audio_oss_file *state, enum audio_direction direction,
                          struct audio_caps *caps)
{
    uint32_t card = state->card;
    if (!card && audio_card_snapshot(0, &card, NULL) < 0) return -ENODEV;
    for (uint32_t device = 0; device < 8; ++device) {
        if (audio_card_pcm_caps(card, device, direction, caps) < 0) continue;
        struct audio_format_caps format;
        int ret = audio_stream_format_caps(card, device, direction, &format);
        if (!ret) caps->formats &= format.pcm.formats;
        else if (ret == -EOPNOTSUPP) caps->formats &= ~AUDIO_FORMAT_U8;
        else return ret;
        state->card = card;
        state->devices[direction] = device;
        return 0;
    }
    return -ENODEV;
}

/** @brief Select the active direction's hardware constraints for OSS negotiation.
 * @param state Borrowed description. @param caps Caller-owned output.
 * @return Zero or hardware errno; no format conversion is advertised.
 */
static int audio_oss_active_caps(struct audio_oss_file *state, struct audio_caps *caps)
{
    int ret = audio_oss_caps(state, state->direction_flags & PCM_ENABLE_OUTPUT
        ? AUDIO_PLAYBACK : AUDIO_CAPTURE, caps);
    if (ret || state->direction_flags != (PCM_ENABLE_OUTPUT | PCM_ENABLE_INPUT)) return ret;
    struct audio_caps capture;
    ret = audio_oss_caps(state, AUDIO_CAPTURE, &capture);
    if (ret) return ret;
    caps->formats &= capture.formats;
    caps->rates &= capture.rates;
    if (caps->channels_min < capture.channels_min) caps->channels_min = capture.channels_min;
    if (caps->channels_max > capture.channels_max) caps->channels_max = capture.channels_max;
    if (caps->period_bytes_min < capture.period_bytes_min) caps->period_bytes_min = capture.period_bytes_min;
    if (capture.period_bytes_max && caps->period_bytes_max > capture.period_bytes_max)
        caps->period_bytes_max = capture.period_bytes_max;
    if (capture.buffer_bytes_max && caps->buffer_bytes_max > capture.buffer_bytes_max)
        caps->buffer_bytes_max = capture.buffer_bytes_max;
    return caps->formats && caps->rates && caps->channels_min <= caps->channels_max ? 0 : -EINVAL;
}

/** @brief Choose the nearest advertised discrete hardware sample rate.
 * @param requested Positive rate. @param mask Hardware rate bits.
 * @return Supported rate, or zero for empty capabilities; no state mutation.
 */
static uint32_t audio_oss_nearest_rate(uint32_t requested, uint64_t mask)
{
    static const uint32_t rates[] = {8000, 11025, 16000, 22050, 32000, 44100,
        48000, 88200, 96000, 176400, 192000};
    uint32_t best = 0, distance = UINT32_MAX;
    for (uint32_t i = 0; i < sizeof(rates) / sizeof(rates[0]); ++i) {
        if (!(mask & (1ULL << i))) continue;
        uint32_t delta = rates[i] > requested ? rates[i] - requested : requested - rates[i];
        if (delta < distance) { distance = delta; best = rates[i]; }
    }
    return best;
}

/** @brief Return the formats exposed by the core-backed OSS description. */
static uint64_t audio_oss_core_formats(struct audio_oss_file *state)
{
    struct audio_caps caps;
    if (!state || audio_oss_active_caps(state, &caps) < 0)
        return 0;
    return caps.formats;
}

/** @brief Convert core format capability bits into OSS format bits. */
static int audio_oss_core_oss_formats(struct audio_oss_file *state)
{
    if (state && state->legacy_generation) return AFMT_S16_LE;
    uint64_t formats = audio_oss_core_formats(state);
    int result = 0;
    if (formats & AUDIO_FORMAT_S8) result |= AFMT_S8;
    if (formats & AUDIO_FORMAT_S16_LE) result |= AFMT_S16_LE;
    if (formats & AUDIO_FORMAT_U8) result |= AFMT_U8;
    return result;
}

/** @brief Return the queue of the exact leased legacy backend.
 * @param file Borrowed OSS description. @return Real queued bytes or zero on absence.
 * Task context; the manager pins the generation through hardware get_state.
 */
static uint32_t audio_oss_driver_queue(const struct audio_oss_file *file)
{
    struct reliefos_audio_state state = {0};
    return file && file->legacy_generation &&
        !driver_manager_audio_state_bound(file->legacy_generation,&state) ? state.queued_bytes : 0U;
}

/** @brief Return the configured software queue capacity. */
static uint32_t audio_oss_capacity(const struct audio_oss_file *file)
{
    if (!file) return 0;
    uint64_t capacity = (uint64_t)file->fragment_bytes * file->fragment_count;
    return capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
}

/** @brief Open and prepare one core PCM direction for the OSS byte stream.
 * Playback is deliberately left PREPARED until one period is queued; a
 * frame-at-a-time OSS writer otherwise starts an empty HDA ring and can
 * underrun before the first syscall has filled it. Capture retains its
 * historical immediate-start behavior.
 */
static int audio_oss_pcm_prepare(struct audio_oss_file *state,
                                 enum audio_direction direction)
{
    if (!state || !state->used) return -EBADF;
    struct audio_pcm **slot = direction == AUDIO_PLAYBACK
        ? &state->playback_pcm : &state->capture_pcm;
    if (*slot) {
        struct audio_pcm_status status;
        int ret = audio_pcm_status(*slot, &status);
        if (ret < 0) return ret;
        if (status.state == AUDIO_PCM_DISCONNECTED) return -ENODEV;
        if (status.state != AUDIO_PCM_OPEN && status.state != AUDIO_PCM_SETUP) return 0;
    }
    struct audio_caps caps;
    int ret = audio_oss_caps(state, direction, &caps);
    if (ret < 0) return ret;
    int error = 0;
    struct audio_pcm *pcm = *slot ? *slot :
        audio_pcm_open(state->card, state->devices[direction], direction, &error);
    if (!pcm) return error;
    struct audio_params params = {
        .rate = state->rate,
        .channels = state->channels,
        .sample_bits = audio_oss_sample_bytes(state->format) * 8u,
        .frame_bytes = audio_oss_sample_bytes(state->format) * state->channels,
        .period_frames = state->fragment_bytes /
            (audio_oss_sample_bytes(state->format) * state->channels),
    };
    /* Negotiate whole-frame fragments, then retain an integral period count. */
    params.buffer_frames = params.period_frames * state->fragment_count;
    if (!params.period_frames || !params.buffer_frames) ret = -EINVAL;
    struct audio_params_ext selected = {.pcm = params,
        .format = (uint32_t)audio_oss_format_bit(state->format),
        .subformat = AUDIO_SUBFORMAT_STD, .significant_bits = params.sample_bits};
    if (!ret) ret = audio_pcm_hw_params_format(pcm, &selected);
    if (!ret) ret = audio_pcm_prepare(pcm);
    if (!ret && direction == AUDIO_CAPTURE &&
        (state->trigger & PCM_ENABLE_INPUT))
        ret = audio_pcm_start(pcm);
    if (ret) {
        if (!*slot) audio_pcm_release(pcm);
        return ret;
    }
    if (direction == AUDIO_PLAYBACK) state->last_output_hw_frames = 0;
    else state->last_input_hw_frames = 0;
    state->fragment_bytes = params.period_frames * params.frame_bytes;
    *slot = pcm;
    return 0;
}

/** @brief Start a prepared OSS playback PCM after its initial period is queued.
 * @param state Borrowed OSS description with a configured playback PCM.
 * @param force Nonzero for explicit SETTRIGGER output enable semantics.
 * @return Zero, a hardware/state errno, or -ENODEV. No-op while running.
 */
static int audio_oss_start_playback(struct audio_oss_file *state, int force)
{
    if (!state || !state->playback_pcm) return 0;
    struct audio_pcm_status status;
    int ret = audio_pcm_status(state->playback_pcm, &status);
    if (ret < 0) return ret;
    if (status.state == AUDIO_PCM_DISCONNECTED) return -ENODEV;
    if (status.state != AUDIO_PCM_PREPARED) return 0;
    uint64_t queued = (status.appl_ptr + status.boundary - status.hw_ptr) %
        status.boundary;
    /* Keep one period of room for the frame-at-a-time writer.  This lets
     * startup prefill absorb scheduler stalls without filling a stopped ring
     * so completely that the writer would wait for an event that cannot occur
     * until the stream is running. */
    uint64_t watermark = status.buffer_frames > status.avail_min
        ? status.buffer_frames - status.avail_min : status.avail_min;
    if (state->fragment_count == AUDIO_OSS_MAX_FRAGMENTS)
        /* Doom explicitly selects the largest batch ring.  Its writer emits
         * whole mixed blocks, so retaining every period of startup slack is
         * more useful than reserving a wakeup slot before the stream runs. */
        watermark = status.buffer_frames;
    if (!force && queued < watermark) return 0;
    return audio_pcm_start(state->playback_pcm);
}

/** @brief Release any core PCM leases owned by one OSS open description. */
static void audio_oss_pcm_release(struct audio_oss_file *state)
{
    if (!state) return;
    if (state->playback_pcm) audio_pcm_release(state->playback_pcm);
    if (state->capture_pcm) audio_pcm_release(state->capture_pcm);
    state->playback_pcm = NULL;
    state->capture_pcm = NULL;
}

/** @brief Return playback queue or capture availability in bytes. */
static uint32_t audio_oss_pcm_bytes(struct audio_pcm *pcm, int capture)
{
    struct audio_pcm_status status;
    if (!pcm || audio_pcm_status(pcm, &status) < 0 || !status.frame_bytes ||
        !status.boundary) return 0;
    if (!capture && (status.error || status.state == AUDIO_PCM_XRUN)) return 0;
    uint64_t distance = capture
        ? (status.hw_ptr + status.boundary - status.appl_ptr) % status.boundary
        : (status.appl_ptr + status.boundary - status.hw_ptr) % status.boundary;
    /* A playback distance beyond the ring means hw_ptr passed appl_ptr.
     * Report empty output space so a polling writer reaches the real EPIPE
     * transfer path; clamping it to a full queue would stop that writer. */
    if (distance > status.buffer_frames)
        distance = capture ? status.buffer_frames : 0;
    uint64_t bytes = distance * status.frame_bytes;
    return bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
}

static uint32_t audio_oss_queue(const struct audio_oss_file *state)
{
    if (state && state->playback_pcm) return audio_oss_pcm_bytes(state->playback_pcm, 0);
    return audio_oss_driver_queue(state);
}

/** @brief Snapshot OSS count_info from a core PCM hardware position.
 * @param pcm Live PCM lease owned by this OSS description.
 * @param last_frames Previous hardware frame position for block deltas.
 * @param info Receives bytes, completed fragment delta, and ring offset.
 * @return Zero or -ENODEV/-EINVAL. No ownership transfer.
 */
static int audio_oss_pcm_count(struct audio_pcm *pcm, uint32_t fragment_bytes,
                               uint64_t *last_frames, count_info *info)
{
    struct audio_pcm_status status;
    if (!pcm || !last_frames || !info || audio_pcm_status(pcm, &status) < 0)
        return -EINVAL;
    if (status.state == AUDIO_PCM_OPEN) { *info = (count_info){0}; return 0; }
    if (!status.frame_bytes || !status.buffer_frames ||
        fragment_bytes < status.frame_bytes)
        return -EINVAL;
    uint64_t period_frames = fragment_bytes / status.frame_bytes;
    uint64_t position = status.hw_ptr % status.boundary;
    uint64_t previous = *last_frames;
    uint64_t advanced = (position + status.boundary - previous) % status.boundary;
    uint32_t blocks = (uint32_t)((previous % period_frames + advanced) / period_frames);
    *last_frames = position;
    *info = (count_info){
        .bytes = (int)(position * status.frame_bytes),
        .blocks = (int)blocks,
        .ptr = (int)((position % status.buffer_frames) * status.frame_bytes),
    };
    return 0;
}

static int audio_oss_backend_ready(const struct audio_oss_file *state)
{
    if (!state) return 0;
    if (state->legacy_generation) {
        struct reliefos_audio_state hardware;
        return !driver_manager_audio_state_bound(state->legacy_generation,&hardware);
    }
    return state->playback_pcm || state->capture_pcm;
}

/** @brief Stop and discard one OSS PCM direction before a reconfiguration.
 * @param pcm Owned PCM lease, or NULL.
 * @return Zero, -ENODEV, or the core stop error. No ownership transfer.
 */
static int audio_oss_drop_pcm(struct audio_pcm *pcm)
{
    return pcm ? audio_pcm_drop(pcm) : 0;
}

/** @brief Invalidate both configured OSS directions before changing geometry.
 * @param state Caller-owned OSS description, serialized in task context.
 * @return Zero or a PCM stop error. Clears byte tails and pointer baselines;
 * core leases remain owned until final description close.
 */
static int audio_oss_reconfigure(struct audio_oss_file *state)
{
    if (state->legacy_generation)
        return audio_oss_driver_queue(state) || state->tail_bytes ? -EBUSY : 0;
    int ret = audio_oss_drop_pcm(state->playback_pcm);
    if (!ret) ret = audio_oss_drop_pcm(state->capture_pcm);
    if (ret) return ret;
    if (state->playback_pcm) ret = audio_pcm_hw_free(state->playback_pcm);
    if (!ret && state->capture_pcm) ret = audio_pcm_hw_free(state->capture_pcm);
    if (ret) return ret;
    state->tail_bytes = state->capture_tail_bytes = 0;
    state->last_output_hw_frames = state->last_input_hw_frames = 0;
    return 0;
}

/** @brief Publish an interruptible wait after an empty/full queue snapshot.
 * @param task Current task with a caller-owned, pinned OSS description.
 * @param epoch Event sequence sampled before querying PCM progress.
 * @return Zero when a racing event requires rechecking, otherwise the parked
 * syscall marker. IRQ notifications cannot be lost between query and sleep.
 */
static int audio_oss_wait_retry(struct task *task, uint32_t epoch)
{
    audio_pcm_wait_current();
    if (audio_pcm_event_epoch() != epoch) {
        audio_pcm_wait_remove(task);
        sched_wake_interruptible(task);
        return 0;
    }
    if (task->state != TASK_BLOCKED) audio_pcm_wait_remove(task);
    return KERNEL_SYSCALL_BLOCKED;
}

/** @brief Drain an OSS playback PCM with epoch-safe interruptible waits.
 * @param task Current task, or NULL for a nonblocking fixture probe.
 * @param file OSS OFD; SYNC waits even when its O_NONBLOCK flag is set.
 * @param pcm Playback PCM lease retained by the caller.
 * @return Zero, EAGAIN, KERNEL_SYSCALL_BLOCKED, or a core errno.
 */
static int audio_oss_drain_wait(struct task *task, struct task_file *file,
                                struct audio_pcm *pcm)
{
    (void)file;
    if (!pcm) return 0;
    if (task) audio_pcm_wait_remove(task);
    for (;;) {
        uint32_t epoch = audio_pcm_event_epoch();
        int ret = audio_pcm_drain(pcm);
        if (ret != -EAGAIN) return ret;
        if (!task || !task->pid) return ret;
        audio_pcm_wait_current();
        if (audio_pcm_event_epoch() != epoch) {
            audio_pcm_wait_remove(task);
            sched_wake_interruptible(task);
            continue;
        }
        if (task->state != TASK_BLOCKED) {
            audio_pcm_wait_remove(task);
            return task->state == TASK_READY ? KERNEL_SYSCALL_BLOCKED : -EAGAIN;
        }
        return KERNEL_SYSCALL_BLOCKED;
    }
}

/** @brief Bind an OSS description to core PCM or one exclusive v1 generation.
 * @param task Current task, unused by admission. @param file Owned description.
 * @return Zero or errno. Task context; v1 permits S16 stereo playback only;
 * core PCM retains its direction-specific leases until final close.
 */
int audio_oss_open(struct task *task, struct task_file *file)
{
    (void)task;
    if (!file || file->audio_oss_file) return -EBADF;
    for (uint32_t i = 0; i < AUDIO_OSS_FILE_MAX; ++i) {
        if (audio_oss_files[i].used) continue;
        audio_oss_files[i] = (struct audio_oss_file){
            .used = 1,
            .rate = AUDIO_OSS_DEFAULT_RATE,
            .channels = AUDIO_OSS_DEFAULT_CHANNELS,
            .format = AUDIO_OSS_DEFAULT_FORMAT,
            .fragment_bytes = AUDIO_OSS_DEFAULT_FRAGMENT,
            .fragment_count = AUDIO_OSS_DEFAULT_FRAGMENTS,
            .trigger = (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY
                ? PCM_ENABLE_INPUT
                : (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY
                    ? PCM_ENABLE_OUTPUT
                    : PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT,
            .direction_flags = (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDONLY
                ? PCM_ENABLE_INPUT
                : (file->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_WRONLY
                    ? PCM_ENABLE_OUTPUT
                    : PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT,
        };
        file->audio_oss_file = &audio_oss_files[i];
        struct audio_oss_file *state = file->audio_oss_file;
        struct reliefos_audio_state hardware;
        int legacy=driver_manager_audio_acquire(&state->legacy_generation,&hardware);
        if (!legacy && (state->direction_flags != PCM_ENABLE_OUTPUT ||
            hardware.channels != 2 || hardware.bits_per_sample != 16)) {
            driver_manager_audio_release(state->legacy_generation);
            *state=(struct audio_oss_file){0};file->audio_oss_file=NULL;return -ENODEV;
        }
        if (legacy && legacy != -ENODEV) {
            *state=(struct audio_oss_file){0};file->audio_oss_file=NULL;return legacy;
        }
        if (!legacy) state->rate=hardware.sample_rate;
        if (!state->legacy_generation) {
            uint32_t wanted = state->direction_flags;
            uint32_t available = 0;
            uint64_t formats = UINT64_MAX, rates = UINT64_MAX;
            uint32_t minimum = 1, maximum = 128;
            int error = 0;
            for (enum audio_direction direction = AUDIO_PLAYBACK; direction <= AUDIO_CAPTURE; ++direction) {
                uint32_t flag = direction == AUDIO_PLAYBACK ? PCM_ENABLE_OUTPUT : PCM_ENABLE_INPUT;
                struct audio_caps caps;
                if (!(wanted & flag) || audio_oss_caps(state, direction, &caps) < 0) continue;
                struct audio_pcm *pcm = audio_pcm_open(state->card, state->devices[direction],
                                                       direction, &error);
                if (!pcm) break;
                if (direction == AUDIO_PLAYBACK) state->playback_pcm = pcm;
                else state->capture_pcm = pcm;
                available |= flag;
                formats &= caps.formats;
                rates &= caps.rates;
                if (minimum < caps.channels_min) minimum = caps.channels_min;
                if (maximum > caps.channels_max) maximum = caps.channels_max;
            }
            if (!error && !available) error = -ENODEV;
            if (!error && (!(formats & (AUDIO_FORMAT_S16_LE | AUDIO_FORMAT_S8 | AUDIO_FORMAT_U8)) ||
                           !rates || minimum > maximum)) error = -EINVAL;
            if (error) {
                audio_oss_pcm_release(state);
                *state = (struct audio_oss_file){0};
                file->audio_oss_file = NULL;
                return error;
            }
            state->direction_flags = state->trigger = available;
            state->rate = audio_oss_nearest_rate(state->rate, rates);
            if (!(formats & AUDIO_FORMAT_S16_LE))
                state->format = formats & AUDIO_FORMAT_U8 ? AFMT_U8 : AFMT_S8;
            if (state->channels < minimum) state->channels = minimum;
            if (state->channels > maximum) state->channels = maximum;
            if (state->channels * audio_oss_sample_bytes(state->format) > sizeof(state->tail)) {
                audio_oss_pcm_release(state); *state = (struct audio_oss_file){0};
                file->audio_oss_file = NULL;
                return -EINVAL;
            }
        }
        return 0;
    }
    return -ENFILE;
}

/** @brief Release one OSS description's core streams or exact legacy lease.
 * @param file Final owned description. @return Zero or -EBADF.
 * Task context; a stale legacy close cannot release a newer backend's lease.
 */
int audio_oss_close(struct task_file *file)
{
    if (!file || !file->audio_oss_file || !file->audio_oss_file->used)
        return -EBADF;
    audio_oss_pcm_release(file->audio_oss_file);
    if (file->audio_oss_file->legacy_generation)
        driver_manager_audio_release(file->audio_oss_file->legacy_generation);
    *file->audio_oss_file = (struct audio_oss_file){0};
    file->audio_oss_file = NULL;
    return 0;
}

/** @brief Read capture bytes from the core PCM ring with byte-stream tails. */
int audio_oss_read(struct task *task, struct task_file *file,
                   void *buffer, uint32_t count)
{
    if (task) audio_pcm_wait_remove(task);
    if (!file || !file->audio_oss_file || !file->audio_oss_file->used)
        return -EBADF;
    if (count && !buffer) return -EFAULT;
    if (!count) return 0;
    struct audio_oss_file *state = file->audio_oss_file;
    if (!(state->direction_flags & PCM_ENABLE_INPUT)) return -EBADF;
    if (!(state->trigger & PCM_ENABLE_INPUT)) return -EAGAIN;
    int ret = audio_oss_pcm_prepare(state, AUDIO_CAPTURE);
    if (ret < 0) return ret;
    uint32_t frame_bytes = audio_oss_sample_bytes(state->format) * state->channels;
    if (!frame_bytes || frame_bytes > sizeof(state->capture_tail)) return -EINVAL;
    uint8_t *output = buffer;
    uint32_t consumed = 0;
    while (consumed < count) {
        uint32_t epoch = audio_pcm_event_epoch();
        struct audio_pcm_status status;
        ret = audio_pcm_status(state->capture_pcm, &status);
        if (ret < 0 || status.error || status.state == AUDIO_PCM_DISCONNECTED)
            return consumed ? (int)consumed : ret < 0 ? ret :
                status.error ? status.error : -ENODEV;
        if (state->capture_tail_bytes) {
            uint32_t take = count - consumed < state->capture_tail_bytes
                ? count - consumed : state->capture_tail_bytes;
            __builtin_memcpy(output + consumed, state->capture_tail, take);
            state->capture_tail_bytes -= take;
            for (uint32_t i = 0; i < state->capture_tail_bytes; ++i)
                state->capture_tail[i] = state->capture_tail[take + i];
            consumed += take;
            continue;
        }
        if (audio_oss_pcm_bytes(state->capture_pcm, 1) < frame_bytes) {
            if (consumed) break;
            if (!task || !task->pid || (file->flags & RELIEFOS_O_NONBLOCK))
                return -EAGAIN;
            int wait = audio_oss_wait_retry(task, epoch);
            if (wait) return wait;
            continue;
        }
        long frames = audio_pcm_transfer(state->capture_pcm, state->capture_tail, 1);
        if (frames < 0) {
            if (!consumed && frames == -EAGAIN && (file->flags & RELIEFOS_O_NONBLOCK))
                return -EAGAIN;
            return consumed ? (int)consumed : (int)frames;
        }
        if (frames != 1) break;
        state->capture_tail_bytes = frame_bytes;
        ++state->read_blocks;
    }
    state->read_bytes += consumed;
    return (int)consumed;
}

/** @brief Transfer complete playback frames through the description's backend.
 * @param task Current task for core PCM waits. @param file Borrowed pinned OFD.
 * @param buffer Borrowed bytes. @param count Requested byte count.
 * @return Accepted bytes or errno. Task context; partial frame tails remain
 * owned by this description, and v1 backpressure uses finite timer retries.
 */
int audio_oss_write(struct task *task, struct task_file *file,
                    const void *buffer, uint32_t count)
{
    if (task) audio_pcm_wait_remove(task);
    if (!file || !file->audio_oss_file || !file->audio_oss_file->used)
        return -EBADF;
    if (count && !buffer) return -EFAULT;
    if (!count) return 0;
    struct audio_oss_file *state = file->audio_oss_file;
    if (!(state->direction_flags & PCM_ENABLE_OUTPUT)) return -EBADF;
    if (!audio_oss_backend_ready(state)) return -ENODEV;
    if (!(state->trigger & PCM_ENABLE_OUTPUT)) return -EAGAIN;
    if (!state->legacy_generation) {
        int ret = audio_oss_pcm_prepare(state, AUDIO_PLAYBACK);
        if (ret < 0) return ret;
    }
    uint32_t frame_bytes = audio_oss_sample_bytes(state->format) * state->channels;
    if (state->playback_pcm) {
        if (!frame_bytes || frame_bytes > sizeof(state->tail)) return -EINVAL;
        const uint8_t *input = buffer;
        uint32_t consumed = 0;
        uint8_t chunk[RELIEFOS_AUDIO_IO_SLICE_BYTES];
        while (consumed < count) {
            uint32_t epoch = audio_pcm_event_epoch();
            if (!state->tail_bytes && count - consumed >= frame_bytes) {
                uint32_t chunk_bytes = count - consumed;
                if (chunk_bytes > sizeof(chunk)) chunk_bytes = sizeof(chunk);
                chunk_bytes -= chunk_bytes % frame_bytes;
                uint32_t chunk_frames = chunk_bytes / frame_bytes;
                __builtin_memcpy(chunk, input + consumed, chunk_bytes);
                long frames = audio_pcm_transfer(state->playback_pcm, chunk,
                                                 chunk_frames);
                if (frames < 0) {
                    if (consumed) return (int)consumed;
                    if (frames != -EAGAIN || !task || !task->pid ||
                        (file->flags & RELIEFOS_O_NONBLOCK)) return (int)frames;
                    int wait = audio_oss_wait_retry(task, epoch);
                    if (wait) return wait;
                    continue;
                }
                if ((uint64_t)frames > chunk_frames) return consumed ?
                    (int)consumed : -EIO;
                if (!frames) break;
                uint32_t accepted = (uint32_t)frames * frame_bytes;
                consumed += accepted;
                state->queued_bytes = audio_oss_pcm_bytes(state->playback_pcm, 0);
                state->written_bytes += accepted;
                state->blocks = (uint32_t)(state->written_bytes / state->fragment_bytes);
                if (state->fragment_bytes &&
                    !(state->written_bytes % state->fragment_bytes)) {
                    int start_ret = audio_oss_start_playback(state, 0);
                    if (start_ret < 0)
                        return consumed ? (int)consumed : start_ret;
                }
                continue;
            }
            uint8_t frame[AUDIO_OSS_FRAME_MAX];
            uint32_t frame_from_input = 0;
            uint32_t frame_from_tail = 0;
            if (state->tail_bytes) {
                frame_from_tail = 1;
                uint32_t need = frame_bytes - state->tail_bytes;
                uint32_t take = count - consumed < need ? count - consumed : need;
                __builtin_memcpy(state->tail + state->tail_bytes, input + consumed, take);
                state->tail_bytes += take;
                consumed += take;
                if (state->tail_bytes < frame_bytes) break;
                __builtin_memcpy(frame, state->tail, frame_bytes);
                state->tail_bytes = 0;
            } else if (count - consumed < frame_bytes) {
                __builtin_memcpy(state->tail, input + consumed, count - consumed);
                state->tail_bytes = count - consumed;
                consumed = count;
                break;
            } else {
                __builtin_memcpy(frame, input + consumed, frame_bytes);
                frame_from_input = frame_bytes;
            }
            long frames = audio_pcm_transfer(state->playback_pcm, frame, 1);
            if (frames < 0) {
                if (frame_from_tail && !state->tail_bytes) {
                    __builtin_memcpy(state->tail, frame, frame_bytes);
                    state->tail_bytes = frame_bytes;
                }
                if (consumed) return (int)consumed;
                if (frames != -EAGAIN || !task || !task->pid ||
                    (file->flags & RELIEFOS_O_NONBLOCK)) return (int)frames;
                int wait = audio_oss_wait_retry(task, epoch);
                if (wait) return wait;
                continue;
            }
            if (frames != 1) break;
            if (frame_from_input) consumed += frame_from_input;
            state->queued_bytes = audio_oss_pcm_bytes(state->playback_pcm, 0);
            state->written_bytes += frame_bytes;
            state->blocks = (uint32_t)(state->written_bytes / state->fragment_bytes);
            if (state->fragment_bytes &&
                !(state->written_bytes % state->fragment_bytes)) {
                int start_ret = audio_oss_start_playback(state, 0);
                if (start_ret < 0)
                    return consumed ? (int)consumed : start_ret;
            }
        }
        return (int)consumed;
    }
    uint32_t capacity = audio_oss_capacity(state);
    const uint8_t *input = buffer;
    uint32_t consumed = 0;
    while (consumed < count) {
        uint32_t queued = audio_oss_driver_queue(state);
        if (queued > capacity) queued = capacity;
        if (queued > capacity - frame_bytes) {
            if (consumed) break;
            /* v1 exposes no period notifications. The syscall's EAGAIN
             * retry sleeps for a timer tick instead of a PCM event wait. */
            return -EAGAIN;
        }
        if (!state->tail_bytes && count-consumed >= frame_bytes) {
            uint32_t chunk=count-consumed;
            if(chunk>RELIEFOS_AUDIO_IO_SLICE_BYTES)chunk=RELIEFOS_AUDIO_IO_SLICE_BYTES;
            if(chunk>capacity-queued)chunk=capacity-queued;
            chunk-=chunk%frame_bytes;
            uint32_t status;
            long written=driver_manager_audio_write_bound(state->legacy_generation,
                input+consumed,chunk,&status);
            if(written<0)return consumed?(int)consumed:(int)written;
            if(!written)return consumed?(int)consumed:-EAGAIN;
            if((uint64_t)written>chunk || written%frame_bytes)return consumed?(int)consumed:-EIO;
            consumed+=(uint32_t)written;state->written_bytes+=(uint32_t)written;
            state->blocks=(uint32_t)(state->written_bytes/state->fragment_bytes);
            state->queued_bytes=queued+(uint32_t)written;
            if((uint32_t)written<chunk)break;
            continue;
        }
        uint8_t frame[AUDIO_OSS_FRAME_MAX];
        uint32_t frame_from_input = 0;
        if (state->tail_bytes) {
            uint32_t need = frame_bytes - state->tail_bytes;
            uint32_t take = count - consumed < need ? count - consumed : need;
            __builtin_memcpy(state->tail + state->tail_bytes, input + consumed, take);
            state->tail_bytes += take;
            consumed += take;
            if (state->tail_bytes < frame_bytes) break;
            __builtin_memcpy(frame, state->tail, frame_bytes);
            state->tail_bytes = 0;
        } else if (count - consumed < frame_bytes) {
            __builtin_memcpy(state->tail, input + consumed, count - consumed);
            state->tail_bytes = count - consumed;
            consumed = count;
            break;
        } else {
            __builtin_memcpy(frame, input + consumed, frame_bytes);
            frame_from_input = frame_bytes;
        }
        uint32_t status = RELIEFOS_AUDIO_STATUS_OK;
        long written = driver_manager_audio_write_bound(state->legacy_generation,frame,frame_bytes,&status);
        if (written < 0) {
            if (!state->tail_bytes) {
                __builtin_memcpy(state->tail, frame, frame_bytes);
                state->tail_bytes = frame_bytes;
            }
            if (!consumed && status == RELIEFOS_AUDIO_STATUS_WOULD_BLOCK &&
                (file->flags & RELIEFOS_O_NONBLOCK)) return -EAGAIN;
            if (!consumed) return (int)written;
            break;
        }
        if (written == 0 && status == RELIEFOS_AUDIO_STATUS_WOULD_BLOCK) {
            __builtin_memcpy(state->tail, frame, frame_bytes);
            state->tail_bytes = frame_bytes;
            if (!consumed && (file->flags & RELIEFOS_O_NONBLOCK)) return -EAGAIN;
            break;
        }
        if ((uint32_t)written < frame_bytes) {
            __builtin_memcpy(state->tail, frame + written, frame_bytes - (uint32_t)written);
            state->tail_bytes = frame_bytes - (uint32_t)written;
        }
        if (frame_from_input) consumed += frame_from_input;
        state->queued_bytes = queued + (uint32_t)written;
        state->written_bytes += (uint32_t)written;
        state->blocks += (state->written_bytes / state->fragment_bytes) !=
                         ((state->written_bytes - (uint32_t)written) / state->fragment_bytes);
        if ((uint32_t)written < frame_bytes) break;
    }
    return (int)consumed;
}

/** @brief Validate and copy one OSS integer ioctl argument. */
static int audio_oss_arg(uint64_t address, int **out)
{
    if (!address || !user_range_writable(address, sizeof(int))) return -EFAULT;
    if (out) *out = (int *)(uintptr_t)address;
    return 0;
}

/** @brief Dispatch OSS negotiation against the bound backend's real abilities.
 * @param task Current task for core waits. @param file Borrowed pinned OFD.
 * @param request OSS ioctl. @param address Validated user output/input address.
 * @return Zero or errno. Task context; v1 never claims capture, reset, pause or
 * hardware pointer controls, and no ALSA card is fabricated for v1 callbacks.
 */
int audio_oss_ioctl(struct task *task, struct task_file *file,
                    uint64_t request, uint64_t address)
{
    (void)task;
    if (!file || !file->audio_oss_file || !file->audio_oss_file->used)
        return -EBADF;
    struct audio_oss_file *state = file->audio_oss_file;
    if (!audio_oss_backend_ready(state) && request != SNDCTL_DSP_NONBLOCK)
        return -ENODEV;
    int *value = NULL;
    int ret;
    switch (request) {
    case SNDCTL_DSP_RESET:
        if (state->legacy_generation) return -ENOTTY;
        ret = audio_oss_reconfigure(state);
        if (ret < 0) return ret;
        state->queued_bytes = 0;
        state->written_bytes = 0;
        state->blocks = 0;
        state->read_bytes = 0;
        state->read_blocks = 0;
        state->trigger = state->direction_flags;
        state->tail_bytes = 0;
        state->capture_tail_bytes = 0;
        state->last_output_hw_frames = state->last_input_hw_frames = 0;
        return 0;
    case SNDCTL_DSP_SYNC: {
        uint32_t frame_bytes = audio_oss_sample_bytes(state->format) * state->channels;
        if (state->legacy_generation && state->tail_bytes == frame_bytes) {
            uint32_t status;
            long written=driver_manager_audio_write_bound(state->legacy_generation,
                state->tail,frame_bytes,&status);
            if (written<0) return (int)written;
            if ((uint32_t)written != frame_bytes) return -EAGAIN;
            state->tail_bytes=0;state->written_bytes+=frame_bytes;
        }
        while (state->tail_bytes == frame_bytes && state->playback_pcm) {
            uint32_t epoch = audio_pcm_event_epoch();
            long frames = audio_pcm_transfer(state->playback_pcm, state->tail, 1);
            if (frames == 1) {
                state->tail_bytes = 0;
                state->written_bytes += frame_bytes;
                break;
            }
            if (frames != -EAGAIN || !task || !task->pid) return (int)frames;
            int wait = audio_oss_wait_retry(task, epoch);
            if (wait) return wait;
        }
        /* OSS sync consumes accepted partial frames and pads to one complete
         * fragment. A local descriptor copy makes draining blocking even when
         * the shared description has O_NONBLOCK, without changing its flags. */
        if (state->tail_bytes || state->playback_pcm) {
            uint64_t accepted = state->written_bytes + state->tail_bytes;
            uint32_t padding = (state->fragment_bytes -
                accepted % state->fragment_bytes) % state->fragment_bytes;
            struct task_file blocking = *file;
            blocking.flags &= ~RELIEFOS_O_NONBLOCK;
            uint8_t silence[256];
            __builtin_memset(silence, state->format == AFMT_U8 ? 0x80 : 0, sizeof(silence));
            while (padding) {
                uint32_t count = padding < sizeof(silence) ? padding : sizeof(silence);
                int written = audio_oss_write(task, &blocking, silence, count);
                if (written <= 0 || written == KERNEL_SYSCALL_BLOCKED) return written;
                padding -= (uint32_t)written;
            }
        }
        if (state->playback_pcm) {
            struct audio_pcm_status status;
            ret = audio_pcm_status(state->playback_pcm, &status);
            if (ret < 0) return ret;
            if (status.state != AUDIO_PCM_OPEN) {
                int drain = audio_oss_drain_wait(task, file, state->playback_pcm);
                state->queued_bytes = audio_oss_queue(state);
                if (drain) return drain;
            }
        }
        state->queued_bytes = audio_oss_queue(state);
        if (state->queued_bytes) return -EAGAIN;
        if (state->capture_pcm) {
            ret = audio_oss_drop_pcm(state->capture_pcm);
            if (!ret) ret = audio_pcm_hw_free(state->capture_pcm);
            if (ret < 0) return ret;
            state->capture_tail_bytes = 0;
            state->last_input_hw_frames = 0;
        }
        return 0;
    }
    case SNDCTL_DSP_NONBLOCK:
        file->flags |= RELIEFOS_O_NONBLOCK;
        return 0;
    case SNDCTL_DSP_GETFMTS:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = audio_oss_core_oss_formats(state);
        return 0;
    case SNDCTL_DSP_GETCAPS:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = state->legacy_generation ? 0 : DSP_CAP_TRIGGER;
        if ((state->direction_flags & (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT)) ==
            (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT)) {
            struct audio_caps playback, capture;
            if (!audio_oss_caps(state, AUDIO_PLAYBACK, &playback) &&
                !audio_oss_caps(state, AUDIO_CAPTURE, &capture)) *value |= DSP_CAP_DUPLEX;
        }
        return 0;
    case SNDCTL_DSP_GETBLKSIZE:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        uint32_t frame_bytes = audio_oss_sample_bytes(state->format) * state->channels;
        *value = (int)(state->fragment_bytes / frame_bytes * frame_bytes);
        return 0;
    case SNDCTL_DSP_SETFMT: {
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        int requested = *value;
        if (requested == AFMT_QUERY) {
            *value = (int)state->format;
            return 0;
        }
        int formats = audio_oss_core_oss_formats(state);
        if (!(formats & requested) ||
            (requested != AFMT_S16_LE && requested != AFMT_U8 && requested != AFMT_S8))
            requested = formats & AFMT_S16_LE ? AFMT_S16_LE :
                formats & AFMT_U8 ? AFMT_U8 : formats & AFMT_S8 ? AFMT_S8 : 0;
        if (!requested) return -EINVAL;
        if (state->format == (uint32_t)requested) { *value = requested; return 0; }
        ret = audio_oss_reconfigure(state);
        if (ret < 0) return ret;
        state->format = (uint32_t)requested;
        *value = requested;
        return 0;
    }
    case SNDCTL_DSP_SPEED: {
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        if (*value <= 0) {
            *value = (int)state->rate;
            return 0;
        }
        if (!state->legacy_generation && !state->playback_pcm &&
            !state->capture_pcm && audio_card_snapshot(0, NULL, NULL) < 0)
            return -ENODEV;
        if (!state->legacy_generation) {
            struct audio_caps caps;
            ret = audio_oss_active_caps(state, &caps);
            if (ret < 0) return ret;
            uint32_t selected = audio_oss_nearest_rate((uint32_t)*value, caps.rates);
            if (!selected) return -EINVAL;
            if (selected != state->rate && (ret = audio_oss_reconfigure(state)) < 0) return ret;
            state->rate = selected;
            *value = (int)state->rate;
            return 0;
        }
        struct reliefos_audio_format format = {
            .sample_rate = (uint32_t)*value,
            .channels = (uint16_t)state->channels,
            .bits_per_sample = (uint16_t)(audio_oss_sample_bytes(state->format) * 8U),
        };
        /* An unchanged v1 rate is a negotiation query, not a hardware reset.
         * In particular, a new description can inherit undrained PCM from
         * the preceding player without needing to reprogram the converter. */
        if (format.sample_rate == state->rate) return 0;
        if (audio_oss_driver_queue(state) || state->tail_bytes) return -EBUSY;
        ret = driver_manager_audio_configure_bound(state->legacy_generation,&format);
        if (ret < 0) return ret;
        struct reliefos_audio_state actual;
        ret=driver_manager_audio_state_bound(state->legacy_generation,&actual);
        if(ret)return ret;
        state->rate = actual.sample_rate;
        *value = (int)state->rate;
        return 0;
    }
    case SNDCTL_DSP_STEREO:
    case SNDCTL_DSP_CHANNELS: {
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        if (request == SNDCTL_DSP_CHANNELS && *value == 0) {
            *value = (int)state->channels;
            return 0;
        }
        if (*value < 0 || (request == SNDCTL_DSP_STEREO && *value > 1) ||
            (request == SNDCTL_DSP_CHANNELS && *value > 128)) return -EINVAL;
        uint32_t channels = request == SNDCTL_DSP_STEREO ? (uint32_t)*value + 1u
            : *value ? (uint32_t)*value : 1u;
        if (state->legacy_generation) channels=2;
        else {
            struct audio_caps caps;
            ret = audio_oss_active_caps(state, &caps);
            if (ret < 0) return ret;
            if (channels < caps.channels_min) channels = caps.channels_min;
            if (channels > caps.channels_max) channels = caps.channels_max;
        }
        if (!channels || channels * audio_oss_sample_bytes(state->format) > sizeof(state->tail))
            return -EINVAL;
        if (channels != state->channels && (ret = audio_oss_reconfigure(state)) < 0) return ret;
        state->channels = channels;
        *value = request == SNDCTL_DSP_STEREO ? (int)channels - 1 : (int)channels;
        return 0;
    }
    case SOUND_PCM_READ_CHANNELS:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = (int)state->channels;
        return 0;
    case SOUND_PCM_READ_RATE:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = (int)state->rate;
        return 0;
    case SNDCTL_DSP_SETFRAGMENT: {
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        uint32_t encoded = (uint32_t)*value;
        uint32_t exponent = encoded & 0xffffU;
        uint32_t fragments = encoded >> 16;
        if (exponent < 8U || exponent > 16U || fragments < 2U || fragments > AUDIO_OSS_MAX_FRAGMENTS)
            return -EINVAL;
        uint32_t period_bytes = 1U << exponent;
        if (!state->legacy_generation) {
            struct audio_caps caps;
            ret = audio_oss_active_caps(state, &caps);
            if (ret < 0) return ret;
            if ((caps.period_bytes_min && period_bytes < caps.period_bytes_min) ||
                (caps.period_bytes_max && period_bytes > caps.period_bytes_max) ||
                (caps.buffer_bytes_max &&
                 (uint64_t)period_bytes * fragments > caps.buffer_bytes_max))
                return -EINVAL;
        }
        ret = audio_oss_reconfigure(state);
        if (ret < 0) return ret;
        state->fragment_bytes = 1U << exponent;
        state->fragment_count = fragments;
        return 0;
    }
    case SNDCTL_DSP_GETOSPACE: {
        if (!address || !user_range_writable(address, sizeof(struct audio_buf_info))) return -EFAULT;
        if ((state->trigger & PCM_ENABLE_OUTPUT) &&
            !state->legacy_generation) {
            ret = audio_oss_pcm_prepare(state, AUDIO_PLAYBACK);
            if (ret < 0) return ret;
        }
        struct audio_buf_info *info = (struct audio_buf_info *)(uintptr_t)address;
        uint32_t queued = audio_oss_queue(state);
        uint32_t capacity = audio_oss_capacity(state);
        if (queued > capacity) queued = capacity;
        uint32_t available = capacity - queued;
        *info = (struct audio_buf_info){
            .fragments = (int)(available / state->fragment_bytes),
            .fragstotal = (int)state->fragment_count,
            .fragsize = (int)state->fragment_bytes,
            .bytes = (int)available,
        };
        return 0;
    }
    case SNDCTL_DSP_GETISPACE: {
        if (state->legacy_generation) return -ENOTTY;
        if (!address || !user_range_writable(address, sizeof(struct audio_buf_info))) return -EFAULT;
        if ((state->trigger & PCM_ENABLE_INPUT) &&
            !state->legacy_generation) {
            ret = audio_oss_pcm_prepare(state, AUDIO_CAPTURE);
            if (ret < 0) return ret;
        }
        uint32_t available = state->capture_pcm ? audio_oss_pcm_bytes(state->capture_pcm, 1) : 0;
        *(struct audio_buf_info *)(uintptr_t)address = (struct audio_buf_info){
            .fragments = (int)(available / state->fragment_bytes),
            .fragstotal = (int)state->fragment_count,
            .fragsize = (int)state->fragment_bytes,
            .bytes = (int)available,
        };
        return 0;
    }
    case SNDCTL_DSP_GETTRIGGER:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = (int)state->trigger;
        return 0;
    case SNDCTL_DSP_SETTRIGGER:
        if (state->legacy_generation) {
            ret=audio_oss_arg(address,&value);if(ret)return ret;
            return *value==PCM_ENABLE_OUTPUT?0:-ENOTTY;
        }
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        if (*value & ~(PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT)) return -EINVAL;
        uint32_t old_trigger = state->trigger;
        uint32_t new_trigger = (uint32_t)*value & state->direction_flags;
        if ((old_trigger & PCM_ENABLE_OUTPUT) && !(new_trigger & PCM_ENABLE_OUTPUT)) {
            ret = audio_oss_drop_pcm(state->playback_pcm);
            if (ret < 0) return ret;
            if (state->playback_pcm) {
                ret = audio_pcm_hw_free(state->playback_pcm);
                if (ret < 0) return ret;
            }
            state->tail_bytes = 0;
            state->written_bytes = 0;
            state->last_output_hw_frames = 0;
        }
        if ((old_trigger & PCM_ENABLE_INPUT) && !(new_trigger & PCM_ENABLE_INPUT)) {
            ret = audio_oss_drop_pcm(state->capture_pcm);
            if (ret < 0) return ret;
            if (state->capture_pcm) {
                ret = audio_pcm_hw_free(state->capture_pcm);
                if (ret < 0) return ret;
            }
        }
        state->trigger = new_trigger;
        if (new_trigger & PCM_ENABLE_OUTPUT) {
            if (!(old_trigger & PCM_ENABLE_OUTPUT) || !state->playback_pcm) {
                ret = audio_oss_pcm_prepare(state, AUDIO_PLAYBACK);
                if (ret < 0) return ret;
            }
            /* Reasserting output enable has always been an explicit start
             * request, even when the descriptor was already enabled. */
            ret = audio_oss_start_playback(state, 1);
            if (ret < 0) return ret;
        }
        if ((new_trigger & PCM_ENABLE_INPUT) &&
            (!(old_trigger & PCM_ENABLE_INPUT) || !state->capture_pcm)) {
            ret = audio_oss_pcm_prepare(state, AUDIO_CAPTURE);
            if (ret < 0) return ret;
        }
        return 0;
    case SNDCTL_DSP_SETDUPLEX:
        if (state->direction_flags != (PCM_ENABLE_INPUT | PCM_ENABLE_OUTPUT)) return -EIO;
        struct audio_caps playback, capture;
        if (audio_oss_caps(state, AUDIO_PLAYBACK, &playback) < 0 ||
            audio_oss_caps(state, AUDIO_CAPTURE, &capture) < 0) return -EIO;
        return 0;
    case SNDCTL_DSP_GETODELAY:
        ret = audio_oss_arg(address, &value);
        if (ret) return ret;
        *value = (int)audio_oss_queue(state);
        return 0;
    case SNDCTL_DSP_GETOPTR: {
        if (state->legacy_generation) return -ENOTTY;
        if (!address || !user_range_writable(address, sizeof(count_info))) return -EFAULT;
        count_info *info = (count_info *)(uintptr_t)address;
        if (state->playback_pcm && audio_oss_pcm_count(state->playback_pcm,
                                                       state->fragment_bytes,
                                                       &state->last_output_hw_frames,
                                                       info) < 0)
            return -ENODEV;
        else if (!state->playback_pcm)
            *info = (count_info){
                .bytes = (int)state->written_bytes,
                .blocks = (int)state->blocks,
                .ptr = (int)(state->written_bytes % state->fragment_bytes),
            };
        uint32_t queued = audio_oss_queue(state);
        state->queued_bytes = queued;
        return 0;
    }
    case SNDCTL_DSP_GETIPTR:
        if (state->legacy_generation) return -ENOTTY;
        if (!address || !user_range_writable(address, sizeof(count_info))) return -EFAULT;
        count_info *info = (count_info *)(uintptr_t)address;
        if (state->capture_pcm && audio_oss_pcm_count(state->capture_pcm,
                                                      state->fragment_bytes,
                                                      &state->last_input_hw_frames,
                                                      info) < 0)
            return -ENODEV;
        else if (!state->capture_pcm)
            *info = (count_info){
                .bytes = (int)state->read_bytes,
                .blocks = (int)state->read_blocks,
                .ptr = (int)(state->read_bytes % state->fragment_bytes),
            };
        return 0;
    default:
        return -ENOTTY;
    }
}

/** @brief Return OSS output readiness and hardware error status. */
short audio_oss_poll(const struct task_file *file, short events)
{
    if (!file || !file->audio_oss_file || !file->audio_oss_file->used)
        return POLLNVAL;
    const struct audio_oss_file *state = file->audio_oss_file;
    if (!audio_oss_backend_ready(state)) return POLLERR;
    short ready = 0;
    struct audio_pcm *directions[] = {state->playback_pcm, state->capture_pcm};
    for (uint32_t i = 0; i < 2; ++i) {
        struct audio_pcm_status status;
        if (!directions[i]) continue;
        if (audio_pcm_status(directions[i], &status) < 0 ||
            status.state == AUDIO_PCM_DISCONNECTED) return POLLERR | POLLHUP;
        if (status.error || status.state == AUDIO_PCM_XRUN) return POLLERR;
    }
    if ((events & POLLIN) && (state->direction_flags & PCM_ENABLE_INPUT) &&
        (state->capture_tail_bytes || (state->capture_pcm &&
        audio_oss_pcm_bytes(state->capture_pcm, 1) >=
            audio_oss_sample_bytes(state->format) * state->channels)))
        ready |= POLLIN;
    if ((events & POLLOUT) && (state->direction_flags & PCM_ENABLE_OUTPUT)) {
        uint32_t queued = audio_oss_queue(state);
        if (queued < audio_oss_capacity(state)) ready |= POLLOUT;
    }
    return ready;
}
