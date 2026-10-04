#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/time.h>
#include <reliefnt/audio.h>
#include <reliefnt/lock.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/time.h>
#include <reliefnt/usercopy.h>
#include <sound/asound.h>

#define AUDIO_TIMER_MAX 32u
#define AUDIO_TIMER_QUEUE_MAX 128u
#define AUDIO_TIMER_DEFAULT_RESOLUTION 1000000u

struct audio_timer_event {
    int32_t event;
    struct linux_timespec timestamp;
    uint32_t value;
};

struct audio_timer_status_abi {
    struct linux_timespec tstamp;
    uint32_t resolution;
    uint32_t lost;
    uint32_t overrun;
    uint32_t queue;
    uint8_t reserved[64];
};

struct audio_timer_tread_abi {
    int32_t event;
    int32_t pad1;
    struct linux_timespec tstamp;
    uint32_t value;
    uint32_t pad2;
};

struct audio_timer_file {
    uint32_t used;
    uint32_t selected;
    uint32_t tread;
    uint32_t running;
    uint32_t paused;
    uint32_t card;
    uint32_t device;
    enum audio_direction direction;
    uint32_t ticks;
    uint32_t filter;
    uint32_t resolution;
    uint32_t head;
    uint32_t count;
    uint32_t lost;
    uint32_t overrun;
    struct audio_timer_event queue[AUDIO_TIMER_QUEUE_MAX];
    struct kernel_spinlock lock;
};

static struct audio_timer_file audio_timers[AUDIO_TIMER_MAX];

/** @brief Copy a bounded literal into an ALSA fixed-width character field. */
static void audio_timer_text(unsigned char *dst, uint32_t capacity,
                             const char *src)
{
    if (!dst || !capacity) return;
    memset(dst, 0, capacity);
    if (!src) return;
    for (uint32_t i = 0; i + 1u < capacity && src[i]; ++i) dst[i] = (unsigned char)src[i];
}

/** @brief Take a free timer OFD slot from the bounded synthetic-device table. */
static struct audio_timer_file *audio_timer_slot(void)
{
    for (uint32_t i = 0; i < AUDIO_TIMER_MAX; ++i)
        if (!__atomic_load_n(&audio_timers[i].used, __ATOMIC_ACQUIRE))
            return &audio_timers[i];
    return NULL;
}

/** @brief Return the timer state owned by an open-file description. */
static struct audio_timer_file *audio_timer_state(const struct task_file *file)
{
    return file && (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
        ? file->audio_timer_file : NULL;
}

/** @brief Queue one timestamped event, dropping the oldest item on overflow. */
static void audio_timer_queue_locked(struct audio_timer_file *timer,
                                     int event, uint32_t value)
{
    if (!timer->running && event == SNDRV_TIMER_EVENT_TICK) return;
    if (event == SNDRV_TIMER_EVENT_TICK && timer->tread && timer->filter &&
        !(timer->filter & (1u << SNDRV_TIMER_EVENT_TICK))) return;
    struct audio_timer_event item = {.event = event, .value = value};
    (void)time_clock_get(LINUX_CLOCK_MONOTONIC, &item.timestamp);
    if (timer->count == AUDIO_TIMER_QUEUE_MAX) {
        timer->head = (timer->head + 1u) % AUDIO_TIMER_QUEUE_MAX;
        --timer->count;
        ++timer->lost;
        ++timer->overrun;
    }
    uint32_t tail = (timer->head + timer->count) % AUDIO_TIMER_QUEUE_MAX;
    timer->queue[tail] = item;
    ++timer->count;
}

/** @brief Publish one hardware PCM period to all matching ALSA timer OFDs.
 * This is called after the audio registry lock is released and performs only
 * bounded IRQ-safe queueing; PCM DMA and user memory are never touched here.
 */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)frames;
    for (uint32_t i = 0; i < AUDIO_TIMER_MAX; ++i) {
        struct audio_timer_file *timer = &audio_timers[i];
        if (!__atomic_load_n(&timer->used, __ATOMIC_ACQUIRE)) continue;
        uint64_t flags;
        kernel_spin_lock_irqsave(&timer->lock, &flags);
        if (timer->used && timer->selected && timer->card == card &&
            timer->device == device && timer->direction == direction) {
            if (error) {
                timer->running = 0;
                if (!timer->tread || !timer->filter ||
                    (timer->filter & (1u << SNDRV_TIMER_EVENT_STOP)))
                    audio_timer_queue_locked(timer, SNDRV_TIMER_EVENT_STOP, 0);
            } else {
                audio_timer_queue_locked(timer, SNDRV_TIMER_EVENT_TICK,
                                         timer->ticks ? timer->ticks : 1u);
            }
        }
        kernel_spin_unlock_irqrestore(&timer->lock, flags);
    }
}

/** @brief Open the synthetic ALSA timer node; selection happens by ioctl. */
int audio_timer_open(struct task *task, struct task_file *file)
{
    (void)task;
    if (!file || !(file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER) ||
        file->audio_timer_file) return -LINUX_EBADF;
    struct audio_timer_file *timer = audio_timer_slot();
    if (!timer) return -LINUX_ENFILE;
    *timer = (struct audio_timer_file){
        .used = 1,
        .ticks = 1,
        .resolution = AUDIO_TIMER_DEFAULT_RESOLUTION,
        .lock = KERNEL_SPINLOCK_INIT,
    };
    file->audio_timer_file = timer;
    return 0;
}

/** @brief Close the final timer OFD and discard queued events. */
int audio_timer_close(struct task_file *file)
{
    struct audio_timer_file *timer = audio_timer_state(file);
    if (!timer) return -LINUX_EBADF;
    uint64_t flags;
    kernel_spin_lock_irqsave(&timer->lock, &flags);
    timer->used = 0;
    timer->selected = 0;
    timer->running = 0;
    timer->count = 0;
    kernel_spin_unlock_irqrestore(&timer->lock, flags);
    file->audio_timer_file = NULL;
    return 0;
}

/** @brief Read one or more queued timer events in the selected ABI format. */
int audio_timer_read(struct task *task, struct task_file *file,
                     void *buffer, uint32_t count)
{
    (void)task;
    struct audio_timer_file *timer = audio_timer_state(file);
    if (!timer || !buffer) return -LINUX_EBADF;
    uint32_t item_size = timer->tread ? (uint32_t)sizeof(struct audio_timer_tread_abi)
                                     : (uint32_t)sizeof(struct snd_timer_read);
    if (!count || count % item_size) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&timer->lock, &flags);
    if (!timer->count) {
        kernel_spin_unlock_irqrestore(&timer->lock, flags);
        return -LINUX_EAGAIN;
    }
    uint32_t items = count / item_size;
    if (items > timer->count) items = timer->count;
    for (uint32_t i = 0; i < items; ++i) {
        struct audio_timer_event item = timer->queue[timer->head];
        timer->head = (timer->head + 1u) % AUDIO_TIMER_QUEUE_MAX;
        --timer->count;
        if (timer->tread) {
            struct audio_timer_tread_abi *out =
                (struct audio_timer_tread_abi *)((uint8_t *)buffer + i * item_size);
            *out = (struct audio_timer_tread_abi){
                .event = item.event, .tstamp = item.timestamp, .value = item.value};
        } else {
            struct snd_timer_read *out =
                (struct snd_timer_read *)((uint8_t *)buffer + i * item_size);
            *out = (struct snd_timer_read){.resolution = timer->resolution,
                                           .ticks = item.value};
        }
    }
    kernel_spin_unlock_irqrestore(&timer->lock, flags);
    return (int)(items * item_size);
}

/** @brief Apply one validated ioctl to a timer OFD. */
static int audio_timer_ioctl_file(struct audio_timer_file *timer,
                                  uint64_t request, void *argument)
{
    if (!timer || !timer->used) return -LINUX_EBADF;
    if (request == SNDRV_TIMER_IOCTL_PVERSION) {
        if (!argument) return -LINUX_EFAULT;
        *(int *)argument = SNDRV_TIMER_VERSION;
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_TREAD_OLD ||
        request == SNDRV_TIMER_IOCTL_TREAD64) {
        if (!argument) return -LINUX_EFAULT;
        timer->tread = *(int *)argument != 0;
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_SELECT) {
        if (!argument) return -LINUX_EFAULT;
        struct snd_timer_select *select = argument;
        if (select->id.dev_class != SNDRV_TIMER_CLASS_PCM ||
            select->id.dev_sclass != SNDRV_TIMER_SCLASS_NONE ||
            select->id.card < 0 || select->id.device < 0 ||
            select->id.subdevice < 0 || select->id.subdevice > 1) return -LINUX_EINVAL;
        uint32_t card;
        int ret = audio_card_snapshot((uint32_t)select->id.card, &card, NULL);
        if (ret < 0) return ret;
        enum audio_direction direction = select->id.subdevice & 1
            ? AUDIO_CAPTURE : AUDIO_PLAYBACK;
        struct audio_caps caps;
        ret = audio_card_pcm_caps(card, (uint32_t)select->id.device,
                                  direction, &caps);
        if (ret < 0) return ret;
        timer->card = card;
        timer->device = (uint32_t)select->id.device;
        timer->direction = direction;
        timer->selected = 1;
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_INFO) {
        if (!argument || !timer->selected) return -LINUX_EBADFD;
        struct snd_timer_info *info = argument;
        *info = (struct snd_timer_info){
            .flags = SNDRV_TIMER_FLG_SLAVE,
            .card = audio_card_index(timer->card),
            .resolution = timer->resolution,
        };
        audio_timer_text(info->id, sizeof(info->id), "reliefos-pcm");
        audio_timer_text(info->name, sizeof(info->name), "ReliefOS PCM period timer");
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_PARAMS) {
        if (!argument || !timer->selected) return -LINUX_EBADFD;
        struct snd_timer_params *params = argument;
        if (!params->ticks || params->queue_size > AUDIO_TIMER_QUEUE_MAX ||
            (params->queue_size && params->queue_size < 32u)) return -LINUX_EINVAL;
        timer->ticks = params->ticks;
        timer->filter = params->filter;
        timer->head = timer->count = timer->lost = timer->overrun = 0;
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_STATUS) {
        if (!argument) return -LINUX_EFAULT;
        struct audio_timer_status_abi *status = argument;
        *status = (struct audio_timer_status_abi){
            .resolution = timer->resolution,
            .lost = timer->lost,
            .overrun = timer->overrun,
            .queue = timer->count,
        };
        (void)time_clock_get(LINUX_CLOCK_MONOTONIC, &status->tstamp);
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_START ||
        request == SNDRV_TIMER_IOCTL_CONTINUE) {
        if (!timer->selected) return -LINUX_EBADFD;
        timer->running = 1;
        timer->paused = 0;
        if (request == SNDRV_TIMER_IOCTL_START &&
            (!timer->tread || !timer->filter ||
             (timer->filter & (1u << SNDRV_TIMER_EVENT_START))))
            audio_timer_queue_locked(timer, SNDRV_TIMER_EVENT_START, timer->resolution);
        return 0;
    }
    if (request == SNDRV_TIMER_IOCTL_STOP ||
        request == SNDRV_TIMER_IOCTL_PAUSE) {
        if (request == SNDRV_TIMER_IOCTL_PAUSE) timer->paused = 1;
        timer->running = 0;
        if (!timer->tread || !timer->filter ||
            (timer->filter & (1u << (request == SNDRV_TIMER_IOCTL_PAUSE
                                      ? SNDRV_TIMER_EVENT_PAUSE : SNDRV_TIMER_EVENT_STOP))))
            audio_timer_queue_locked(timer,
                request == SNDRV_TIMER_IOCTL_PAUSE ? SNDRV_TIMER_EVENT_PAUSE
                                                   : SNDRV_TIMER_EVENT_STOP, 0);
        return 0;
    }
    return -LINUX_ENOTTY;
}

/** @brief Copy and dispatch an ALSA timer ioctl through the syscall boundary. */
int audio_timer_ioctl(struct task *task, struct task_file *file,
                      uint64_t request, uint64_t address)
{
    (void)task;
    struct audio_timer_file *timer = audio_timer_state(file);
    if (!timer) return -LINUX_EBADF;
    uint32_t size = _IOC_SIZE(request);
    if (!size) return audio_timer_ioctl_file(timer, request, NULL);
    if (size > 1024u || !address) return -LINUX_EFAULT;
    uint32_t direction = _IOC_DIR(request);
    int copy_in = (direction & _IOC_WRITE) != 0;
    int copy_out = (direction & _IOC_READ) != 0;
    if ((copy_in && !user_range_ok(address, size)) ||
        (copy_out && !user_range_writable(address, size))) return -LINUX_EFAULT;
    union { max_align_t align; uint8_t bytes[1024]; } local = {0};
    if (copy_in) __builtin_memcpy(local.bytes, (const void *)(uintptr_t)address, size);
    int ret = audio_timer_ioctl_file(timer, request, local.bytes);
    if (!ret && copy_out) __builtin_memcpy((void *)(uintptr_t)address, local.bytes, size);
    return ret;
}

/** @brief Return timer readiness and disconnect errors for poll/epoll. */
short audio_timer_poll(const struct task_file *file, short events)
{
    struct audio_timer_file *timer = audio_timer_state(file);
    if (!timer) return POLLNVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&timer->lock, &flags);
    short ready = timer->count ? (POLLIN | POLLRDNORM) : 0;
    if (!timer->used || !timer->selected) ready |= POLLERR | POLLHUP;
    kernel_spin_unlock_irqrestore(&timer->lock, flags);
    return ready & (events | POLLERR | POLLHUP);
}

/** @brief Reject seek because timer descriptors are event streams. */
int audio_timer_seek(struct task_file *file, int64_t offset, int whence,
                     uint64_t *result)
{
    (void)file; (void)offset; (void)whence; (void)result;
    return -LINUX_ESPIPE;
}
