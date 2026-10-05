#include <limits.h>
#include <sound/asound.h>
#include <stddef.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/time.h>
#include <reliefnt/time.h>
#include <reliefnt/audio.h>
#include <reliefnt/lock.h>

#define AUDIO_PCM_MAX 32u
#define AUDIO_PCM_BUFFER_BYTES 524288u
#define AUDIO_PCM_BASE_BUFFER_BYTES 65536u
#define AUDIO_PCM_BOUNDARY_BASE (1ULL << 60)
#define AUDIO_PCM_MMAP_PAGE_BYTES 4096u
#define AUDIO_PCM_PROT_READ 0x1u
#define AUDIO_PCM_PROT_WRITE 0x2u

struct audio_pcm {
    uint32_t live;
    uint32_t card;
    uint32_t stream;
    uint32_t device;
    enum audio_direction direction;
    enum audio_pcm_state state;
    struct audio_caps caps;
    struct audio_params params;
    struct audio_params_ext selected;
    struct audio_format_caps format_caps;
    uint32_t explicit_format;
    struct audio_dma dma;
    uint8_t buffer[AUDIO_PCM_BASE_BUFFER_BYTES] __attribute__((aligned(4096)));
    uint8_t *extended_buffer;
    void *extended_storage;
    uint64_t hw_ptr;
    uint64_t driver_frames;
    uint64_t appl_ptr;
    uint64_t boundary;
    uint64_t wake_seq;
    uint64_t avail_min;
    uint32_t auto_start;
    uint64_t start_threshold;
    uint64_t stop_threshold;
    uint32_t tstamp_type;
    uint32_t tstamp_mode;
    struct linux_timespec trigger_tstamp;
    uint64_t silence_threshold;
    uint64_t silence_size;
    uint64_t silence_origin;
    uint64_t silence_covered;
    uint32_t silence_initialized;
    uint32_t generation;
    uint32_t mapping_refs;
    uint32_t data_refs;
    uint32_t control_refs;
    uint8_t status_page[4096] __attribute__((aligned(4096)));
    uint8_t control_page[4096] __attribute__((aligned(4096)));
    uint32_t file_refs;
    uint32_t close_pending;
    int error;
    struct audio_pcm *link;
    struct kernel_spinlock lock;
};

static struct audio_pcm pcm_slots[AUDIO_PCM_MAX];
static uint32_t pending_releases;

/* Host-side PCM fixtures do not link the kernel heap.  The weak declarations
 * keep their small inline-buffer paths self-contained while the kernel linker
 * resolves these calls to the real page-backed heap implementation. */
extern void *kernel_malloc(size_t size) __attribute__((weak));
extern void kernel_free(void *memory) __attribute__((weak));

static uint8_t *audio_pcm_buffer(struct audio_pcm *pcm)
{
    return pcm->extended_buffer ? pcm->extended_buffer : pcm->buffer;
}

static const uint8_t *audio_pcm_const_buffer(const struct audio_pcm *pcm)
{
    return pcm->extended_buffer ? pcm->extended_buffer : pcm->buffer;
}

static void audio_pcm_free_buffer(struct audio_pcm *pcm)
{
    void *storage = pcm->extended_storage;
    pcm->extended_storage = NULL;
    pcm->extended_buffer = NULL;
    if (storage && kernel_free) kernel_free(storage);
}

static int audio_pcm_prepare_buffer(struct audio_pcm *pcm, uint32_t bytes,
                                    void **new_storage, uint8_t **new_buffer)
{
    *new_storage = NULL;
    *new_buffer = audio_pcm_buffer(pcm);
    if (bytes <= AUDIO_PCM_BASE_BUFFER_BYTES || pcm->extended_buffer) {
        if (pcm->extended_buffer) *new_buffer = pcm->extended_buffer;
        return 0;
    }
    if (!kernel_malloc || !kernel_free) return -ENOMEM;
    /* Retain one backing through HW_FREE/reconfiguration. Reserve the core
     * limit so a later larger geometry cannot overrun the reused allocation. */
    size_t allocation = (size_t)AUDIO_PCM_BUFFER_BYTES + AUDIO_PCM_MMAP_PAGE_BYTES - 1u;
    void *storage = kernel_malloc(allocation);
    if (!storage) return -ENOMEM;
    uintptr_t aligned = ((uintptr_t)storage + AUDIO_PCM_MMAP_PAGE_BYTES - 1u) &
                        ~(uintptr_t)(AUDIO_PCM_MMAP_PAGE_BYTES - 1u);
    *new_storage = storage;
    *new_buffer = (uint8_t *)aligned;
    return 0;
}

/** @brief Silence uncommitted playback samples without committing user frames.
 * @param pcm Configured PCM under its IRQ-safe lock; task or IRQ context.
 * @return None. Tracks silence independently of appl_ptr and never moves it.
 */
static void audio_pcm_silence_locked(struct audio_pcm *pcm)
{
    if (pcm->direction != AUDIO_PLAYBACK || !pcm->silence_size ||
        !pcm->params.buffer_frames) return;
    uint64_t capacity = pcm->params.buffer_frames;
    bool continuous = pcm->silence_size >= pcm->boundary;
    uint64_t origin = continuous ? pcm->hw_ptr : pcm->appl_ptr;
    uint64_t queued = (pcm->appl_ptr + pcm->boundary - pcm->hw_ptr) % pcm->boundary;
    if (!pcm->silence_initialized) {
        pcm->silence_covered = continuous && queued <= capacity ? queued : 0;
        pcm->silence_initialized = 1;
    } else {
        uint64_t advanced = (origin + pcm->boundary - pcm->silence_origin) % pcm->boundary;
        pcm->silence_covered = advanced < pcm->silence_covered
            ? pcm->silence_covered - advanced : 0;
    }
    pcm->silence_origin = origin;
    uint64_t frames;
    if (continuous) {
        frames = capacity - pcm->silence_covered;
    } else {
        if (queued > capacity || queued + pcm->silence_covered >= pcm->silence_threshold)
            return;
        frames = pcm->silence_threshold - queued - pcm->silence_covered;
        if (frames > pcm->silence_size) frames = pcm->silence_size;
        uint64_t room = capacity - queued - pcm->silence_covered;
        if (frames > room) frames = room;
    }
    uint32_t offset = (uint32_t)((origin + pcm->silence_covered) % capacity) *
                      pcm->params.frame_bytes;
    uint32_t bytes = (uint32_t)frames * pcm->params.frame_bytes;
    uint32_t first = pcm->dma.bytes - offset;
    if (first > bytes) first = bytes;
    int sample = pcm->selected.format == AUDIO_FORMAT_U8 ? 0x80 : 0;
    uint8_t *buffer = audio_pcm_buffer(pcm);
    memset(buffer + offset, sample, first);
    if (bytes > first) memset(buffer, sample, bytes - first);
    pcm->silence_covered += frames;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/** @brief Publish only trusted state/counters to the user-readable metadata page.
 * @param pcm Live core object under its IRQ-safe lock; the page is core-owned.
 * @return None. Task/IRQ safe; release stores order DMA-visible progress.
 */
static void audio_pcm_publish_locked(struct audio_pcm *pcm)
{
    struct __snd_pcm_mmap_status *st = (void *)pcm->status_page;
    __atomic_store_n(&st->hw_ptr,pcm->hw_ptr,__ATOMIC_RELEASE);
    if (pcm->tstamp_mode) {
        struct linux_timespec now;
        int clock = pcm->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW
            ? LINUX_CLOCK_MONOTONIC_RAW : pcm->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_MONOTONIC
            ? LINUX_CLOCK_MONOTONIC : LINUX_CLOCK_REALTIME;
        if (!time_clock_get(clock, &now)) {
            st->tstamp.tv_sec = now.tv_sec;
            st->tstamp.tv_nsec = now.tv_nsec;
        }
    }
    __atomic_store_n(&st->state,(snd_pcm_state_t)pcm->state,__ATOMIC_RELEASE);
}

/** @brief Publish committed control values after a task-context pointer change.
 * @param pcm Core object under its IRQ-safe lock; no ownership transfer.
 * @return None. Does not run for ordinary IRQ progress, preserving user updates.
 */
static void audio_pcm_publish_control_locked(struct audio_pcm *pcm)
{
    struct __snd_pcm_mmap_control *ct = (void *)pcm->control_page;
    __atomic_store_n(&ct->appl_ptr,pcm->appl_ptr,__ATOMIC_RELEASE);
    __atomic_store_n(&ct->avail_min,pcm->avail_min,__ATOMIC_RELEASE);
}

/** @brief Validate the ALSA boundary before importing shared control values.
 * @param pcm Configured live object under its IRQ-safe lock.
 * @return Zero or -EINVAL; no core field changes for an invalid pointer.
 * Task/IRQ context; bounded acquire loads pair with client release stores.
 */
static int audio_pcm_import_control_locked(struct audio_pcm *pcm)
{
    if (!pcm->control_refs || !pcm->params.buffer_frames) return 0;
    struct __snd_pcm_mmap_control *ct = (void *)pcm->control_page;
    uint64_t appl = __atomic_load_n(&ct->appl_ptr,__ATOMIC_ACQUIRE);
    uint64_t minimum = __atomic_load_n(&ct->avail_min,__ATOMIC_ACQUIRE);
    if (appl >= pcm->boundary) return -EINVAL;
    /* Continuous dmix/dsnoop slaves keep appl_ptr behind a running hw_ptr.
     * Ring availability and XRUN detection are separate from pointer validity. */
    pcm->appl_ptr = appl;
    pcm->avail_min = minimum;
    return 0;
}

/** @brief Import forward progress without letting a delayed IRQ rewind HWSYNC.
 * @param pcm Live configured PCM under its IRQ-safe lock.
 * @param frames Cumulative driver frames in the current prepare epoch.
 * @return None. Compare cumulative frames before wrapping the public pointer.
 */
static void audio_pcm_progress_locked(struct audio_pcm *pcm, uint64_t frames)
{
    if (frames >= pcm->driver_frames) {
        pcm->driver_frames = frames;
        pcm->hw_ptr = frames % pcm->boundary;
    }
}

/** @brief Update permanent PCM metadata after a pinned registry notification.
 * @param stream Core generation stream token; stale/disconnected slots are ignored.
 * @param frames Cumulative completed driver frames in the prepare epoch.
 * @param error Zero, negative XRUN error, or -ENODEV on forced disconnect.
 * @return None. IRQ/task safe, bounded by AUDIO_PCM_MAX, no callbacks or sleep.
 */
static void audio_pcm_notify(uint32_t stream, uint64_t frames, int error)
{
    for (uint32_t i=0;i<AUDIO_PCM_MAX;++i) {
        struct audio_pcm *pcm = &pcm_slots[i];
        if (!__atomic_load_n(&pcm->live,__ATOMIC_ACQUIRE)) continue;
        uint64_t flags;
        kernel_spin_lock_irqsave(&pcm->lock,&flags);
        if (pcm->live && pcm->stream == stream &&
            pcm->state != AUDIO_PCM_DISCONNECTED) {
            audio_pcm_progress_locked(pcm, frames);
            if (!error && pcm->state == AUDIO_PCM_RUNNING &&
                !audio_pcm_import_control_locked(pcm))
                audio_pcm_silence_locked(pcm);
            if (error) {
                pcm->error = error;
                pcm->state = error == -ENODEV ? AUDIO_PCM_DISCONNECTED : AUDIO_PCM_XRUN;
            }
            ++pcm->wake_seq;
            audio_pcm_publish_locked(pcm);
        }
        kernel_spin_unlock_irqrestore(&pcm->lock,flags);
    }
}

static uint32_t audio_pcm_container_bytes(uint32_t sample_bits)
{
    if (sample_bits <= 8u) return 1u;
    if (sample_bits <= 16u) return 2u;
    if (sample_bits <= 32u) return 4u;
    return 0u;
}

static uint64_t audio_pcm_format_bit(uint32_t sample_bits)
{
    if (sample_bits == 8u) return AUDIO_FORMAT_S8;
    if (sample_bits == 16u) return AUDIO_FORMAT_S16_LE;
    if (sample_bits == 20u) return AUDIO_FORMAT_S20_LE;
    if (sample_bits == 24u) return AUDIO_FORMAT_S24_LE;
    if (sample_bits == 32u) return AUDIO_FORMAT_S32_LE;
    return 0;
}

static uint64_t audio_pcm_rate_bit(uint32_t rate)
{
    switch (rate) {
    case 8000u: return AUDIO_RATE_8000;
    case 11025u: return AUDIO_RATE_11025;
    case 16000u: return AUDIO_RATE_16000;
    case 22050u: return AUDIO_RATE_22050;
    case 32000u: return AUDIO_RATE_32000;
    case 44100u: return AUDIO_RATE_44100;
    case 48000u: return AUDIO_RATE_48000;
    case 88200u: return AUDIO_RATE_88200;
    case 96000u: return AUDIO_RATE_96000;
    case 176400u: return AUDIO_RATE_176400;
    case 192000u: return AUDIO_RATE_192000;
    default: return 0;
    }
}

static int audio_pcm_validate(const struct audio_pcm *pcm, const struct audio_params *params,
                             uint64_t format, uint32_t *bytes)
{
    if (!params || !params->rate || !params->channels || !params->sample_bits ||
        !params->period_frames || !params->buffer_frames ||
        params->buffer_frames < params->period_frames ||
        params->buffer_frames % params->period_frames) return -EINVAL;
    uint32_t container = audio_pcm_container_bytes(params->sample_bits);
    if (!container || params->channels > UINT32_MAX / container ||
        params->frame_bytes != params->channels * container) return -EINVAL;
    uint64_t rate = audio_pcm_rate_bit(params->rate);
    if ((pcm->caps.formats && !(pcm->caps.formats & format)) ||
        (pcm->caps.rates && !(pcm->caps.rates & rate)) ||
        (pcm->caps.channels_min && params->channels < pcm->caps.channels_min) ||
        (pcm->caps.channels_max && params->channels > pcm->caps.channels_max)) return -EINVAL;
    if (params->frame_bytes > UINT32_MAX / params->buffer_frames) return -EOVERFLOW;
    *bytes = params->frame_bytes * params->buffer_frames;
    if (*bytes > AUDIO_PCM_BUFFER_BYTES ||
        (pcm->caps.buffer_bytes_max && *bytes > pcm->caps.buffer_bytes_max)) return -EINVAL;
    uint32_t period_bytes = params->frame_bytes * params->period_frames;
    if ((pcm->caps.period_bytes_min && period_bytes < pcm->caps.period_bytes_min) ||
        (pcm->caps.period_bytes_max && period_bytes > pcm->caps.period_bytes_max)) return -EINVAL;
    return 0;
}

static void audio_pcm_reset_locked(struct audio_pcm *pcm)
{
    pcm->hw_ptr = 0;
    pcm->driver_frames = 0;
    pcm->appl_ptr = 0;
    pcm->trigger_tstamp = (struct linux_timespec){0};
    pcm->silence_initialized = 0;
    pcm->silence_covered = 0;
    pcm->error = 0;
    pcm->wake_seq++;
    memset(audio_pcm_buffer(pcm), pcm->selected.format == AUDIO_FORMAT_U8 ? 0x80 : 0,
           pcm->dma.bytes);
    audio_pcm_publish_control_locked(pcm);
}

/** @brief Test whether an operation may commit to this PCM generation.
 * @param pcm Borrowed PCM under its IRQ-safe lock, in task or IRQ context.
 * @return True while live and connected; forced disconnect is irreversible.
 */
static bool audio_pcm_connected_locked(const struct audio_pcm *pcm)
{
    return pcm->live && pcm->state != AUDIO_PCM_DISCONNECTED;
}

/** @brief Consume coalesced progress without changing disconnected metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Event count, XRUN/input errno, or ENODEV after permanent disconnect.
 * No PCM lock spans registry polling or a pinned hardware STOP callback.
 */
static int audio_pcm_event(struct audio_pcm *pcm)
{
    uint64_t frames = 0;
    int error = 0;
    int ret = audio_stream_period_poll(pcm->stream, &frames, &error);
    if (ret < 0) return ret;
    uint32_t stop = 0;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (ret > 0) audio_pcm_progress_locked(pcm, frames);
    int input = audio_pcm_import_control_locked(pcm);
    if (!input && pcm->state == AUDIO_PCM_RUNNING) audio_pcm_silence_locked(pcm);
    pcm->wake_seq++;
    if (error) {
        pcm->error = error;
        if (pcm->state == AUDIO_PCM_RUNNING || pcm->state == AUDIO_PCM_PAUSED ||
            pcm->state == AUDIO_PCM_DRAINING) {
            pcm->state = AUDIO_PCM_XRUN;
            stop = 1;
        }
    }
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    if (stop) (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
    return input ? input : (error ? error : ret);
}

/** @brief Snapshot state for an operation requiring a connected generation.
 * @param pcm Borrowed non-NULL PCM, serialized in task context.
 * @param state Caller-owned writable state output, unchanged on failure.
 * @return Zero or ENODEV; an IRQ-safe lock protects the snapshot.
 */
static int audio_pcm_live_state(struct audio_pcm *pcm, enum audio_pcm_state *state)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    *state = pcm->state;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

struct audio_pcm *audio_pcm_open(uint32_t card, uint32_t device,
                                 enum audio_direction direction, int *error)
{
    if (error) *error = -EINVAL;
    if (!error || (uint32_t)direction > AUDIO_CAPTURE) return NULL;
    for (uint32_t i = 0; i < AUDIO_PCM_MAX; ++i)
        if (pcm_slots[i].live && pcm_slots[i].card == card &&
            pcm_slots[i].device == device && pcm_slots[i].direction == direction) {
            *error = -EBUSY;
            return NULL;
        }
    struct audio_caps caps;
    int ret = audio_card_pcm_caps(card, device, direction, &caps);
    if (ret) { *error = ret; return NULL; }
    uint32_t stream;
    ret = audio_stream_open(card, device, direction, &stream);
    if (ret) { *error = ret; return NULL; }
    struct audio_format_caps format_caps = {0};
    int extret = audio_stream_format_caps(card, device, direction, &format_caps);
    if (extret && extret != -EOPNOTSUPP) {
        audio_stream_close(stream); *error = extret; return NULL;
    }
    if (extret) {
        format_caps.pcm = caps;
        /* Legacy prepare identifies signed samples by precision only. */
        format_caps.pcm.formats &= ~AUDIO_FORMAT_U8;
        for (unsigned n = 0; n < 5; ++n) {
            format_caps.format_bits[n] = 1u << n;
            format_caps.subformats[n] = 1u << AUDIO_SUBFORMAT_STD;
        }
    }
    struct audio_pcm *pcm = NULL;
    for (uint32_t i = 0; i < AUDIO_PCM_MAX; ++i)
        if (!pcm_slots[i].live) { pcm = &pcm_slots[i]; break; }
    if (!pcm) {
        audio_stream_close(stream);
        *error = -ENOSPC;
        return NULL;
    }
    /* A retired slot may still own a large DMA ring from its prior generation. */
    audio_pcm_free_buffer(pcm);
    *pcm = (struct audio_pcm){
        .live = 0,
        .file_refs = 1,
        .format_caps = format_caps,
        .explicit_format = !extret,
        .card = card,
        .stream = stream,
        .device = device,
        .direction = direction,
        .state = AUDIO_PCM_OPEN,
        .caps = caps,
        .boundary = AUDIO_PCM_BOUNDARY_BASE,
        .lock = KERNEL_SPINLOCK_INIT,
    };
    audio_pcm_set_notifier(audio_pcm_notify);
    audio_pcm_publish_locked(pcm);
    __atomic_store_n(&pcm->live,1,__ATOMIC_RELEASE);
    *error = 0;
    return pcm;
}

/** @brief Copy the live PCM capability snapshot without exposing PCM storage.
 * @param pcm Live PCM object.
 * @param caps Writable capability output.
 * @return Zero on success, or -ENODEV/-EINVAL.
 */
int audio_pcm_caps(struct audio_pcm *pcm, struct audio_caps *caps)
{
    if (!pcm || !caps) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    *caps = pcm->caps;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Validate and retain one PCM data/status/control mapping. */
int audio_pcm_mmap_acquire(struct audio_pcm *pcm, uint64_t offset,
                           uint64_t length, uint32_t prot,
                           struct audio_pcm_mmap *mapping)
{
    if (!pcm || !mapping || !length ||
        (prot & ~(AUDIO_PCM_PROT_READ | AUDIO_PCM_PROT_WRITE))) return -EINVAL;
    enum audio_pcm_mmap_region region;
    switch (offset) {
    case AUDIO_PCM_MMAP_OFFSET_DATA:
        region = AUDIO_PCM_MMAP_DATA;
        break;
    case AUDIO_PCM_MMAP_OFFSET_STATUS_OLD:
    case AUDIO_PCM_MMAP_OFFSET_STATUS_NEW:
        region = AUDIO_PCM_MMAP_STATUS;
        break;
    case AUDIO_PCM_MMAP_OFFSET_CONTROL_OLD:
    case AUDIO_PCM_MMAP_OFFSET_CONTROL_NEW:
        region = AUDIO_PCM_MMAP_CONTROL;
        break;
    default:
        return -EINVAL;
    }
    if ((length & (AUDIO_PCM_MMAP_PAGE_BYTES - 1u)) != 0) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (region == AUDIO_PCM_MMAP_DATA && !pcm->dma.bytes) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EBADFD;
    }
    int ret = 0;
    if (region == AUDIO_PCM_MMAP_DATA) {
        if (length > pcm->dma.bytes ||
            ((uint64_t)(uintptr_t)audio_pcm_buffer(pcm) & (AUDIO_PCM_MMAP_PAGE_BYTES - 1u)) ||
            (pcm->direction == AUDIO_CAPTURE && !(prot & AUDIO_PCM_PROT_READ)) ||
            (pcm->direction == AUDIO_PLAYBACK && !(prot & AUDIO_PCM_PROT_WRITE))) {
            ret = -EINVAL;
        }
    } else if (region == AUDIO_PCM_MMAP_STATUS) {
        if (length > AUDIO_PCM_MMAP_PAGE_BYTES || !(prot & AUDIO_PCM_PROT_READ) ||
            (prot & AUDIO_PCM_PROT_WRITE)) {
            ret = (prot & AUDIO_PCM_PROT_WRITE) ? -EPERM : -EINVAL;
        }
    } else if (length > AUDIO_PCM_MMAP_PAGE_BYTES ||
               !(prot & AUDIO_PCM_PROT_READ)) {
        ret = -EINVAL;
    }
    if (!ret) {
        ++pcm->mapping_refs;
        if (region == AUDIO_PCM_MMAP_DATA) ++pcm->data_refs;
        if (region == AUDIO_PCM_MMAP_CONTROL) ++pcm->control_refs;
        *mapping = (struct audio_pcm_mmap){
            .pcm = pcm,
            .region = region,
            .generation = pcm->generation,
            .prot = prot,
            .offset = offset,
            .length = length,
            .backing = region == AUDIO_PCM_MMAP_DATA
                ? (uint64_t)(uintptr_t)audio_pcm_buffer(pcm)
                : (uint64_t)(uintptr_t)(region == AUDIO_PCM_MMAP_STATUS
                    ? pcm->status_page : pcm->control_page),
        };
    }
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Retain the backing owned by an existing VMA, without reconfiguring it.
 * @param pcm Borrowed PCM whose existing mapping keeps its backing live.
 * @param region Region recorded in that VMA; unchanged by protection or splits.
 * @return Zero or -EINVAL for an invalid reference. Task context, IRQ-safe lock.
 * The new reference must be released on child exit or removal of the split VMA.
 */
int audio_pcm_mmap_retain(struct audio_pcm *pcm, enum audio_pcm_mmap_region region)
{
    if (!pcm || region > AUDIO_PCM_MMAP_CONTROL) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live || !pcm->mapping_refs) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EINVAL;
    }
    ++pcm->mapping_refs;
        if (region == AUDIO_PCM_MMAP_DATA) ++pcm->data_refs;
    if (region == AUDIO_PCM_MMAP_CONTROL) ++pcm->control_refs;
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Release one PCM mapping reference retained by audio_pcm_mmap_acquire. */
void audio_pcm_mmap_release(struct audio_pcm *pcm, enum audio_pcm_mmap_region region)
{
    if (!pcm) return;
    bool retire = false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (pcm->mapping_refs) --pcm->mapping_refs;
    if (region == AUDIO_PCM_MMAP_DATA && pcm->data_refs) --pcm->data_refs;
    if (region == AUDIO_PCM_MMAP_CONTROL && pcm->control_refs) --pcm->control_refs;
    if (!pcm->mapping_refs && !pcm->file_refs && !pcm->close_pending) {
        pcm->live = 0;
        retire = true;
    }
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    if (retire) audio_pcm_free_buffer(pcm);
}

/** @brief Select a core-owned DMA ring in OPEN/SETUP/PREPARED.
 * @param pcm Live OFD-owned stream; serialized in task context.
 * @param selection Borrowed validated hardware and memory-format configuration.
 * @return Zero, -EBADFD for state/data mappings, or validation/device errno.
 * The caller retains ownership; IRQ-safe locks protect published counters.
 */
static int audio_pcm_hw_params_selected(struct audio_pcm *pcm,
                                         const struct audio_params_ext *selection)
{
    if (!pcm || !selection) return -EINVAL;
    const struct audio_params *params = &selection->pcm;
    enum audio_pcm_state state;
    int ret = audio_pcm_live_state(pcm, &state);
    if (ret) return ret;
    uint64_t mapping_flags;
    kernel_spin_lock_irqsave(&pcm->lock, &mapping_flags);
    uint32_t mapping_refs = pcm->data_refs;
    kernel_spin_unlock_irqrestore(&pcm->lock, mapping_flags);
    if (mapping_refs) return -EBADFD;
    if (state != AUDIO_PCM_OPEN && state != AUDIO_PCM_SETUP && state != AUDIO_PCM_PREPARED)
        return -EBADFD;
    uint32_t bytes;
    ret = audio_pcm_validate(pcm, params, selection->format, &bytes);
    if (ret) return ret;
    uint32_t period_bytes = params->period_frames * params->frame_bytes;
    uint32_t periods = params->buffer_frames / params->period_frames;
    const struct audio_format_caps *caps = &pcm->format_caps;
    if ((caps->format_step_bytes && period_bytes % caps->format_step_bytes) ||
        (caps->period_count_min && periods < caps->period_count_min) ||
        (caps->period_count_max && periods > caps->period_count_max)) return -EINVAL;
    void *new_storage = NULL;
    uint8_t *buffer = NULL;
    ret = audio_pcm_prepare_buffer(pcm, bytes, &new_storage, &buffer);
    if (ret) return ret;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        if (new_storage && kernel_free) kernel_free(new_storage);
        return -ENODEV;
    }
    if (new_storage) {
        pcm->extended_storage = new_storage;
        pcm->extended_buffer = buffer;
    }
    pcm->params = *params;
    pcm->selected = *selection;
    pcm->dma = (struct audio_dma){.kernel = buffer,
                                  .bus = (uint64_t)(uintptr_t)buffer,
                                  .bytes = bytes};
    pcm->boundary = AUDIO_PCM_BOUNDARY_BASE - (AUDIO_PCM_BOUNDARY_BASE % params->buffer_frames);
    pcm->avail_min = params->period_frames;
    pcm->start_threshold = params->period_frames;
    pcm->stop_threshold = params->buffer_frames;
    ++pcm->generation;
    pcm->file_refs = 1;
    audio_pcm_reset_locked(pcm);
    pcm->state = AUDIO_PCM_SETUP;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Select the legacy signed sample format by its precision.
 * @param pcm Borrowed OFD-owned PCM, serialized in task context.
 * @param params Borrowed hardware geometry and precision.
 * @return Zero or state/geometry errno. No ownership transfer; IRQ-safe commit.
 */
int audio_pcm_hw_params(struct audio_pcm *pcm, const struct audio_params *params)
{
    if (!params) return -EINVAL;
    struct audio_params_ext selection = {.pcm = *params,
        .format = (uint32_t)audio_pcm_format_bit(params->sample_bits),
        .subformat = AUDIO_SUBFORMAT_STD, .significant_bits = params->sample_bits};
    return audio_pcm_hw_params_selected(pcm, &selection);
}

/** @brief Query the actual selected-format and DMA geometry constraints.
 * @param pcm Borrowed live OFD-owned PCM, serialized in task context.
 * @param caps Receives a copy; caller owns the output storage.
 * @return Zero or -EINVAL/-ENODEV; IRQ-safe snapshot, no module callback.
 */
int audio_pcm_format_caps(struct audio_pcm *pcm, struct audio_format_caps *caps)
{
    if (!pcm || !caps) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    int ret = pcm->live ? 0 : -ENODEV;
    if (!ret) *caps = pcm->format_caps;
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Commit an explicit ALSA/OSS memory format and subformat.
 * @param pcm Borrowed OFD-owned PCM, serialized in task context.
 * @param selection Borrowed exact format, precision and DMA geometry.
 * @return Zero or -EINVAL/state/device errno. No ownership transfer.
 * Hardware preparation occurs later; IRQ-safe commit never invokes the driver.
 */
int audio_pcm_hw_params_format(struct audio_pcm *pcm,
                                const struct audio_params_ext *selection)
{
    if (!pcm || !selection || selection->subformat > AUDIO_SUBFORMAT_MSBITS_MAX)
        return -EINVAL;
    uint32_t allowed = 0;
    for (unsigned i = 0; i < 6; ++i)
        if (pcm->format_caps.format_bits[i] == selection->format)
            allowed |= pcm->format_caps.subformats[i];
    if (!(allowed & (1u << selection->subformat))) return -EINVAL;
    return audio_pcm_hw_params_selected(pcm, selection);
}

/** @brief Release a stopped PCM configuration while retaining its open lease.
 * @param pcm OFD-owned stream, serialized in task context; must be non-NULL.
 * @return Zero, -EBADFD unless SETUP/PREPARED without data mappings, or errno.
 * Hardware STOP precedes clearing the core ring; no backing page is freed.
 * A concurrent fatal disconnect returns -ENODEV even if STOP reports an error.
 */
int audio_pcm_hw_free(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (pcm->data_refs || (pcm->state != AUDIO_PCM_SETUP &&
                              pcm->state != AUDIO_PCM_PREPARED)) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EBADFD;
    }
    enum audio_pcm_state state = pcm->state;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    int ret = state == AUDIO_PCM_PREPARED
        ? audio_stream_trigger(pcm->stream, AUDIO_STOP) : 0;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (ret) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return ret;
    }
    audio_pcm_reset_locked(pcm);
    pcm->params = (struct audio_params){0};
    pcm->dma = (struct audio_dma){0};
    pcm->avail_min = 0;
    pcm->state = AUDIO_PCM_OPEN;
    ++pcm->generation;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Return the Linux identity of the OFD-owned PCM substream.
 * @param pcm Live core object; borrowed and serialized by task execution.
 * @param info Writable kernel-owned ALSA identity output.
 * @return Zero or -EINVAL/-ENODEV. No ownership transfer or user dereference.
 */
int audio_pcm_info(struct audio_pcm *pcm, struct snd_pcm_info *info)
{
    if (!pcm || !info) return -EINVAL;
    *info = (struct snd_pcm_info){.device = pcm->device, .stream = pcm->direction};
    return audio_card_pcm_info(pcm->card, info);
}

/** @brief Commit validated Linux software thresholds and clock selection.
 * @param pcm Configured live object borrowed by the OFD in task context.
 * @param params In/out kernel copy; boundary is returned on success.
 * @return Zero or -EINVAL/-EBADFD/-ENODEV. Validate before mutation under
 * the IRQ-safe PCM lock; caller retains all storage and reference ownership.
 */
int audio_pcm_sw_params(struct audio_pcm *pcm, struct snd_pcm_sw_params *params)
{
    if (!pcm || !params) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    int ret = !pcm->live || pcm->state == AUDIO_PCM_DISCONNECTED ? -ENODEV :
              pcm->state == AUDIO_PCM_OPEN ? -EBADFD : 0;
    if (!ret && (params->tstamp_mode < 0 || params->tstamp_mode > SNDRV_PCM_TSTAMP_LAST ||
        (params->proto >= SNDRV_PCM_VERSION && params->tstamp_type > SNDRV_PCM_TSTAMP_TYPE_LAST) ||
        !params->avail_min ||
        (params->silence_size >= pcm->boundary ? params->silence_threshold != 0 :
         params->silence_size > params->silence_threshold ||
         params->silence_threshold > pcm->params.buffer_frames))) ret = -EINVAL;
    if (!ret) {
        pcm->tstamp_mode = params->tstamp_mode;
        if (params->proto >= SNDRV_PCM_VERSION) pcm->tstamp_type = params->tstamp_type;
        pcm->avail_min = params->avail_min;
        pcm->auto_start = 1;
        pcm->start_threshold = params->start_threshold;
        pcm->stop_threshold = params->stop_threshold;
        pcm->silence_threshold = params->silence_threshold;
        pcm->silence_size = params->silence_size;
        pcm->silence_initialized = 0;
        pcm->silence_covered = 0;
        if (pcm->state == AUDIO_PCM_RUNNING) audio_pcm_silence_locked(pcm);
        params->boundary = pcm->boundary;
        audio_pcm_publish_control_locked(pcm);
        audio_pcm_publish_locked(pcm);
    }
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Select the Linux clock used by status and mmap timestamps.
 * @param pcm Borrowed live core object in serialized task context.
 * @param type Realtime, monotonic, or monotonic-raw ALSA clock selector.
 * @return Zero or -EINVAL/-ENODEV; selection changes under the IRQ-safe lock.
 */
int audio_pcm_tstamp(struct audio_pcm *pcm, int type)
{
    if (!pcm || type < 0 || type > SNDRV_PCM_TSTAMP_TYPE_LAST) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    int ret = pcm->live ? 0 : -ENODEV;
    if (!ret) pcm->tstamp_type = type;
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Report the selected interleaved mmap channel layout.
 * @param pcm Configured OFD-owned object; borrowed in task context.
 * @param info In/out kernel copy containing a zero-based channel index.
 * @return Zero, -EINVAL for invalid channel, or -EBADFD before HW_PARAMS.
 */
int audio_pcm_channel_info(struct audio_pcm *pcm, struct snd_pcm_channel_info *info)
{
    if (!pcm || !info) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    int ret = !pcm->live ? -ENODEV : !pcm->params.frame_bytes ? -EBADFD :
              info->channel >= pcm->params.channels ? -EINVAL : 0;
    if (!ret) {
        info->offset = 0;
        info->first = info->channel * (pcm->params.frame_bytes / pcm->params.channels) * 8u;
        info->step = pcm->params.frame_bytes * 8u;
    }
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Refresh progress and enforce Linux HWSYNC state errors.
 * @param pcm Borrowed live object; no reference transfer in task context.
 * @return Zero, -EPIPE for XRUN, -EBADFD for OPEN/SETUP, or device errno.
 * Running/draining streams query the pinned driver outside the PCM lock, then
 * recheck connection/state before publishing; no timer periods are invented.
 */
int audio_pcm_hwsync(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    int ret = audio_pcm_event(pcm);
    if (ret < 0) return ret;
    enum audio_pcm_state state;
    ret = audio_pcm_live_state(pcm, &state);
    if (ret) return ret;
    if (state == AUDIO_PCM_XRUN) return -EPIPE;
    if (state == AUDIO_PCM_PREPARED || state == AUDIO_PCM_PAUSED) return 0;
    if (state != AUDIO_PCM_RUNNING &&
        (state != AUDIO_PCM_DRAINING || pcm->direction != AUDIO_PLAYBACK))
        return -EBADFD;
    uint64_t frames = 0;
    ret = audio_stream_pointer(pcm->stream, &frames);
    uint64_t flags;
    int stop = 0;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) ret = -ENODEV;
    else if (pcm->state == AUDIO_PCM_XRUN) ret = pcm->error ? pcm->error : -EPIPE;
    else if (ret < 0) {
        pcm->error = ret;
        pcm->state = ret == -ENODEV ? AUDIO_PCM_DISCONNECTED : AUDIO_PCM_XRUN;
        stop = ret != -ENODEV;
    } else {
        ret = audio_pcm_import_control_locked(pcm);
        if (!ret) {
            audio_pcm_progress_locked(pcm, frames);
            if (pcm->state == AUDIO_PCM_RUNNING) audio_pcm_silence_locked(pcm);
        }
    }
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    if (stop) {
        (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
        int connected = audio_pcm_live_state(pcm, &state);
        if (connected) return connected;
    }
    return ret;
}

/** @brief Move the application pointer by bounded committed/available frames.
 * @param pcm Borrowed configured object serialized in task context.
 * @param frames Requested frame count; zero is a successful no-op in any state.
 * @param forward Nonzero to advance, zero to rewind.
 * @return Actual moved frames or negative Linux state/device errno. Updates
 * control metadata under the IRQ-safe lock and never accesses user memory.
 */
long audio_pcm_move(struct audio_pcm *pcm, uint64_t frames, int forward)
{
    if (!pcm) return -EINVAL;
    if (!frames) return 0;
    int ret = audio_pcm_hwsync(pcm);
    if (ret) return ret;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    uint64_t queued = (pcm->appl_ptr + pcm->boundary - pcm->hw_ptr) % pcm->boundary;
    uint64_t available = (pcm->hw_ptr + pcm->boundary - pcm->appl_ptr) % pcm->boundary;
    uint64_t limit = pcm->direction == AUDIO_PLAYBACK ? queued : available;
    if (limit > pcm->params.buffer_frames) limit = 0;
    if ((pcm->direction == AUDIO_PLAYBACK && forward) ||
        (pcm->direction == AUDIO_CAPTURE && !forward)) limit = pcm->params.buffer_frames - limit;
    if (frames > limit) frames = limit;
    pcm->appl_ptr = forward ? (pcm->appl_ptr + frames) % pcm->boundary :
                             (pcm->appl_ptr + pcm->boundary - frames) % pcm->boundary;
    audio_pcm_publish_control_locked(pcm);
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return (long)frames;
}

/** @brief Prepare configured DMA without reviving a disconnected generation.
 * @param pcm Borrowed configured PCM, serialized in task context.
 * @return Zero or state/hardware errno; a concurrent disconnect returns ENODEV even on callback error.
 * XRUN recovery first proves STOP; failure retains state, pointers and DMA. Old
 * stopped-run notifications are discarded before the new pointer epoch. No PCM
 * lock spans a pinned module callback; commit rechecks under the lock.
 */
int audio_pcm_prepare(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    enum audio_pcm_state state;
    int ret = audio_pcm_live_state(pcm, &state);
    if (ret) return ret;
    if (state != AUDIO_PCM_SETUP && state != AUDIO_PCM_XRUN) return -EBADFD;
    /* The IRQ notifier may have published XRUN before task-side event polling
     * can stop the hardware. Quiesce that lease before resetting its DMA. */
    if (state == AUDIO_PCM_XRUN) {
        ret = audio_stream_trigger(pcm->stream, AUDIO_STOP);
        int connected = audio_pcm_live_state(pcm, &state);
        if (connected) return connected;
        if (ret) return ret;
    }
    /* STOP/DROP ends the old run, but its coalesced period event can still be
     * pending. Consume it before publishing the new zeroed pointer epoch. */
    uint64_t old_frames;
    int old_error;
    ret = audio_stream_period_poll(pcm->stream, &old_frames, &old_error);
    if (ret < 0) return ret;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    audio_pcm_reset_locked(pcm);
    pcm->state = AUDIO_PCM_SETUP;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    if (pcm->explicit_format)
        ret = audio_stream_prepare_format(pcm->stream, &pcm->selected, &pcm->dma);
    else
        ret = audio_stream_prepare(pcm->stream, &pcm->params, &pcm->dma);
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (ret) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return ret;
    }
    pcm->state = AUDIO_PCM_PREPARED;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Start one prepared PCM and commit only while still connected.
 * @param pcm Borrowed PCM owned by the serialized task transaction.
 * @return Zero or state/hardware errno; fatal disconnect dominates callback errors.
 */
static int audio_pcm_start_one(struct audio_pcm *pcm)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) { audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags); return -ENODEV; }
    if (pcm->state != AUDIO_PCM_PREPARED) {
        int ret = pcm->state == AUDIO_PCM_RUNNING ? -EBUSY : -EBADFD;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return ret;
    }
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    int ret = audio_stream_trigger(pcm->stream, AUDIO_START);
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (ret) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return ret;
    }
    pcm->state = AUDIO_PCM_RUNNING;
    audio_pcm_silence_locked(pcm);
    struct linux_timespec now;
    int clock = pcm->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_MONOTONIC_RAW
        ? LINUX_CLOCK_MONOTONIC_RAW : pcm->tstamp_type == SNDRV_PCM_TSTAMP_TYPE_MONOTONIC
        ? LINUX_CLOCK_MONOTONIC : LINUX_CLOCK_REALTIME;
    if (!time_clock_get(clock, &now)) pcm->trigger_tstamp = now;
    pcm->wake_seq++;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Start a PCM and its linked peer without reviving disconnected state.
 * @param pcm Borrowed master PCM, serialized in task context.
 * @return Zero or start/STOP/state errno; disconnected rollback returns ENODEV.
 * Failed rollback STOP retains DMA and publishes XRUN rather than PREPARED.
 */
int audio_pcm_start(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    int ret = audio_pcm_start_one(pcm);
    if (ret || !pcm->link) return ret;
    ret = audio_pcm_start_one(pcm->link);
    if (ret) {
        int stop = audio_stream_trigger(pcm->stream, AUDIO_STOP);
        uint64_t flags;
        kernel_spin_lock_irqsave(&pcm->lock, &flags);
        if (!audio_pcm_connected_locked(pcm)) ret = -ENODEV;
        else if (stop) {
            pcm->state = AUDIO_PCM_XRUN;
            pcm->error = stop;
            ret = stop;
        } else pcm->state = AUDIO_PCM_PREPARED;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    }
    return ret;
}

/** @brief Stop and reset a connected PCM while preserving fatal metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Zero or state/hardware errno; disconnect at commit overrides callback errors with -ENODEV.
 */
int audio_pcm_drop(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    enum audio_pcm_state state;
    int ret = audio_pcm_live_state(pcm, &state);
    if (ret) return ret;
    if (state == AUDIO_PCM_RUNNING || state == AUDIO_PCM_PAUSED ||
        state == AUDIO_PCM_DRAINING || state == AUDIO_PCM_XRUN) {
        ret = audio_stream_trigger(pcm->stream, AUDIO_STOP);
    }
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (ret) {
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return ret;
    }
    audio_pcm_reset_locked(pcm);
    pcm->state = AUDIO_PCM_SETUP;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Reset stream pointers and backing samples while preserving state. */
int audio_pcm_reset(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    if (pcm->state != AUDIO_PCM_RUNNING && pcm->state != AUDIO_PCM_PREPARED &&
        pcm->state != AUDIO_PCM_PAUSED) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EBADFD;
    }
    pcm->appl_ptr = pcm->hw_ptr;
    pcm->error = 0;
    ++pcm->wake_seq;
    audio_pcm_publish_control_locked(pcm);
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Stop a connected stream and publish an XRUN condition.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return STOP result or -ENODEV on disconnect, overriding other callback errors.
 * Disconnected metadata remains immutable.
 */
int audio_pcm_xrun(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    pcm->state = AUDIO_PCM_XRUN;
    pcm->error = -EPIPE;
    pcm->wake_seq++;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    int ret = audio_stream_trigger(pcm->stream, AUDIO_STOP);
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) ret = -ENODEV;
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Read or commit Linux SYNC_PTR fields atomically.
 * @param pcm Live OFD-owned stream in serialized task context.
 * @param flags APPL/AVAIL_MIN set request reads; clear request commits. HWSYNC polls.
 * @param appl_ptr Proposed pointer only when APPL is clear, below boundary.
 * @param avail_min Proposed wake threshold only when AVAIL_MIN is clear.
 * @param status Caller-owned writable snapshot, required.
 * @return Zero, -EINVAL for invalid flags/pointer, or device errno. No ownership
 * transfer; all validation precedes mutations under the IRQ-safe PCM lock.
 */
int audio_pcm_sync(struct audio_pcm *pcm, uint32_t flags,
                   uint64_t appl_ptr, uint64_t avail_min,
                   struct audio_pcm_status *status)
{
    if (!pcm || !status) return -EINVAL;
    if (flags & ~(1u | 2u | 4u)) return -EINVAL;
    if (flags & 1u) {
        int event = audio_pcm_hwsync(pcm);
        if (event < 0) return event;
    }
    uint64_t lock_flags;
    kernel_spin_lock_irqsave(&pcm->lock, &lock_flags);
    if (!pcm->live) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, lock_flags);
        return -ENODEV;
    }
    if (!(flags & 2u)) {
        if (appl_ptr >= pcm->boundary) {
            audio_pcm_publish_locked(pcm);
            kernel_spin_unlock_irqrestore(&pcm->lock, lock_flags);
            return -EINVAL;
        }
        pcm->appl_ptr = appl_ptr;
    }
    if (!(flags & 4u)) pcm->avail_min = avail_min;
    audio_pcm_publish_control_locked(pcm);
    audio_pcm_publish_locked(pcm);
    *status = (struct audio_pcm_status){
        .state = pcm->state,
        .direction = pcm->direction,
        .frame_bytes = pcm->params.frame_bytes,
        .buffer_frames = pcm->params.buffer_frames,
        .avail_min = pcm->avail_min,
        .boundary = pcm->boundary,
        .hw_ptr = pcm->hw_ptr,
        .appl_ptr = pcm->appl_ptr,
        .error = pcm->error,
        .tstamp = {
            .tv_sec = ((struct __snd_pcm_mmap_status *)(void *)pcm->status_page)->tstamp.tv_sec,
            .tv_nsec = ((struct __snd_pcm_mmap_status *)(void *)pcm->status_page)->tstamp.tv_nsec,
        },
        .trigger_tstamp = {
            .tv_sec = pcm->trigger_tstamp.tv_sec,
            .tv_nsec = pcm->trigger_tstamp.tv_nsec,
        },
    };
    pcm->wake_seq++;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, lock_flags);
    return 0;
}

/** @brief Pause or resume without overwriting a concurrent forced disconnect.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @param pause True for PAUSE, false for UNPAUSE.
 * @return Zero or state/hardware errno; disconnect at commit overrides callback errors with -ENODEV.
 */
int audio_pcm_pause(struct audio_pcm *pcm, bool pause)
{
    if (!pcm) return -EINVAL;
    enum audio_pcm_state state;
    int ret = audio_pcm_live_state(pcm, &state);
    if (ret) return ret;
    if (state != (pause ? AUDIO_PCM_RUNNING : AUDIO_PCM_PAUSED)) return -EBADFD;
    ret = audio_stream_trigger(pcm->stream, pause ? AUDIO_PAUSE : AUDIO_UNPAUSE);
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) ret = -ENODEV;
    else if (!ret) {
        pcm->state = pause ? AUDIO_PCM_PAUSED : AUDIO_PCM_RUNNING;
        pcm->wake_seq++;
        audio_pcm_publish_locked(pcm);
    }
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

/** @brief Silence every unsubmitted drain frame without changing appl_ptr.
 * @param pcm Locked playback PCM with validated period/buffer geometry.
 * @return None. Task context; writes only free DMA bytes, retaining the queued
 * interval even when it wraps or fills the ring. No callback/allocation.
 */
static void audio_pcm_drain_tail_locked(struct audio_pcm *pcm)
{
    uint64_t queued = (pcm->appl_ptr + pcm->boundary - pcm->hw_ptr) % pcm->boundary;
    if (queued >= pcm->params.buffer_frames) return;
    /* Cyclic DMA can pass the final period before STOP is serviced. All free
     * frames must be silent, including aligned ends and the next ring wrap. */
    uint64_t bytes = (pcm->params.buffer_frames - queued) * pcm->params.frame_bytes;
    uint64_t offset = (pcm->appl_ptr % pcm->params.buffer_frames) * pcm->params.frame_bytes;
    uint64_t ring_bytes = (uint64_t)pcm->params.buffer_frames * pcm->params.frame_bytes;
    uint8_t silence = pcm->selected.format == AUDIO_FORMAT_U8 ? 0x80u : 0u;
    uint8_t *buffer = audio_pcm_buffer(pcm);
    uint64_t first = ring_bytes - offset;
    if (first > bytes) first = bytes;
    memset(buffer + offset, silence, (size_t)first);
    if (bytes > first) memset(buffer, silence, (size_t)(bytes - first));
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

/** @brief Drain submitted playback frames, accepting the final period's overshoot.
 * @param pcm Live configured PCM; capture delegates to DROP.
 * @return 0 after safe STOP, -EAGAIN while queued, or state/hardware errno.
 * Task context; no PCM lock is held across driver START/STOP callbacks. DMA
 * tail padding does not advance appl_ptr; failed STOP keeps DRAINING retryable.
 * Fatal disconnect overrides callback errors with -ENODEV.
 */
int audio_pcm_drain(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    if (pcm->direction == AUDIO_CAPTURE) return audio_pcm_drop(pcm);
    if (pcm->state == AUDIO_PCM_PREPARED && pcm->appl_ptr != pcm->hw_ptr) {
        int start = audio_pcm_start(pcm);
        if (start) return start;
    }
    int event = audio_pcm_event(pcm);
    if (event < 0) return event;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) { audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags); return -ENODEV; }
    if (pcm->state == AUDIO_PCM_PREPARED && pcm->appl_ptr == pcm->hw_ptr) {
        pcm->state = AUDIO_PCM_SETUP;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return 0;
    }
    if (pcm->state != AUDIO_PCM_RUNNING && pcm->state != AUDIO_PCM_DRAINING) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EBADFD;
    }
    uint64_t queued = (pcm->appl_ptr + pcm->boundary - pcm->hw_ptr) % pcm->boundary;
    if (pcm->state == AUDIO_PCM_DRAINING && queued > pcm->params.buffer_frames &&
        (pcm->hw_ptr + pcm->boundary - pcm->appl_ptr) % pcm->boundary <= pcm->params.buffer_frames)
        queued = 0;
    if (queued > pcm->params.buffer_frames) {
        pcm->state = AUDIO_PCM_XRUN;
        pcm->error = -EPIPE;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
        return -EPIPE;
    }
    if (queued) {
        audio_pcm_drain_tail_locked(pcm);
        pcm->state = AUDIO_PCM_DRAINING;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -EAGAIN;
    }
    pcm->state = AUDIO_PCM_DRAINING;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    int ret = audio_stream_trigger(pcm->stream, AUDIO_STOP);
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!audio_pcm_connected_locked(pcm)) ret = -ENODEV;
    else if (!ret) {
        pcm->state = AUDIO_PCM_SETUP;
        pcm->wake_seq++;
        audio_pcm_publish_locked(pcm);
    }
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return ret;
}

int audio_pcm_link(struct audio_pcm *master, struct audio_pcm *slave)
{
    if (!master || !slave || master == slave || !master->live || !slave->live ||
        master->card != slave->card) return -EINVAL;
    struct audio_pcm *first = master < slave ? master : slave;
    struct audio_pcm *second = master < slave ? slave : master;
    uint64_t first_flags, second_flags;
    kernel_spin_lock_irqsave(&first->lock, &first_flags);
    kernel_spin_lock_irqsave(&second->lock, &second_flags);
    int ret = (master->link || slave->link) ? -EBUSY : 0;
    if (!ret) { master->link = slave; slave->link = master; }
    kernel_spin_unlock_irqrestore(&second->lock, second_flags);
    kernel_spin_unlock_irqrestore(&first->lock, first_flags);
    return ret;
}

int audio_pcm_unlink(struct audio_pcm *pcm)
{
    if (!pcm) return -EINVAL;
    struct audio_pcm *other = pcm->link;
    if (!other) return 0;
    struct audio_pcm *first = pcm < other ? pcm : other;
    struct audio_pcm *second = pcm < other ? other : pcm;
    uint64_t first_flags, second_flags;
    kernel_spin_lock_irqsave(&first->lock, &first_flags);
    kernel_spin_lock_irqsave(&second->lock, &second_flags);
    pcm->link = NULL;
    if (other->link == pcm) other->link = NULL;
    kernel_spin_unlock_irqrestore(&second->lock, second_flags);
    kernel_spin_unlock_irqrestore(&first->lock, first_flags);
    return 0;
}

static void audio_pcm_copy_in(struct audio_pcm *pcm, uint64_t offset,
                              const uint8_t *source, uint32_t bytes)
{
    uint32_t start = (uint32_t)(offset % pcm->dma.bytes);
    uint32_t first = pcm->dma.bytes - start;
    if (first > bytes) first = bytes;
    uint8_t *buffer = audio_pcm_buffer(pcm);
    memcpy(buffer + start, source, first);
    if (first != bytes) memcpy(buffer, source + first, bytes - first);
}

static void audio_pcm_copy_out(const struct audio_pcm *pcm, uint64_t offset,
                               uint8_t *target, uint32_t bytes)
{
    uint32_t start = (uint32_t)(offset % pcm->dma.bytes);
    uint32_t first = pcm->dma.bytes - start;
    if (first > bytes) first = bytes;
    const uint8_t *buffer = audio_pcm_const_buffer(pcm);
    memcpy(target, buffer + start, first);
    if (first != bytes) memcpy(target + first, buffer, bytes - first);
}

/** @brief Transfer complete frames through the configured interleaved DMA ring.
 * @param pcm Borrowed live PCM, serialized in task context; must be non-NULL.
 * @param kernel_data Kernel-owned sample storage; writable for capture, readable
 * for playback, with at least frames times frame_bytes bytes; NULL only if zero.
 * @param frames Requested number of complete frames.
 * @return Frames copied (including short progress), or negative state/driver
 * errno when no progress is made. Auto-start failure after playback progress is
 * latched for the next call; permanent disconnect metadata is never overwritten.
 * No PCM lock spans a driver callback. The caller retains sample ownership.
 */
long audio_pcm_transfer(struct audio_pcm *pcm, void *kernel_data, uint32_t frames)
{
    if (!pcm || (!kernel_data && frames)) return -EINVAL;
    if (pcm->auto_start && pcm->direction == AUDIO_CAPTURE &&
        pcm->state == AUDIO_PCM_PREPARED && frames >= pcm->start_threshold) {
        int start = audio_pcm_start(pcm);
        if (start) return start;
    }
    int event = audio_pcm_event(pcm);
    if (event == -ENODEV) return -ENODEV;
    if (event < 0) return event;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live) { audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags); return -ENODEV; }
    if (pcm->error) {
        int error = pcm->error;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return error;
    }
    if (pcm->state != AUDIO_PCM_PREPARED && pcm->state != AUDIO_PCM_RUNNING &&
        pcm->state != AUDIO_PCM_PAUSED && pcm->state != AUDIO_PCM_DRAINING) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return pcm->state == AUDIO_PCM_XRUN ? -EPIPE : -EBADFD;
    }
    uint64_t available;
    if (pcm->direction == AUDIO_PLAYBACK) {
        uint64_t queued = (pcm->appl_ptr + pcm->boundary - pcm->hw_ptr) % pcm->boundary;
        if (queued > pcm->params.buffer_frames) {
            pcm->error = -EPIPE;
            pcm->state = AUDIO_PCM_XRUN;
            audio_pcm_publish_locked(pcm);
            kernel_spin_unlock_irqrestore(&pcm->lock, flags);
            (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
            return -EPIPE;
        }
        uint32_t room = pcm->params.buffer_frames - (uint32_t)queued;
        uint32_t take = frames < room ? frames : room;
        if (!take) {
            audio_pcm_publish_locked(pcm);
            kernel_spin_unlock_irqrestore(&pcm->lock, flags); return -EAGAIN;
        }
        audio_pcm_copy_in(pcm, pcm->appl_ptr * pcm->params.frame_bytes,
                          kernel_data, take * pcm->params.frame_bytes);
        pcm->appl_ptr = (pcm->appl_ptr + take) % pcm->boundary;
        if (pcm->state == AUDIO_PCM_RUNNING) audio_pcm_silence_locked(pcm);
        audio_pcm_publish_control_locked(pcm);
        pcm->wake_seq++;
        bool start = pcm->auto_start && pcm->state == AUDIO_PCM_PREPARED &&
                     queued + take >= pcm->start_threshold;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        if (start) {
            int error = audio_pcm_start(pcm);
            if (error) {
                kernel_spin_lock_irqsave(&pcm->lock, &flags);
                if (audio_pcm_connected_locked(pcm)) pcm->error = error;
                audio_pcm_publish_locked(pcm);
                kernel_spin_unlock_irqrestore(&pcm->lock, flags);
            }
        }
        return take;
    }
    available = (pcm->hw_ptr + pcm->boundary - pcm->appl_ptr) % pcm->boundary;
    if (available > pcm->params.buffer_frames) {
        pcm->error = -EPIPE;
        pcm->state = AUDIO_PCM_XRUN;
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
        return -EPIPE;
    }
    uint32_t take = frames < available ? frames : (uint32_t)available;
    if (!take) { audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags); return -EAGAIN; }
    audio_pcm_copy_out(pcm, pcm->appl_ptr * pcm->params.frame_bytes,
                       kernel_data, take * pcm->params.frame_bytes);
    pcm->appl_ptr = (pcm->appl_ptr + take) % pcm->boundary;
    audio_pcm_publish_control_locked(pcm);
    pcm->wake_seq++;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return take;
}

int audio_pcm_status(struct audio_pcm *pcm, struct audio_pcm_status *status)
{
    if (!pcm || !status) return -EINVAL;
    int event = audio_pcm_event(pcm);
    if (event == -ENODEV) return -ENODEV;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if (!pcm->live) {
        audio_pcm_publish_locked(pcm);
        kernel_spin_unlock_irqrestore(&pcm->lock, flags);
        return -ENODEV;
    }
    audio_pcm_publish_locked(pcm);
    *status = (struct audio_pcm_status){
        .state = pcm->state,
        .direction = pcm->direction,
        .frame_bytes = pcm->params.frame_bytes,
        .buffer_frames = pcm->params.buffer_frames,
        .avail_min = pcm->avail_min,
        .boundary = pcm->boundary,
        .hw_ptr = pcm->hw_ptr,
        .appl_ptr = pcm->appl_ptr,
        .error = pcm->error,
        .tstamp = {
            .tv_sec = ((struct __snd_pcm_mmap_status *)(void *)pcm->status_page)->tstamp.tv_sec,
            .tv_nsec = ((struct __snd_pcm_mmap_status *)(void *)pcm->status_page)->tstamp.tv_nsec,
        },
        .trigger_tstamp = {
            .tv_sec = pcm->trigger_tstamp.tv_sec,
            .tv_nsec = pcm->trigger_tstamp.tv_nsec,
        },
    };
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    return 0;
}

/** @brief Drop the final file reference while retaining DMA on failed STOP.
 * @param pcm Borrowed live PCM owned by the closing OFD; may be NULL.
 * @return None. Task context. Failed hardware close retains the core lease and
 * backing even after the last munmap; manager task service retries retirement.
 * A fatal DISCONNECTED state and its ENODEV error survive failed close callbacks.
 */
void audio_pcm_release(struct audio_pcm *pcm)
{
    if (!pcm) return;
    (void)audio_pcm_unlink(pcm);
    enum audio_pcm_state state;
    if (!audio_pcm_live_state(pcm, &state) &&
        (state == AUDIO_PCM_RUNNING || state == AUDIO_PCM_PAUSED ||
         state == AUDIO_PCM_DRAINING || state == AUDIO_PCM_XRUN))
        (void)audio_stream_trigger(pcm->stream, AUDIO_STOP);
    int ret=audio_stream_close(pcm->stream);
    bool retire = false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&pcm->lock, &flags);
    if(ret){
        if(!pcm->close_pending)__atomic_add_fetch(&pending_releases,1,__ATOMIC_RELEASE);
        pcm->close_pending=1;
        pcm->error=pcm->state==AUDIO_PCM_DISCONNECTED ? -ENODEV : ret;
        pcm->state=ret==-ENODEV || pcm->state==AUDIO_PCM_DISCONNECTED ?
            AUDIO_PCM_DISCONNECTED : AUDIO_PCM_XRUN;
    }else{
        if(pcm->close_pending)__atomic_sub_fetch(&pending_releases,1,__ATOMIC_RELEASE);
        pcm->close_pending=0;
        if(pcm->state!=AUDIO_PCM_DISCONNECTED)pcm->state=AUDIO_PCM_OPEN;
    }
    pcm->file_refs = 0;
    pcm->live = pcm->mapping_refs || pcm->close_pending ? 1 : 0;
    retire = !pcm->live;
    audio_pcm_publish_locked(pcm);
    kernel_spin_unlock_irqrestore(&pcm->lock, flags);
    if (retire) audio_pcm_free_buffer(pcm);
}
/** @brief Inspect whether final OFD releases retained an unsafe hardware lease.
 * @return True while task retirement is needed. IRQ safe, no module callback.
 */
bool audio_pcm_release_work_pending(void)
{
    return __atomic_load_n(&pending_releases,__ATOMIC_ACQUIRE)!=0;
}
/** @brief Retry retained final releases with module images protected by manager admission.
 * @param budget Maximum PCM close attempts in this task phase.
 * @return Attempt count. Task context without execution ownership. A successful
 * close marks retirement ready; backing remains reserved until the manager
 * reacquires execution ownership and publishes retirement.
 */
uint32_t audio_pcm_retry_releases(uint32_t budget)
{
    uint32_t work=0;uint64_t flags;
    for(uint32_t i=0;i<AUDIO_PCM_MAX && work<budget;++i){
        struct audio_pcm *pcm=&pcm_slots[i];
        kernel_spin_lock_irqsave(&pcm->lock,&flags);
        bool run=pcm->live && !pcm->file_refs && pcm->close_pending==1;
        if(run)pcm->close_pending=2;
        kernel_spin_unlock_irqrestore(&pcm->lock,flags);
        if(!run)continue;
        int ret=audio_stream_close(pcm->stream);++work;
        kernel_spin_lock_irqsave(&pcm->lock,&flags);
        pcm->close_pending=ret ? 1 : 3;
        kernel_spin_unlock_irqrestore(&pcm->lock,flags);
    }
    return work;
}
/** @brief Publish completed retirements after manager execution ownership resumes.
 * @return None. Task context under execution ownership. Serializing with opens
 * prevents a new PCM from reinitializing a slot while its old lock is held.
 * VMA releases keep the slot reserved while close_pending is nonzero.
 */
void audio_pcm_finish_releases(void)
{
    uint64_t flags;
    for(uint32_t i=0;i<AUDIO_PCM_MAX;++i){
        struct audio_pcm *pcm=&pcm_slots[i];
        bool retire = false;
        kernel_spin_lock_irqsave(&pcm->lock,&flags);
        if(pcm->close_pending==3){
            pcm->close_pending=0;
            __atomic_sub_fetch(&pending_releases,1,__ATOMIC_RELEASE);
            pcm->live=pcm->mapping_refs ? 1 : 0;
            audio_pcm_publish_locked(pcm);
            retire = !pcm->live;
        }
        kernel_spin_unlock_irqrestore(&pcm->lock,flags);
        if (retire) audio_pcm_free_buffer(pcm);
    }
}
