#include "pcm_internal.h"

static uint32_t audio_pcm_container_bytes(uint32_t sample_bits);

static uint64_t audio_pcm_format_bit(uint32_t sample_bits);

static uint64_t audio_pcm_rate_bit(uint32_t rate);

static int audio_pcm_validate(const struct audio_pcm *pcm, const struct audio_params *params,
                             uint64_t format, uint32_t *bytes);

/** @brief Select a core-owned DMA ring in OPEN/SETUP/PREPARED.
 * @param pcm Live OFD-owned stream; serialized in task context.
 * @param selection Borrowed validated hardware and memory-format configuration.
 * @return Zero, -EBADFD for state/data mappings, or validation/device errno.
 * The caller retains ownership; IRQ-safe locks protect published counters.
 */
static int audio_pcm_hw_params_selected(struct audio_pcm *pcm,
                                         const struct audio_params_ext *selection);


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
