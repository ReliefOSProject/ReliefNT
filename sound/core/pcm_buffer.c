#include "pcm_internal.h"

/** @brief Borrow the read-only CPU alias for a retained PCM ring.
 * @param pcm Caller-retained PCM, serialized against reconfiguration.
 * @return Borrowed allocation alias, nullable before HW_PARAMS; no locks.
 */
static const uint8_t *audio_pcm_const_buffer(const struct audio_pcm *pcm);


/** @brief Return the CPU alias for the active PCM ring.
 * @param pcm Live PCM object; caller serializes against reconfiguration.
 * @return Borrowed CPU pointer to the explicitly allocated ring, never a
 *         DMA bus address or a physical address.
 */
uint8_t *audio_pcm_buffer(struct audio_pcm *pcm)
{
    return pcm->extended_buffer;
}

/** @brief Borrow a PCM ring without translating its CPU address to a bus PFN.
 * @param pcm Stable caller-retained PCM.
 * @return Borrowed alias, NULL before allocation; no ownership or locks changed.
 */
static const uint8_t *audio_pcm_const_buffer(const struct audio_pcm *pcm)
{
    return pcm->extended_buffer;
}

/** @brief Retire a PCM backing allocation and its provider reference.
 * @param pcm Retired PCM object with no file or mmap references.
 * @return None. Clears both aliases before releasing backing pages and the
 * provider. Final file/VMA and failed-STOP leases must already have retired.
 * Task context; no PCM lock may be held while the kernel heap is called.
 */
void audio_pcm_free_buffer(struct audio_pcm *pcm)
{
    void *storage = pcm->extended_storage;
    pcm->extended_storage = NULL;
    pcm->extended_buffer = NULL;
    kernel_free(storage);
    void *metadata=pcm->metadata_storage;
    pcm->metadata_storage=NULL;pcm->status_page=NULL;pcm->control_page=NULL;
    kernel_free(metadata);
    struct module *owner=pcm->code_owner;
    pcm->code_owner=NULL;
    module_put(owner);
}

/** @brief Select or allocate an aligned CPU alias for the requested PCM ring.
 * @param pcm Live PCM object; caller serializes against reconfiguration.
 * @param bytes Validated ring length, no larger than AUDIO_PCM_BUFFER_BYTES.
 * @param new_storage Receives a new allocation owner, or NULL when reusing an
 *                    an existing allocated ring.
 * @param new_buffer Receives the writable CPU alias; it is not a DMA address.
 * @return Zero, EINVAL for invalid geometry, or ENOMEM on allocation failure.
 * Task context. On success with non-NULL new_storage, caller must either
 * publish both outputs together or free the new owner on a later failure.
 */
int audio_pcm_prepare_buffer(struct audio_pcm *pcm, uint32_t bytes,
                             void **new_storage, uint8_t **new_buffer)
{
    *new_storage = NULL;
    *new_buffer = audio_pcm_buffer(pcm);
    if (!bytes || bytes>AUDIO_PCM_BUFFER_BYTES)return -EINVAL;
    if (pcm->extended_buffer) {
        return 0;
    }
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
void audio_pcm_silence_locked(struct audio_pcm *pcm)
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
    __builtin_memset(buffer + offset, sample, first);
    if (bytes > first) __builtin_memset(buffer, sample, bytes - first);
    pcm->silence_covered += frames;
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void audio_pcm_copy_in(struct audio_pcm *pcm, uint64_t offset,
                              const uint8_t *source, uint32_t bytes)
{
    uint32_t start = (uint32_t)(offset % pcm->dma.bytes);
    uint32_t first = pcm->dma.bytes - start;
    if (first > bytes) first = bytes;
    uint8_t *buffer = audio_pcm_buffer(pcm);
    __builtin_memcpy(buffer + start, source, first);
    if (first != bytes) __builtin_memcpy(buffer, source + first, bytes - first);
}

void audio_pcm_copy_out(const struct audio_pcm *pcm, uint64_t offset,
                               uint8_t *target, uint32_t bytes)
{
    uint32_t start = (uint32_t)(offset % pcm->dma.bytes);
    uint32_t first = pcm->dma.bytes - start;
    if (first > bytes) first = bytes;
    const uint8_t *buffer = audio_pcm_const_buffer(pcm);
    __builtin_memcpy(target, buffer + start, first);
    if (first != bytes) __builtin_memcpy(target + first, buffer, bytes - first);
}
