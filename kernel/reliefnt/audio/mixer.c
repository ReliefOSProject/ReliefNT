#include <limits.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/soundcard.h>
#include <stdint.h>
#include <string.h>

#include <reliefnt/audio.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/usercopy.h>

#define AUDIO_MIXER_FILE_MAX 64U
#define AUDIO_MIXER_NONE UINT32_MAX

struct audio_mixer_file {
    uint32_t used;
    uint32_t card;
    uint32_t playback;
    uint32_t capture;
    uint32_t mute;
    uint32_t source;
    uint32_t devmask;
    uint32_t recmask;
    uint32_t stereodevs;
    uint32_t caps;
    uint32_t recsrc;
};

static struct audio_mixer_file audio_mixer_files[AUDIO_MIXER_FILE_MAX];

static char audio_mixer_lower(char value)
{
    return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

static int audio_mixer_name_has(const char *name, const char *needle)
{
    if (!name || !needle || !needle[0]) return 0;
    for (uint32_t i = 0; name[i]; ++i) {
        uint32_t j = 0;
        while (needle[j] && name[i + j] &&
               audio_mixer_lower(name[i + j]) == audio_mixer_lower(needle[j])) ++j;
        if (!needle[j]) return 1;
    }
    return 0;
}

static int audio_mixer_writable(const struct audio_control_info *info)
{
    return info && (info->access & AUDIO_CONTROL_WRITE) &&
           (info->type == AUDIO_CONTROL_INTEGER || info->type == AUDIO_CONTROL_BOOLEAN);
}

static int audio_mixer_read_info(const struct audio_mixer_file *file, uint32_t control,
                                 struct audio_control_info *info)
{
    if (!file || !file->used || control == AUDIO_MIXER_NONE || !info) return -EOPNOTSUPP;
    int ret = audio_card_control_info(file->card, control, info);
    if (ret) return ret;
    if (!info->count || info->count > 2) return -EOPNOTSUPP;
    return 0;
}

static uint32_t audio_mixer_source_bit(const char *name, uint32_t index)
{
    if (audio_mixer_name_has(name, "mic")) return SOUND_MASK_MIC;
    if (audio_mixer_name_has(name, "line")) return SOUND_MASK_LINE;
    return index == 0 ? SOUND_MASK_LINE : SOUND_MASK_MIC;
}

static int audio_mixer_select_controls(struct audio_mixer_file *file)
{
    uint32_t count = 0;
    int ret = audio_card_control_count(file->card, &count);
    if (ret) return ret;
    file->playback = file->capture = file->mute = file->source = AUDIO_MIXER_NONE;
    uint32_t fallback = AUDIO_MIXER_NONE;
    for (uint32_t i = 0; i < count; ++i) {
        struct audio_control_info info;
        ret = audio_card_control_info(file->card, i, &info);
        if (ret) return ret;
        if (audio_mixer_writable(&info) && info.type == AUDIO_CONTROL_INTEGER) {
            if (fallback == AUDIO_MIXER_NONE) fallback = i;
            if (file->playback == AUDIO_MIXER_NONE &&
                (audio_mixer_name_has(info.name, "playback") ||
                 audio_mixer_name_has(info.name, "master") ||
                 audio_mixer_name_has(info.name, "output")))
                file->playback = i;
            if (file->capture == AUDIO_MIXER_NONE &&
                (audio_mixer_name_has(info.name, "capture") ||
                 audio_mixer_name_has(info.name, "record") ||
                 audio_mixer_name_has(info.name, "input")))
                file->capture = i;
        }
        if (audio_mixer_writable(&info) && audio_mixer_name_has(info.name, "mute") &&
            file->mute == AUDIO_MIXER_NONE)
            file->mute = i;
        if (info.type == AUDIO_CONTROL_ENUMERATED &&
            (info.access & AUDIO_CONTROL_WRITE) && file->source == AUDIO_MIXER_NONE &&
            (audio_mixer_name_has(info.name, "source") ||
             audio_mixer_name_has(info.name, "route")))
            file->source = i;
    }
    if (file->playback == AUDIO_MIXER_NONE) file->playback = fallback;
    file->devmask = 0;
    file->recmask = 0;
    file->stereodevs = 0;
    if (file->playback != AUDIO_MIXER_NONE) {
        struct audio_control_info info;
        ret = audio_mixer_read_info(file, file->playback, &info);
        if (ret) return ret;
        file->devmask |= SOUND_MASK_VOLUME | SOUND_MASK_PCM | SOUND_MASK_SPEAKER;
        if (info.count >= 2)
            file->stereodevs |= SOUND_MASK_VOLUME | SOUND_MASK_PCM | SOUND_MASK_SPEAKER;
    }
    if (file->capture != AUDIO_MIXER_NONE) {
        struct audio_control_info info;
        ret = audio_mixer_read_info(file, file->capture, &info);
        if (ret) return ret;
        file->devmask |= SOUND_MASK_MIC | SOUND_MASK_LINE;
        file->recmask |= SOUND_MASK_MIC | SOUND_MASK_LINE;
        if (info.count >= 2) file->stereodevs |= SOUND_MASK_MIC | SOUND_MASK_LINE;
    }
    if (file->mute != AUDIO_MIXER_NONE)
        file->devmask |= (uint32_t)1u << SOUND_MIXER_MUTE;
    if (file->source != AUDIO_MIXER_NONE) file->caps |= SOUND_CAP_EXCL_INPUT;
    file->recsrc = file->recmask & (SOUND_MASK_LINE | SOUND_MASK_MIC);
    if (!file->recsrc && file->recmask) file->recsrc = file->recmask & -file->recmask;
    return 0;
}

int audio_mixer_open_card(uint32_t card, struct audio_mixer_file **out)
{
    if (!out) return -EINVAL;
    for (uint32_t i = 0; i < AUDIO_MIXER_FILE_MAX; ++i) {
        struct audio_mixer_file *file = &audio_mixer_files[i];
        if (file->used) continue;
        *file = (struct audio_mixer_file){.used = 1, .card = card,
                                          .playback = AUDIO_MIXER_NONE,
                                          .capture = AUDIO_MIXER_NONE,
                                          .mute = AUDIO_MIXER_NONE,
                                          .source = AUDIO_MIXER_NONE};
        int ret = audio_mixer_select_controls(file);
        if (ret) { *file = (struct audio_mixer_file){0}; return ret; }
        *out = file;
        return 0;
    }
    return -ENFILE;
}

int audio_mixer_close_file(struct audio_mixer_file *file)
{
    if (!file || !file->used) return -EBADF;
    *file = (struct audio_mixer_file){0};
    return 0;
}

static int audio_mixer_percent_to_value(const struct audio_control_info *info,
                                        uint32_t percent, int64_t *out)
{
    if (!info || !out || percent > 100 || info->max < info->min) return -EINVAL;
    if (info->type == AUDIO_CONTROL_BOOLEAN) {
        *out = percent ? 1 : 0;
        return 0;
    }
    uint64_t range = (uint64_t)(info->max - info->min);
    uint64_t whole = range / 100u;
    uint64_t rem = range % 100u;
    uint64_t delta = whole * percent + (rem * percent + 50u) / 100u;
    if (delta > range) delta = range;
    *out = info->min + (int64_t)delta;
    return 0;
}

static int audio_mixer_value_to_percent(const struct audio_control_info *info,
                                        int64_t value, uint32_t *out)
{
    if (!info || !out || info->max < info->min || value < info->min || value > info->max)
        return -ERANGE;
    if (info->type == AUDIO_CONTROL_BOOLEAN) {
        *out = value ? 100u : 0u;
        return 0;
    }
    uint64_t range = (uint64_t)(info->max - info->min);
    if (!range) { *out = 100u; return 0; }
    uint64_t delta = (uint64_t)(value - info->min);
    *out = (uint32_t)((delta * 100u + range / 2u) / range);
    if (*out > 100u) *out = 100u;
    return 0;
}

static int audio_mixer_volume_control(const struct audio_mixer_file *file, unsigned channel,
                                      uint32_t *control)
{
    if (!file || !control) return -EINVAL;
    switch (channel) {
    case SOUND_MIXER_VOLUME:
    case SOUND_MIXER_PCM:
    case SOUND_MIXER_SPEAKER:
        *control = file->playback; return file->playback == AUDIO_MIXER_NONE ? -EOPNOTSUPP : 0;
    case SOUND_MIXER_LINE:
    case SOUND_MIXER_MIC:
    case SOUND_MIXER_RECLEV:
        *control = file->capture; return file->capture == AUDIO_MIXER_NONE ? -EOPNOTSUPP : 0;
    default:
        return -EOPNOTSUPP;
    }
}

static int audio_mixer_get_volume(struct audio_mixer_file *file, uint32_t control, int *value)
{
    struct audio_control_info info;
    struct audio_control_value current;
    int ret = audio_mixer_read_info(file, control, &info);
    if (ret) return ret;
    ret = audio_card_control_read(file->card, control, &current);
    if (ret) return ret;
    uint32_t left, right;
    ret = audio_mixer_value_to_percent(&info, current.values[0], &left);
    if (ret) return ret;
    right = left;
    if (info.count >= 2) {
        ret = audio_mixer_value_to_percent(&info, current.values[1], &right);
        if (ret) return ret;
    }
    *value = (int)(left | (right << 8));
    return 0;
}

static int audio_mixer_set_volume(struct audio_mixer_file *file, uint32_t control, int value)
{
    struct audio_control_info info;
    struct audio_control_value next;
    int ret = audio_mixer_read_info(file, control, &info);
    if (ret) return ret;
    uint32_t left = (uint32_t)value & 0xffu;
    uint32_t right = ((uint32_t)value >> 8) & 0xffu;
    if (left > 100 || right > 100) return -EINVAL;
    ret = audio_mixer_percent_to_value(&info, left, &next.values[0]);
    if (ret) return ret;
    next.values[1] = next.values[0];
    if (info.count >= 2) {
        ret = audio_mixer_percent_to_value(&info, right, &next.values[1]);
        if (ret) return ret;
    }
    return audio_control_write_shared(file->card, control, &next);
}

static int audio_mixer_get_recsrc(struct audio_mixer_file *file, int *value)
{
    if (!file || !value) return -EINVAL;
    if (file->source == AUDIO_MIXER_NONE) { *value = (int)file->recsrc; return 0; }
    struct audio_control_info info;
    struct audio_control_value current;
    int ret = audio_mixer_read_info(file, file->source, &info);
    if (ret) return ret;
    ret = audio_card_control_read(file->card, file->source, &current);
    if (ret) return ret;
    if (current.values[0] < 0 || (uint64_t)current.values[0] >= info.items) return -ERANGE;
    *value = (int)audio_mixer_source_bit(info.item_names[current.values[0]],
                                         (uint32_t)current.values[0]);
    return 0;
}

static int audio_mixer_set_recsrc(struct audio_mixer_file *file, int value)
{
    if (!file || value <= 0 || ((uint32_t)value & ~file->recmask)) return -EINVAL;
    if ((uint32_t)value & ((uint32_t)value - 1u)) return -EINVAL;
    if (file->source == AUDIO_MIXER_NONE) { file->recsrc = (uint32_t)value; return 0; }
    struct audio_control_info info;
    struct audio_control_value next = {0};
    int ret = audio_mixer_read_info(file, file->source, &info);
    if (ret) return ret;
    uint32_t selected = 0;
    for (; selected < info.items && selected < 16; ++selected) {
        if (audio_mixer_source_bit(info.item_names[selected], selected) == (uint32_t)value) break;
    }
    if (selected >= info.items || selected >= 16) return -EINVAL;
    next.values[0] = selected;
    ret = audio_control_write_shared(file->card, file->source, &next);
    if (!ret) {
        file->recsrc = (uint32_t)value;
    }
    return ret;
}

int audio_mixer_ioctl_file(struct audio_mixer_file *file, uint64_t request, void *argument)
{
    if (!file || !file->used) return -EBADF;
    int *value = argument;
    switch (request) {
    case SOUND_MIXER_READ_DEVMASK:
        if (!value) return -EFAULT; *value = (int)file->devmask; return 0;
    case SOUND_MIXER_READ_RECMASK:
        if (!value) return -EFAULT; *value = (int)file->recmask; return 0;
    case SOUND_MIXER_READ_STEREODEVS:
        if (!value) return -EFAULT; *value = (int)file->stereodevs; return 0;
    case SOUND_MIXER_READ_CAPS:
        if (!value) return -EFAULT; *value = (int)file->caps; return 0;
    case SOUND_MIXER_READ_RECSRC:
        if (!value) return -EFAULT; return audio_mixer_get_recsrc(file, value);
    case SOUND_MIXER_WRITE_RECSRC:
        if (!value) return -EFAULT; return audio_mixer_set_recsrc(file, *value);
    case SOUND_MIXER_INFO: {
        if (!argument) return -EFAULT;
        mixer_info *info = argument;
        struct audio_card_identity identity;
        int ret = audio_card_identity(file->card, &identity);
        if (ret) return ret;
        memset(info, 0, sizeof(*info));
        for (uint32_t i = 0; i + 1 < sizeof(info->id) && identity.id[i]; ++i) info->id[i] = identity.id[i];
        for (uint32_t i = 0; i + 1 < sizeof(info->name) && identity.name[i]; ++i) info->name[i] = identity.name[i];
        return 0;
    }
    default:
        break;
    }
    unsigned channel = _IOC_NR(request);
    if (_IOC_TYPE(request) != 'M')
        return -ENOTTY;
    if (channel == SOUND_MIXER_MUTE) {
        if (request != SOUND_MIXER_READ_MUTE && request != SOUND_MIXER_WRITE_MUTE)
            return -ENOTTY;
        if (file->mute == AUDIO_MIXER_NONE) return -EOPNOTSUPP;
        if (!value) return -EFAULT;
        if (request == SOUND_MIXER_READ_MUTE) return audio_mixer_get_volume(file, file->mute, value);
        return audio_mixer_set_volume(file, file->mute, *value);
    }
    if (channel >= SOUND_MIXER_NRDEVICES) return -ENOTTY;
    uint32_t control;
    int ret = audio_mixer_volume_control(file, channel, &control);
    if (ret) return ret;
    if (!value) return -EFAULT;
    if (request == MIXER_READ(channel)) return audio_mixer_get_volume(file, control, value);
    if (request == MIXER_WRITE(channel)) return audio_mixer_set_volume(file, control, *value);
    return -ENOTTY;
}

int audio_mixer_open(struct task *task, struct task_file *file)
{
    (void)task;
    if (!file || !(file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER)) return -EBADF;
    if (file->node.volume_id == 0) {
        uint32_t card;
        int ret = audio_card_snapshot(0, &card, NULL);
        if (ret) return ret;
        file->node.volume_id = card;
    }
    return audio_mixer_open_card(file->node.volume_id, &file->audio_mixer_file);
}

int audio_mixer_close(struct task_file *file)
{
    if (!file || !file->audio_mixer_file) return -EBADF;
    int ret = audio_mixer_close_file(file->audio_mixer_file);
    file->audio_mixer_file = NULL;
    return ret;
}

int audio_mixer_ioctl(struct task *task, struct task_file *file,
                      uint64_t request, uint64_t address)
{
    (void)task;
    if (!file || !file->audio_mixer_file) return -EBADF;
    uint32_t size = _IOC_SIZE(request);
    if (!size) return audio_mixer_ioctl_file(file->audio_mixer_file, request, NULL);
    if (size > 1024u || !address) return -EFAULT;
    uint32_t direction = _IOC_DIR(request);
    int copy_in = (direction & _IOC_WRITE) != 0;
    int copy_out = (direction & _IOC_READ) != 0;
    if ((copy_in && !user_range_ok(address, size)) ||
        (copy_out && !user_range_writable(address, size))) return -EFAULT;
    union { max_align_t align; uint8_t bytes[1024]; } local = {0};
    if (copy_in) __builtin_memcpy(local.bytes, (const void *)(uintptr_t)address, size);
    int ret = audio_mixer_ioctl_file(file->audio_mixer_file, request, local.bytes);
    if (!ret && copy_out) __builtin_memcpy((void *)(uintptr_t)address, local.bytes, size);
    return ret;
}

short audio_mixer_poll(const struct task_file *file, short events)
{
    if (!file || !file->audio_mixer_file || !file->audio_mixer_file->used) return POLLNVAL;
    if (audio_card_index(file->audio_mixer_file->card) < 0) return POLLERR | POLLHUP;
    return events & (POLLOUT | POLLWRNORM);
}
