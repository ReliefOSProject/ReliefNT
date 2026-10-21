#include "pcm_internal.h"



/** @brief Publish only trusted state/counters to the user-readable metadata page.
 * @param pcm Live core object under its IRQ-safe lock; the page is core-owned.
 * @return None. Task/IRQ safe; release stores order DMA-visible progress.
 */
void audio_pcm_publish_locked(struct audio_pcm *pcm)
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
void audio_pcm_publish_control_locked(struct audio_pcm *pcm)
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
int audio_pcm_import_control_locked(struct audio_pcm *pcm)
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
