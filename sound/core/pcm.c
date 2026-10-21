#include "pcm_internal.h"

/** @brief Start one prepared PCM and commit only while still connected.
 * @param pcm Borrowed PCM owned by the serialized task transaction.
 * @return Zero or state/hardware errno; fatal disconnect dominates callback errors.
 */
static int audio_pcm_start_one(struct audio_pcm *pcm);

/** @brief Silence every unsubmitted drain frame without changing appl_ptr.
 * @param pcm Locked playback PCM with validated period/buffer geometry.
 * @return None. Task context; writes only free DMA bytes, retaining the queued
 * interval even when it wraps or fills the ring. No callback/allocation.
 */
static void audio_pcm_drain_tail_locked(struct audio_pcm *pcm);

struct audio_pcm pcm_slots[AUDIO_PCM_MAX];

static uint32_t pending_releases;
void audio_pcm_reset_locked(struct audio_pcm *pcm)
{
    pcm->hw_ptr = 0;
    pcm->driver_frames = 0;
    pcm->appl_ptr = 0;
    pcm->trigger_tstamp = (struct linux_timespec){0};
    pcm->silence_initialized = 0;
    pcm->silence_covered = 0;
    pcm->error = 0;
    pcm->wake_seq++;
    __builtin_memset(audio_pcm_buffer(pcm), pcm->selected.format == AUDIO_FORMAT_U8 ? 0x80 : 0,
           pcm->dma.bytes);
    audio_pcm_publish_control_locked(pcm);
}

/** @brief Test whether an operation may commit to this PCM generation.
 * @param pcm Borrowed PCM under its IRQ-safe lock, in task or IRQ context.
 * @return True while live and connected; forced disconnect is irreversible.
 */
bool audio_pcm_connected_locked(const struct audio_pcm *pcm)
{
    return pcm->live && pcm->state != AUDIO_PCM_DISCONNECTED;
}

/** @brief Consume coalesced progress without changing disconnected metadata.
 * @param pcm Borrowed live PCM, serialized in task context.
 * @return Event count, XRUN/input errno, or ENODEV after permanent disconnect.
 * No PCM lock spans registry polling or a pinned hardware STOP callback.
 */
int audio_pcm_event(struct audio_pcm *pcm)
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
int audio_pcm_live_state(struct audio_pcm *pcm, enum audio_pcm_state *state)
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
    __builtin_memset(buffer + offset, silence, (size_t)first);
    if (bytes > first) __builtin_memset(buffer, silence, (size_t)(bytes - first));
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
