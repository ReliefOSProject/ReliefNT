#include <limits.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <stddef.h>
#include <stdint.h>

#include <sound/asound.h>
#include <sound/tlv.h>
#include <reliefnt/audio.h>
#include <reliefnt/futex.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/usercopy.h>

#define AUDIO_CONTROL_FILE_MAX 64U
#define AUDIO_CONTROL_MAX 64U
#define AUDIO_CONTROL_EVENT_MAX 64U

struct audio_control_file {
    uint32_t used;
    uint32_t card;
    uint32_t subscribed;
    uint32_t caller_pid;
    uint32_t opener_pid;
    struct audio_control_event_queue events;
};

struct audio_control_lock {
    uint32_t used;
    uint32_t card;
    uint32_t control;
    uint32_t owner_pid;
    struct audio_control_file *owner;
};

static struct audio_control_file audio_control_files[AUDIO_CONTROL_FILE_MAX];
static struct audio_control_lock audio_control_locks[AUDIO_CONTROL_EVENT_MAX];

/** @brief Copy a bounded NUL-terminated control name into an ALSA ID. */
static void audio_control_copy_name(unsigned char *dst, const char *src,
                                    size_t capacity)
{
    if (!dst || !capacity) return;
    size_t i = 0;
    while (src && src[i] && i + 1 < capacity) {
        dst[i] = (unsigned char)src[i];
        ++i;
    }
    dst[i] = 0;
}

/** @brief Return the open-file lock for one card/control, if any. */
static struct audio_control_lock *audio_control_find_lock(uint32_t card,
                                                           uint32_t control)
{
    for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_MAX; ++i)
        if (audio_control_locks[i].used && audio_control_locks[i].card == card &&
            audio_control_locks[i].control == control)
            return &audio_control_locks[i];
    return NULL;
}

/** @brief Resolve an ALSA element ID to a zero-based real control index. */
static int audio_control_resolve(struct audio_control_file *file,
                                 const struct snd_ctl_elem_id *id,
                                 uint32_t *control)
{
    if (!file || !file->used || !id || !control) return -EINVAL;
    uint32_t count = 0;
    int ret = audio_card_control_count(file->card, &count);
    if (ret) return ret;
    if (id->numid) {
        if (id->numid > count) return -ENOENT;
        *control = id->numid - 1;
        return 0;
    }
    if (id->iface != SNDRV_CTL_ELEM_IFACE_MIXER || id->device ||
        id->subdevice || id->index) return -ENOENT;
    for (uint32_t i = 0; i < count; ++i) {
        struct audio_control_info info;
        ret = audio_card_control_info(file->card, i, &info);
        if (ret) return ret;
        if (!__builtin_strncmp((const char *)id->name, info.name, SNDRV_CTL_ELEM_ID_NAME_MAXLEN)) {
            *control = i;
            return 0;
        }
    }
    return -ENOENT;
}

/** @brief Populate an ALSA element ID from real control metadata. */
static int audio_control_fill_id(struct audio_control_file *file, uint32_t control,
                                 struct snd_ctl_elem_id *id)
{
    struct audio_control_info info;
    int ret = audio_card_control_info(file->card, control, &info);
    if (ret) return ret;
    __builtin_memset(id, 0, sizeof(*id));
    id->numid = control + 1;
    id->iface = SNDRV_CTL_ELEM_IFACE_MIXER;
    audio_control_copy_name(id->name, info.name, sizeof(id->name));
    return 0;
}

/** @brief Return whether a control is currently owned by another OFD. */
static int audio_control_locked_by_other(struct audio_control_file *file,
                                         uint32_t control)
{
    struct audio_control_lock *lock = audio_control_find_lock(file->card, control);
    return lock && lock->owner != file;
}

/** @brief Validate an integer or enumerated value against metadata. */
static int audio_control_validate_value(const struct audio_control_info *info,
                                        const struct audio_control_value *value)
{
    if (!info || !value || !info->count || info->count > 16) return -EINVAL;
    if (info->type == AUDIO_CONTROL_ENUMERATED) {
        if (info->items > 16) return -EINVAL;
        for (uint32_t i = 0; i < info->count; ++i)
            if (value->values[i] < 0 || (uint64_t)value->values[i] >= info->items)
                return -EINVAL;
        return 0;
    }
    if (info->type != AUDIO_CONTROL_BOOLEAN && info->type != AUDIO_CONTROL_INTEGER)
        return -EINVAL;
    for (uint32_t i = 0; i < info->count; ++i) {
        int64_t v = value->values[i];
        if (v < info->min || v > info->max) return -ERANGE;
        if (info->step && ((v - info->min) % info->step) != 0) return -EINVAL;
    }
    return 0;
}

/** @brief Open a control OFD and register its preallocated event queue.
 * @param card Live generation token with at most AUDIO_CONTROL_MAX elements.
 * @param out Writable output receiving owned OFD state on success.
 * @return Zero, -EINVAL, -ENODEV, -EOVERFLOW, or -ENFILE. Execution ownership
 * serializes the file pool; queue attachment separately excludes IRQ updates.
 */
int audio_control_open_card(uint32_t card, struct audio_control_file **out)
{
    if (!out) return -EINVAL;
    uint32_t count = 0;
    int ret = audio_card_control_count(card, &count);
    if (ret) return ret;
    if (count > AUDIO_CONTROL_MAX) return -EOVERFLOW;
    for (uint32_t i = 0; i < AUDIO_CONTROL_FILE_MAX; ++i) {
        struct audio_control_file *file = &audio_control_files[i];
        if (file->used) continue;
        *file = (struct audio_control_file){.used = 1, .card = card};
        ret = audio_control_queue_open(card, &file->events);
        if (ret) { file->used = 0; return ret; }
        *out = file;
        return 0;
    }
    return -ENFILE;
}

/** @brief Detach an OFD event queue and release its element locks.
 * @param file Live owned OFD released at its final description reference.
 * @return Zero or -EBADF. Execution ownership serializes lock/file tables;
 * queue detachment completes under the registry lock before storage reuse.
 */
int audio_control_close_file(struct audio_control_file *file)
{
    if (!file || !file->used) return -EBADF;
    int ret = audio_control_queue_close(&file->events);
    if (ret) return ret;
    for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_MAX; ++i) {
        if (!audio_control_locks[i].used || audio_control_locks[i].owner != file) continue;
        audio_control_locks[i] = (struct audio_control_lock){0};
    }
    *file = (struct audio_control_file){0};
    return 0;
}

/** @brief Dispatch an ALSA control ioctl through shared OFD state.
 * @param file Live control OFD, serialized by execution ownership.
 * @param request Linux ioctl number; unsupported operations return explicit errors.
 * @param argument Kernel-accessible storage, including validated nested buffers.
 * @return Zero or a negative Linux errno; successful writes notify subscribers.
 * SUBSCRIBE_EVENTS reads back state for negative inputs and preserves queued
 * events on repeated enable. ELEM_INFO reports LOCK only while locked, OWNER
 * only to that OFD, and owner=-1 when unlocked, matching Linux control semantics.
 */
int audio_alsa_control_ioctl(struct audio_control_file *file, uint64_t request,
                             void *argument)
{
    if (!file || !file->used) return -EBADF;
    switch (request) {
    case SNDRV_CTL_IOCTL_PVERSION:
        if (!argument) return -EFAULT;
        *(int *)argument = SNDRV_CTL_VERSION;
        return 0;
    case SNDRV_CTL_IOCTL_CARD_INFO: {
        if (!argument) return -EFAULT;
        struct audio_card_identity identity;
        int ret = audio_card_identity(file->card, &identity);
        if (ret) return ret;
        struct snd_ctl_card_info *info = argument;
        __builtin_memset(info, 0, sizeof(*info));
        int card = audio_card_index(file->card);
        if (card < 0) return -ENODEV;
        info->card = card;
        audio_control_copy_name(info->id, identity.id, sizeof(info->id));
        audio_control_copy_name(info->driver, "reliefnt", sizeof(info->driver));
        audio_control_copy_name(info->name, identity.name, sizeof(info->name));
        audio_control_copy_name(info->longname, identity.name, sizeof(info->longname));
        audio_control_copy_name(info->mixername, identity.name, sizeof(info->mixername));
        audio_control_copy_name(info->components, identity.id, sizeof(info->components));
        return 0;
    }
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE: {
        if (!argument) return -EFAULT;
        int previous = *(int *)argument;
        *(int *)argument = -1;
        if (previous >= 7) return 0;
        int first = previous < 0 ? 0 : previous + 1;
        for (int device=first; device<8; ++device) {
            struct snd_pcm_info info = {.device=(uint32_t)device};
            if (!audio_card_pcm_info(file->card,&info)) {
                *(int *)argument = device;
                return 0;
            }
            info = (struct snd_pcm_info){.device=(uint32_t)device,.stream=AUDIO_CAPTURE};
            if (!audio_card_pcm_info(file->card,&info)) {
                *(int *)argument = device;
                return 0;
            }
        }
        struct audio_card_identity identity;
        return audio_card_identity(file->card,&identity);
    }
    case SNDRV_CTL_IOCTL_PCM_INFO:
        return argument ? audio_card_pcm_info(file->card,argument) : -EFAULT;
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE:
        return argument ? 0 : -EFAULT;
    case SNDRV_CTL_IOCTL_ELEM_LIST: {
        if (!argument) return -EFAULT;
        struct snd_ctl_elem_list *list = argument;
        uint32_t count = 0;
        int ret = audio_card_control_count(file->card, &count);
        if (ret) return ret;
        if (list->space && !list->pids) return -EFAULT;
        list->count = count;
        list->used = 0;
        if (list->offset >= count) return 0;
        uint32_t available = count - list->offset;
        uint32_t used = list->space < available ? list->space : available;
        for (uint32_t i = 0; i < used; ++i) {
            ret = audio_control_fill_id(file, list->offset + i, &list->pids[i]);
            if (ret) return ret;
        }
        list->used = used;
        return 0;
    }
    case SNDRV_CTL_IOCTL_ELEM_INFO: {
        if (!argument) return -EFAULT;
        struct snd_ctl_elem_info request_info = *(struct snd_ctl_elem_info *)argument;
        uint32_t control;
        int ret = audio_control_resolve(file, &request_info.id, &control);
        if (ret) return ret;
        struct audio_control_info info;
        ret = audio_card_control_info(file->card, control, &info);
        if (ret) return ret;
        unsigned int requested_item = request_info.value.enumerated.item;
        struct snd_ctl_elem_info *out = argument;
        __builtin_memset(out, 0, sizeof(*out));
        ret = audio_control_fill_id(file, control, &out->id);
        if (ret) return ret;
        out->count = info.count;
        out->type = info.type == AUDIO_CONTROL_BOOLEAN ? SNDRV_CTL_ELEM_TYPE_BOOLEAN :
                    info.type == AUDIO_CONTROL_ENUMERATED ? SNDRV_CTL_ELEM_TYPE_ENUMERATED :
                    SNDRV_CTL_ELEM_TYPE_INTEGER;
        out->access = (info.access & AUDIO_CONTROL_READ) ? SNDRV_CTL_ELEM_ACCESS_READ : 0;
        if (info.access & AUDIO_CONTROL_WRITE) out->access |= SNDRV_CTL_ELEM_ACCESS_WRITE;
        if (info.access & AUDIO_CONTROL_VOLATILE) out->access |= SNDRV_CTL_ELEM_ACCESS_VOLATILE;
        if (info.db_min || info.db_step)
            out->access |= SNDRV_CTL_ELEM_ACCESS_TLV_READ;
        struct audio_control_lock *lock = audio_control_find_lock(file->card, control);
        if (lock) {
            out->access |= SNDRV_CTL_ELEM_ACCESS_LOCK;
            if (lock->owner == file) out->access |= SNDRV_CTL_ELEM_ACCESS_OWNER;
        }
        if (info.type == AUDIO_CONTROL_ENUMERATED) {
            if (requested_item >= info.items) return -EINVAL;
            out->value.enumerated.items = info.items;
            out->value.enumerated.item = requested_item;
            audio_control_copy_name((unsigned char *)out->value.enumerated.name,
                                    info.item_names[requested_item],
                                    sizeof(out->value.enumerated.name));
        } else {
            out->value.integer.min = info.min;
            out->value.integer.max = info.max;
            out->value.integer.step = info.step;
        }
        out->owner = lock ? (int32_t)lock->owner_pid : -1;
        return 0;
    }
    case SNDRV_CTL_IOCTL_ELEM_READ:
    case SNDRV_CTL_IOCTL_ELEM_WRITE: {
        if (!argument) return -EFAULT;
        struct snd_ctl_elem_value *value = argument;
        if (value->indirect) return -EINVAL;
        uint32_t control;
        int ret = audio_control_resolve(file, &value->id, &control);
        if (ret) return ret;
        struct audio_control_info info;
        ret = audio_card_control_info(file->card, control, &info);
        if (ret) return ret;
        if (request == SNDRV_CTL_IOCTL_ELEM_READ) {
            if (!(info.access & AUDIO_CONTROL_READ)) return -EACCES;
            struct audio_control_value current;
            ret = audio_card_control_read(file->card, control, &current);
            if (ret) return ret;
            for (uint32_t i = 0; i < info.count && i < 16; ++i) {
                if (info.type == AUDIO_CONTROL_ENUMERATED)
                    value->value.enumerated.item[i] = (unsigned int)current.values[i];
                else value->value.integer.value[i] = (long)current.values[i];
            }
            return 0;
        }
        if (!(info.access & AUDIO_CONTROL_WRITE)) return -EACCES;
        if (audio_control_locked_by_other(file, control)) return -EPERM;
        struct audio_control_value next = {0};
        for (uint32_t i = 0; i < info.count && i < 16; ++i)
            next.values[i] = info.type == AUDIO_CONTROL_ENUMERATED
                ? value->value.enumerated.item[i] : value->value.integer.value[i];
        ret = audio_control_validate_value(&info, &next);
        if (ret) return ret;
        ret = audio_card_control_write(file->card, control, &next);
        if (!ret) audio_control_changed(file->card, control);
        return ret;
    }
    case SNDRV_CTL_IOCTL_ELEM_LOCK:
    case SNDRV_CTL_IOCTL_ELEM_UNLOCK: {
        if (!argument) return -EFAULT;
        uint32_t control;
        int ret = audio_control_resolve(file, argument, &control);
        if (ret) return ret;
        struct audio_control_lock *lock = audio_control_find_lock(file->card, control);
        if (request == SNDRV_CTL_IOCTL_ELEM_LOCK) {
            if (lock) return -EBUSY;
            for (uint32_t i = 0; i < AUDIO_CONTROL_EVENT_MAX; ++i) {
                if (audio_control_locks[i].used) continue;
                audio_control_locks[i] = (struct audio_control_lock){
                    .used = 1, .card = file->card, .control = control,
                    .owner_pid = file->opener_pid, .owner = file};
                return 0;
            }
            return -ENFILE;
        }
        if (!lock) return -EINVAL;
        if (lock->owner != file) return -EPERM;
        *lock = (struct audio_control_lock){0};
        return 0;
    }
    case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS: {
        if (!argument) return -EFAULT;
        int subscribe = *(int *)argument;
        if (subscribe < 0) {
            *(int *)argument = (int)file->subscribed;
            return 0;
        }
        int ret = audio_control_queue_subscribe(&file->events, subscribe);
        if (ret) return ret;
        file->subscribed = subscribe != 0;
        return 0;
    }
    case SNDRV_CTL_IOCTL_TLV_READ: {
        if (!argument) return -EFAULT;
        struct snd_ctl_tlv *tlv = argument;
        if (!tlv->numid || tlv->length < 8u) return -EINVAL;
        struct snd_ctl_elem_id id = {.numid = tlv->numid};
        uint32_t control;
        int ret = audio_control_resolve(file, &id, &control);
        if (ret) return ret;
        struct audio_control_info info;
        ret = audio_card_control_info(file->card, control, &info);
        if (ret) return ret;
        if (info.type != AUDIO_CONTROL_INTEGER || (!info.db_min && !info.db_step))
            return -ENXIO;
        if (tlv->length < 16u) return -ENOMEM;
        tlv->tlv[0] = SNDRV_CTL_TLVT_DB_SCALE;
        tlv->tlv[1] = 8;
        tlv->tlv[2] = (uint32_t)info.db_min;
        tlv->tlv[3] = (uint32_t)info.db_step;
        return 0;
    }
    case SNDRV_CTL_IOCTL_ELEM_ADD:
    case SNDRV_CTL_IOCTL_ELEM_REPLACE:
    case SNDRV_CTL_IOCTL_ELEM_REMOVE:
        return -EOPNOTSUPP;
    case SNDRV_CTL_IOCTL_TLV_WRITE:
    case SNDRV_CTL_IOCTL_TLV_COMMAND:
        return -ENXIO;
    default:
        return -ENOTTY;
    }
}

/** @brief Write a control on behalf of another OFD such as /dev/mixer.
 * ALSA element locks are open-file-description scoped; an unrelated mixer
 * description must observe the same ownership boundary and cannot steal it.
 */
int audio_control_write_shared(uint32_t card, uint32_t control,
                               const struct audio_control_value *value)
{
    if (!value || audio_control_find_lock(card, control)) return -EPERM;
    int ret = audio_card_control_write(card, control, value);
    if (!ret) audio_control_changed(card, control);
    return ret;
}

/** @brief Copy and consume one pending event from a subscribed control OFD.
 * @param file Borrowed live control description, serialized by execution ownership.
 * @param buffer Kernel-accessible output for one snd_ctl_event.
 * @param count Output capacity in bytes.
 * @return Event bytes, -EBADFD if unsubscribed, -EAGAIN if empty, or negative errno.
 * Does not sleep; the task-aware wrapper owns blocking and signal wakeups.
 */
int audio_control_read_file(struct audio_control_file *file, void *buffer,
                            uint32_t count)
{
    if (!file || !file->used) return -EBADF;
    if (!file->subscribed) return -EBADFD;
    if (!buffer || count < sizeof(struct snd_ctl_event)) return -EINVAL;
    uint32_t control, mask = SNDRV_CTL_EVENT_MASK_VALUE;
    int ret = audio_control_queue_next(&file->events, &control, &mask, 1);
    if (ret) return ret;
    struct snd_ctl_event event = {0};
    event.type = SNDRV_CTL_EVENT_ELEM;
    event.data.elem.mask = mask;
    ret = audio_control_fill_id(file, control, &event.data.elem.id);
    if (ret) return ret;
    __builtin_memcpy(buffer, &event, sizeof(event));
    return (int)sizeof(event);
}

/** @brief Read an event or publish a race-checked interruptible control wait.
 * @param task Current task pinned by the syscall execution transaction.
 * @param file Borrowed control OFD, retained across syscall retries.
 * @param buffer Kernel-accessible output for one snd_ctl_event.
 * @param count Output capacity in bytes.
 * @return Event bytes, negative errno, or KERNEL_SYSCALL_BLOCKED for retry.
 * Queue exhaustion uses internal -EAGAIN; O_NONBLOCK callers use read_file.
 */
int audio_control_read_task(struct task *task, struct task_file *file,
                            void *buffer, uint32_t count)
{
    if (!task || !file || !file->audio_control_file)
        return -EBADF;
    audio_control_wait_remove(task);
    uint32_t epoch = audio_control_event_epoch();
    int ret = audio_control_read_file(file->audio_control_file, buffer, count);
    if (ret != -EAGAIN || task->pid == 0)
        return ret;

    /* Snapshot before the empty probe, then publish sleep before rechecking.
     * A wake between insertion and sleep can otherwise be overwritten by
     * TASK_BLOCKED; explicitly restore readiness in that case. */
    audio_control_wait_current();
    if (audio_control_event_epoch() != epoch) {
        audio_control_wait_remove(task);
        sched_wake_interruptible(task);
        ret = audio_control_read_file(file->audio_control_file, buffer, count);
        if (ret != -EAGAIN) return ret;
    }
    if (task->state != TASK_BLOCKED) {
        audio_control_wait_remove(task);
        /* READY includes pending signals and unrelated control wakes.  Keep
         * it runnable while the epilogue rewinds this syscall for retry. */
        return task->state == TASK_READY ? KERNEL_SYSCALL_BLOCKED : -EAGAIN;
    }
    return KERNEL_SYSCALL_BLOCKED;
}

/** @brief Inspect pending control events without consuming them.
 * @param file Borrowed live control OFD, serialized by execution ownership.
 * @param events Requested poll bits; only read readiness is produced.
 * @return Read bits for pending events, POLLNVAL for an invalid OFD, or
 * POLLERR|POLLHUP for a disconnected card. Does not allocate or sleep.
 */
short audio_control_poll_file(const struct audio_control_file *file, short events)
{
    if (!file || !file->used) return POLLNVAL;
    uint32_t count = 0;
    if (audio_card_control_count(file->card, &count) < 0) return POLLERR | POLLHUP;
    if (!file->subscribed) return 0;
    uint32_t control = 0, mask = 0;
    int ret = audio_control_queue_next((struct audio_control_event_queue *)&file->events,
                                      &control, &mask, 0);
    if (ret) return 0;
    return (events & (POLLIN | POLLRDNORM)) ? (POLLIN | POLLRDNORM) : 0;
}

/** @brief Open a /dev/snd/controlC* task file. */
int audio_control_open(struct task *task, struct task_file *file)
{
    if (!file || !(file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL))
        return -EBADF;
    int ret = audio_control_open_card(file->node.volume_id,&file->audio_control_file);
    if (!ret) file->audio_control_file->opener_pid = task ? task->pid : 0;
    return ret;
}

/** @brief Close the control state owned by a task file description. */
int audio_control_close(struct task_file *file)
{
    if (!file || !file->audio_control_file) return -EBADF;
    int ret = audio_control_close_file(file->audio_control_file);
    file->audio_control_file = NULL;
    return ret;
}

/** @brief Dispatch public control requests with bounded usercopy storage.
 * @param task Calling task, nullable in host fixtures; borrowed for owner PID.
 * @param file Live control OFD pinned by the syscall layer across this call.
 * @param request Native Linux ioctl number; unknown requests bypass usercopy.
 * @param address User argument and any nested arrays, validated before access.
 * @return Zero or negative Linux errno. Value payloads use their public 1224-byte
 * size; TLV/list nested user buffers are independently checked. NEXT_DEVICE's
 * _IOR encoding still consumes the previous device cursor, as Linux does.
 * No DMA exposure.
 */
int audio_control_ioctl(struct task *task, struct task_file *file,
                        uint64_t request, uint64_t address)
{
    if (!file || !file->audio_control_file) return -EBADF;
    /* Unknown commands do not consume their encoded userspace argument. */
    switch (request) {
    case SNDRV_CTL_IOCTL_PVERSION:
    case SNDRV_CTL_IOCTL_CARD_INFO:
    case SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE:
    case SNDRV_CTL_IOCTL_PCM_INFO:
    case SNDRV_CTL_IOCTL_PCM_PREFER_SUBDEVICE:
    case SNDRV_CTL_IOCTL_ELEM_LIST:
    case SNDRV_CTL_IOCTL_ELEM_INFO:
    case SNDRV_CTL_IOCTL_ELEM_READ:
    case SNDRV_CTL_IOCTL_ELEM_WRITE:
    case SNDRV_CTL_IOCTL_ELEM_LOCK:
    case SNDRV_CTL_IOCTL_ELEM_UNLOCK:
    case SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS:
    case SNDRV_CTL_IOCTL_TLV_READ:
    case SNDRV_CTL_IOCTL_ELEM_ADD:
    case SNDRV_CTL_IOCTL_ELEM_REPLACE:
    case SNDRV_CTL_IOCTL_ELEM_REMOVE:
    case SNDRV_CTL_IOCTL_TLV_WRITE:
    case SNDRV_CTL_IOCTL_TLV_COMMAND:
        break;
    default:
        return -ENOTTY;
    }
    uint32_t size = _IOC_SIZE(request);
    if (!size) {
        file->audio_control_file->caller_pid = task ? task->pid : 0;
        int ret = audio_alsa_control_ioctl(file->audio_control_file, request, NULL);
        file->audio_control_file->caller_pid = 0;
        return ret;
    }
    if (size > sizeof(struct snd_ctl_elem_value) || !address) return -EFAULT;
    uint32_t direction = _IOC_DIR(request);
    int copy_in = (direction & _IOC_WRITE) != 0 ||
                  request == SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE;
    int copy_out = (direction & _IOC_READ) != 0;
    if ((copy_in && !user_range_ok(address, size)) ||
        (copy_out && !user_range_writable(address, size))) return -EFAULT;
    union { max_align_t align; uint8_t bytes[sizeof(struct snd_ctl_elem_value)]; } local = {0};
    if (copy_in) __builtin_memcpy(local.bytes, (const void *)(uintptr_t)address, size);
    if (request == SNDRV_CTL_IOCTL_TLV_READ) {
        struct snd_ctl_tlv *tlv = (struct snd_ctl_tlv *)local.bytes;
        uint64_t payload = address + sizeof(*tlv);
        if (tlv->length >= 16u &&
            (payload < address || !user_range_writable(payload,16u))) return -EFAULT;
        int ret = audio_alsa_control_ioctl(file->audio_control_file,request,tlv);
        if (!ret) __builtin_memcpy((void *)(uintptr_t)payload,tlv->tlv,16u);
        return ret; /* Linux leaves the header's capacity unchanged. */
    }
    if (request == SNDRV_CTL_IOCTL_ELEM_LIST) {
        struct snd_ctl_elem_list *list = (struct snd_ctl_elem_list *)local.bytes;
        if (list->space && (!list->pids ||
            !user_range_writable((uintptr_t)list->pids,
                                 (uint64_t)list->space * sizeof(*list->pids))))
            return -EFAULT;
    }
    file->audio_control_file->caller_pid = task ? task->pid : 0;
    int ret = audio_alsa_control_ioctl(file->audio_control_file, request, local.bytes);
    file->audio_control_file->caller_pid = 0;
    if (!ret && copy_out) __builtin_memcpy((void *)(uintptr_t)address, local.bytes, size);
    return ret;
}
