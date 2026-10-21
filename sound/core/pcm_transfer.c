#include "pcm_internal.h"



/** @brief Import forward progress without letting a delayed IRQ rewind HWSYNC.
 * @param pcm Live configured PCM under its IRQ-safe lock.
 * @param frames Cumulative driver frames in the current prepare epoch.
 * @return None. Compare cumulative frames before wrapping the public pointer.
 */
void audio_pcm_progress_locked(struct audio_pcm *pcm, uint64_t frames)
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
void audio_pcm_notify(uint32_t stream, uint64_t frames, int error)
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
