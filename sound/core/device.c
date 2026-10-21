#include <limits.h>
#include <linux/poll.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/errno.h>
#include <sound/asound.h>
#include <reliefnt/audio.h>
#include <reliefnt/futex.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/usercopy.h>

#define AUDIO_DEVICE_MAX 64u

struct audio_file {
    struct audio_pcm *pcm;
    uint32_t card;
    uint32_t device;
    enum audio_direction direction;
    uint32_t user_pversion;
};

static struct audio_file audio_files[AUDIO_DEVICE_MAX];
static struct audio_file *audio_device_file(const struct task_file *file);

/**
 * @brief Identify a task file backed by a standard PCM device node.
 * @param file Candidate task file; may be NULL.
 * @return Non-zero only for an opened standard PCM node.
 */
static int audio_device_is(const struct task_file *file)
{
    return file && (file->flags & TASK_FILE_FLAG_DEV_NODE) &&
           file->node.first_cluster == STORAGE_DEV_KIND_AUDIO;
}

/**
 * @brief Parse one bounded decimal component of a PCM node name.
 * @param cursor In/out cursor at the first digit; must be non-NULL.
 * @param value Output value supplied by the caller.
 * @param maximum Inclusive component limit.
 * @return Zero on success, or -EINVAL for missing, overflowing, or excessive digits.
 */
static int audio_device_parse_decimal(const char **cursor, uint32_t *value,
                                      uint32_t maximum)
{
    const char *p = *cursor;
    uint32_t result = 0;
    if (!p || *p < '0' || *p > '9') return -EINVAL;
    do {
        uint32_t digit = (uint32_t)(*p++ - '0');
        if (digit > maximum || result > (maximum - digit) / 10u) return -EINVAL;
        result = result * 10u + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = p;
    *value = result;
    return 0;
}

/**
 * @brief Parse Linux-style pcmC<N>D<M>{p,c} node names.
 * @param name NUL-terminated basename; trailing text is rejected.
 * @param card Optional output card ordinal.
 * @param device Optional output device index in the range 0..7.
 * @param direction Optional output playback or capture direction.
 * @return Zero on success, or -EINVAL for malformed/overflowing names.
 */
int audio_device_parse_name(const char *name, uint32_t *card, uint32_t *device,
                            enum audio_direction *direction)
{
    const char *p = name;
    uint32_t parsed_card, parsed_device;
    if (!p || p[0] != 'p' || p[1] != 'c' || p[2] != 'm' || p[3] != 'C')
        return -EINVAL;
    p += 4;
    if (audio_device_parse_decimal(&p, &parsed_card, 0x00ffffffu) < 0 ||
        *p++ != 'D' || audio_device_parse_decimal(&p, &parsed_device, 7u) < 0 ||
        (p[0] != 'p' && p[0] != 'c') || p[1] != 0) return -EINVAL;
    if (card) *card = parsed_card;
    if (device) *device = parsed_device;
    if (direction) *direction = p[0] == 'p' ? AUDIO_PLAYBACK : AUDIO_CAPTURE;
    return 0;
}

/**
 * @brief Reserve one bounded open-file audio state slot.
 * @return A zeroed free slot, or NULL when the table is exhausted.
 */
static struct audio_file *audio_device_slot(void)
{
    for (uint32_t i = 0; i < AUDIO_DEVICE_MAX; ++i)
        if (!audio_files[i].pcm) return &audio_files[i];
    return NULL;
}

/**
 * @brief Acquire an unconfigured PCM before publishing a standard descriptor.
 * @param task Calling task; currently unused by the core adapter.
 * @param file Descriptor/OFD storage containing the synthetic node identity.
 * @return Zero with the stream in OPEN, or a negative Linux errno. Task context;
 * execution ownership serializes the pool. The OFD owns the core reference.
 */
int audio_device_open(struct task *task, struct task_file *file)
{
    (void)task;
    if (!audio_device_is(file) || file->audio_file) return -EINVAL;
    uint32_t card = AUDIO_DEVICE_CARD(file->node.volume_id);
    uint32_t device = AUDIO_DEVICE_INDEX(file->node.volume_id);
    enum audio_direction direction = AUDIO_DEVICE_DIRECTION(file->node.volume_id);
    if ((uint32_t)direction > AUDIO_CAPTURE) return -EINVAL;
    struct audio_file *audio = audio_device_slot();
    if (!audio) return -ENOSPC;
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card, device, direction, &error);
    if (!pcm) return error;
    *audio = (struct audio_file){
        .pcm = pcm,
        .card = card,
        .device = device,
        .direction = direction,
    };
    file->audio_file = audio;
    return 0;
}

/**
 * @brief Validate a standard PCM mmap and return its core mapping metadata.
 * @param task Calling task; reserved for future access-mode checks.
 * @param file Open PCM description.
 * @param offset Linux mmap offset selector.
 * @param length Page-aligned requested mapping length.
 * @param prot Linux PROT_READ/PROT_WRITE bits.
 * @param mapping Writable mapping metadata output.
 * @return Zero for the data ring, or a negative Linux errno.
 */
int audio_device_mmap(struct task *task, struct task_file *file,
                      uint64_t offset, uint64_t length, uint32_t prot,
                      struct audio_pcm_mmap *mapping)
{
    (void)task;
    struct audio_file *audio = audio_device_file(file);
    if (!audio || !mapping) return -LINUX_EBADF;
    int ret = audio_pcm_mmap_acquire(audio->pcm, offset, length, prot, mapping);
    if (ret < 0) return ret;
    return 0;
}

/**
 * @brief Release the PCM reference owned by the final open-file description.
 * @param file Description whose private audio state should be released.
 * @return Zero; NULL or already-closed descriptions are harmless.
 */
int audio_device_close(struct task_file *file)
{
    if (!file || !file->audio_file) return 0;
    struct audio_file *audio = file->audio_file;
    file->audio_file = NULL;
    audio_pcm_release(audio->pcm);
    *audio = (struct audio_file){0};
    return 0;
}

/**
 * @brief Resolve the private audio state from a standard PCM description.
 * @param file Descriptor or promoted open-file description; may be NULL.
 * @return Private state, or NULL for a non-audio/closed file.
 */
static struct audio_file *audio_device_file(const struct task_file *file)
{
    if (!audio_device_is(file)) return NULL;
    return file->audio_file;
}

/**
 * @brief Transfer whole PCM frames while preserving Linux short-I/O semantics.
 * @param task Calling task; may be NULL for a kernel-side nonblocking probe.
 * @param file Open PCM description.
 * @param buffer Kernel-accessible transfer buffer; may be NULL only for zero count.
 * @param count Requested byte count; a trailing partial frame is not submitted.
 * @param writing Non-zero for playback, zero for capture.
 * @return Bytes transferred, or a negative Linux errno. Task context under
 * execution ownership; the caller retains the OFD and validates user pages.
 */
static int audio_device_transfer(struct task *task, struct task_file *file,
                                 void *buffer, uint32_t count, int writing)
{
    struct audio_file *audio = audio_device_file(file);
    if (!audio || (!buffer && count)) return -LINUX_EBADF;
    if (!count) return 0;
    if (audio->direction != (writing ? AUDIO_PLAYBACK : AUDIO_CAPTURE)) return -LINUX_EBADF;
    struct audio_pcm_status status;
    int error = audio_pcm_status(audio->pcm, &status);
    if (error) return error;
    if (!status.frame_bytes || status.state == AUDIO_PCM_OPEN) return -LINUX_EBADFD;
    uint32_t frames = count / status.frame_bytes;
    if (!frames) return -LINUX_EINVAL;
    if (frames > INT_MAX / status.frame_bytes) return -LINUX_EINVAL;
    if (task) audio_pcm_wait_remove(task);
    uint32_t epoch = audio_pcm_event_epoch();
    uint32_t done = 0;
    uint32_t chunk_max = 4096u / status.frame_bytes;
    if (!chunk_max) return -LINUX_EINVAL;
    for (;;) {
        uint32_t chunk = frames - done;
        if (chunk > chunk_max) chunk = chunk_max;
        long ret = audio_pcm_transfer(audio->pcm,
            (uint8_t *)buffer + (uint64_t)done * status.frame_bytes, chunk);
        if (ret > 0) {
            done += (uint32_t)ret;
            if (done == frames || (uint32_t)ret < chunk)
                return (int)(done * status.frame_bytes);
            continue;
        }
        if (done) return (int)(done * status.frame_bytes);
        if (ret != -LINUX_EAGAIN || !task || !task->pid ||
            (file->flags & RELIEFOS_O_NONBLOCK)) return (int)ret;
        audio_pcm_wait_current();
        if (audio_pcm_event_epoch() != epoch) {
            audio_pcm_wait_remove(task);
            sched_wake_interruptible(task);
            epoch = audio_pcm_event_epoch();
            continue;
        }
        if (task->state != TASK_BLOCKED) {
            audio_pcm_wait_remove(task);
            return task->state == TASK_READY ? KERNEL_SYSCALL_BLOCKED : -LINUX_EAGAIN;
        }
        return KERNEL_SYSCALL_BLOCKED;
    }
}

/** @brief Identify PCM ioctls before inspecting a user argument.
 * @param request Native Linux PCM ioctl number.
 * @return Nonzero for the implemented command set; zero returns ENOTTY without
 * dereferencing a potentially invalid argument pointer.
 */
static int audio_device_ioctl_known(uint64_t request)
{
    switch (request) {
    case SNDRV_PCM_IOCTL_TSTAMP:
    case SNDRV_PCM_IOCTL_LINK:
    case SNDRV_PCM_IOCTL_DRAIN:
    case SNDRV_PCM_IOCTL_INFO:
    case SNDRV_PCM_IOCTL_SW_PARAMS:
    case SNDRV_PCM_IOCTL_CHANNEL_INFO:
    case SNDRV_PCM_IOCTL_UNLINK:
    case SNDRV_PCM_IOCTL_REWIND:
    case SNDRV_PCM_IOCTL_FORWARD:
    case SNDRV_PCM_IOCTL_PVERSION:
    case SNDRV_PCM_IOCTL_USER_PVERSION:
    case SNDRV_PCM_IOCTL_TTSTAMP:
    case SNDRV_PCM_IOCTL_HW_REFINE:
    case SNDRV_PCM_IOCTL_HW_PARAMS:
    case SNDRV_PCM_IOCTL_HW_FREE:
    case SNDRV_PCM_IOCTL_RESET:
    case SNDRV_PCM_IOCTL_XRUN:
    case SNDRV_PCM_IOCTL_STATUS:
    case SNDRV_PCM_IOCTL_STATUS_EXT:
    case SNDRV_PCM_IOCTL_DELAY:
    case SNDRV_PCM_IOCTL_HWSYNC:
    case SNDRV_PCM_IOCTL_SYNC_PTR:
    case SNDRV_PCM_IOCTL_PREPARE:
    case SNDRV_PCM_IOCTL_START:
    case SNDRV_PCM_IOCTL_DROP:
    case SNDRV_PCM_IOCTL_PAUSE:
    case SNDRV_PCM_IOCTL_RESUME:
    case SNDRV_PCM_IOCTL_WRITEI_FRAMES:
    case SNDRV_PCM_IOCTL_READI_FRAMES:
        return 1;
    default:
        return 0;
    }
}

/**
 * @brief Read captured PCM bytes from a standard capture description.
 * @param task Calling task used by the syscall adapter.
 * @param file Open capture description.
 * @param buffer Writable kernel-accessible destination.
 * @param count Destination capacity in bytes.
 * @return Bytes transferred, or a negative Linux errno.
 */
int audio_device_read(struct task *task, struct task_file *file,
                      void *buffer, uint32_t count)
{
    return audio_device_transfer(task, file, buffer, count, 0);
}

/**
 * @brief Write playback PCM bytes to a standard playback description.
 * @param task Calling task used by the syscall adapter.
 * @param file Open playback description.
 * @param buffer Kernel-accessible source bytes.
 * @param count Source length in bytes.
 * @return Bytes transferred, or a negative Linux errno.
 */
int audio_device_write(struct task *task, struct task_file *file,
                       const void *buffer, uint32_t count)
{
    return audio_device_transfer(task, file, (void *)(uintptr_t)buffer, count, 1);
}

/**
 * @brief Dispatch the standard PCM ioctl extension point.
 * @param task Calling task pinned by syscall execution; NULL allows nonblocking probes.
 * @param file Open PCM description.
 * @param request Native Linux ioctl number.
 * @param address User argument address passed to the ALSA adapter.
 * @return Zero with copied result, negative Linux errno, or KERNEL_SYSCALL_BLOCKED.
 * Task context; nested ranges and writable outputs are validated before state
 * mutation. LINK takes a direct fd; frame I/O uses bounded copies and the PCM
 * wait queue. The caller pins the OFD throughout retry and releases it at exit.
 */
int audio_device_ioctl(struct task *task, struct task_file *file,
                       uint64_t request, uint64_t address)
{
    struct audio_file *audio = audio_device_file(file);
    if (!audio) return -LINUX_EBADF;
    if (!audio_device_ioctl_known(request)) return -LINUX_ENOTTY;
    if (request == SNDRV_PCM_IOCTL_TSTAMP) return 0;
    if (request == SNDRV_PCM_IOCTL_LINK) {
        if (!task || address > INT_MAX)
            return -LINUX_EBADF;
        struct task_file *other = task_descriptor_for_fd(task, (int)address);
        if (!other || !other->used) return -LINUX_EBADF;
        struct audio_file *target = audio_device_file(task_file_description(other));
        return target ? audio_pcm_link(audio->pcm, target->pcm) : -LINUX_EBADF;
    }
    if (request == SNDRV_PCM_IOCTL_DRAIN) {
        if (task) audio_pcm_wait_remove(task);
        for (;;) {
            uint32_t epoch = audio_pcm_event_epoch();
            int ret = audio_pcm_drain(audio->pcm);
            if (ret != -LINUX_EAGAIN || !task || !task->pid ||
                (file->flags & RELIEFOS_O_NONBLOCK)) return ret;
            audio_pcm_wait_current();
            if (audio_pcm_event_epoch() != epoch) {
                audio_pcm_wait_remove(task);
                sched_wake_interruptible(task);
                continue;
            }
            if (task->state != TASK_BLOCKED) audio_pcm_wait_remove(task);
            return KERNEL_SYSCALL_BLOCKED;
        }
    }
    uint32_t size = _IOC_SIZE(request);
    if (!size) return audio_alsa_ioctl(audio->pcm, request, NULL);
    if (size > 1024u || !address) return -LINUX_EFAULT;
    uint32_t direction = _IOC_DIR(request);
    int copy_in = (direction & _IOC_WRITE) != 0;
    int copy_out = (direction & _IOC_READ) != 0;
    /* These historical _IOR commands still carry an input selector/pointer. */
    if (request == SNDRV_PCM_IOCTL_READI_FRAMES ||
        request == SNDRV_PCM_IOCTL_CHANNEL_INFO) copy_in = 1;
    if (request == SNDRV_PCM_IOCTL_WRITEI_FRAMES) copy_out = 1;
    if ((copy_in && !user_range_ok(address, size)) ||
        (copy_out && !user_range_writable(address, size))) return -LINUX_EFAULT;
    union {
        max_align_t align;
        uint8_t bytes[1024];
    } local = {0};
    if (copy_in) __builtin_memcpy(local.bytes, (const void *)(uintptr_t)address, size);
    if (request == SNDRV_PCM_IOCTL_READI_FRAMES ||
        request == SNDRV_PCM_IOCTL_WRITEI_FRAMES) {
        struct snd_xferi *transfer = (struct snd_xferi *)local.bytes;
        struct audio_pcm_status status;
        int ret = audio_pcm_status(audio->pcm, &status);
        if (ret) return ret;
        if (!status.frame_bytes) return -LINUX_EBADFD;
        if (transfer->frames > UINT32_MAX ||
            (transfer->frames && !transfer->buf) ||
            transfer->frames > UINT32_MAX / status.frame_bytes) return -LINUX_EINVAL;
        uint64_t bytes = transfer->frames * status.frame_bytes;
        if (bytes && (request == SNDRV_PCM_IOCTL_WRITEI_FRAMES
                          ? !user_range_ok((uintptr_t)transfer->buf, bytes)
                          : !user_range_writable((uintptr_t)transfer->buf, bytes)))
            return -LINUX_EFAULT;
        int writing = request == SNDRV_PCM_IOCTL_WRITEI_FRAMES;
        int done = audio_device_transfer(task, file, transfer->buf, (uint32_t)bytes, writing);
        if (done < 0) return done;
        transfer->result = done / status.frame_bytes;
        __builtin_memcpy((void *)(uintptr_t)address, local.bytes, size);
        return 0;
    }
    int ret = audio_alsa_ioctl(audio->pcm, request, local.bytes);
    if (!ret && request == SNDRV_PCM_IOCTL_USER_PVERSION)
        audio->user_pversion = *(uint32_t *)(void *)local.bytes;
    if (!ret && copy_out) __builtin_memcpy((void *)(uintptr_t)address, local.bytes, size);
    return ret;
}

/**
 * @brief Report poll readiness from one PCM status snapshot.
 * @param file Open PCM description.
 * @param events Requested POLLIN/POLLOUT bits.
 * @return Ready/error/hangup mask, including POLLNVAL for an invalid file.
 */
short audio_device_poll(const struct task_file *file, short events)
{
    const struct audio_file *audio = audio_device_file(file);
    if (!audio) return POLLNVAL;
    struct audio_pcm_status status;
    int ret = audio_pcm_status(audio->pcm, &status);
    if (ret < 0) return POLLERR | POLLHUP;
    if (status.state == AUDIO_PCM_OPEN || status.state == AUDIO_PCM_SETUP) return 0;
    short ready = 0;
    if (status.error || status.state == AUDIO_PCM_XRUN) ready |= POLLERR;
    uint64_t available = (status.hw_ptr + status.boundary - status.appl_ptr) % status.boundary;
    if (status.direction == AUDIO_PLAYBACK) {
        uint64_t queued = (status.appl_ptr + status.boundary - status.hw_ptr) % status.boundary;
        if ((events & POLLOUT) && queued <= status.buffer_frames &&
            status.buffer_frames - queued >= status.avail_min) ready |= POLLOUT;
    } else if ((events & POLLIN) && available >= status.avail_min &&
               available <= status.buffer_frames) {
        ready |= POLLIN;
    }
    return ready;
}

/**
 * @brief Reject seek operations on a frame-stream device.
 * @param file Open PCM description.
 * @param offset Requested byte offset, ignored.
 * @param whence Requested origin, ignored.
 * @param result Optional result pointer, ignored.
 * @return Always -ESPIPE for a valid or invalid stream description.
 */
int audio_device_seek(struct task_file *file, int64_t offset, int whence,
                      uint64_t *result)
{
    (void)file;
    (void)offset;
    (void)whence;
    (void)result;
    return -LINUX_ESPIPE;
}
