#include "../internal.h"

static void evdev_copy_text(char *dst, uint32_t capacity, const char *src);


/**
 * @brief Resolve the VT that owns an evdev-opening task.
 * @param task Current task; may be null.
 * @return Fixed VT number, or zero for an unbound raw evdev client.
 *
 * Xorg opens its input devices before it switches the VT to KD_GRAPHICS.  The
 * active-graphics bit is therefore not a reliable open-time hint.  Its
 * inherited tty endpoint is stable across that transition, so prefer the
 * controlling tty and then any explicit VT endpoint in the descriptor table.
 */
uint32_t task_evdev_vt_hint(const struct task *task)
{
    const struct task_fd_table_state *fds;
    if (!task) return 0;
    if (pty_vt_number(task->controlling_pty_id))
        return task->controlling_pty_id;
    fds = sched_task_fds(task);
    for (uint32_t i = 0; i < SCHED_TASK_PTY_FD_MAX; ++i) {
        const struct task_pty_fd *entry = &fds->pty_fds[i];
        if (entry->used && entry->endpoint == TASK_PTY_ENDPOINT_SLAVE &&
            pty_vt_number(entry->pty_id))
            return entry->pty_id;
    }
    return 0;
}

/**
 * @brief Bind an evdev descriptor to a VT once its owner opens that VT.
 * @param task Current task; may be null for an invalid syscall context.
 * @param file Evdev open-file description to update.
 * @return Nothing; an already-bound or otherwise unresolvable descriptor is unchanged.
 *
 * Xorg can open /dev/input/event* before xf86OpenConsole creates its explicit
 * /dev/ttyN endpoint.  The open-time hint is then zero and would leave the
 * descriptor on the raw stream forever.  Retry the same stable descriptor
 * lookup at the first read/poll after the tty endpoint exists.  Keep the
 * existing cursor so VT hand-off snapshots that were queued in between are
 * still delivered to the newly bound consumer.
 */
void task_evdev_bind_vt(struct task *task, struct task_file *file)
{
    uint32_t number;
    if (!task || !file || file->input_vt || file->input_vt_manual) return;
    number = task_evdev_vt_hint(task);
    if (number) file->input_vt = number;
}

static void evdev_copy_text(char *dst, uint32_t capacity, const char *src)
{
    uint32_t i = 0;
    if (!dst || !capacity) return;
    while (src && src[i] && i + 1U < capacity) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

/**
 * @brief Write global VT query results after a VT descriptor has been resolved.
 * @param request VT_OPENQRY or VT_GETSTATE.
 * @param user_arg User output address, checked for writable access here.
 * @return Zero on success or a negative errno.
 */
int task_vt_query_ioctl(uint64_t request, uint64_t user_arg)
{
    if (request == VT_OPENQRY) {
        if (!user_range_writable(user_arg, sizeof(int))) return -RELIEFOS_EFAULT;
        int free_vt = pty_vt_open_query();
        *(int *)(uintptr_t)user_arg = free_vt;
        return 0;
    }
    if (!user_range_writable(user_arg, sizeof(struct vt_stat))) return -RELIEFOS_EFAULT;
    struct vt_stat *state = (struct vt_stat *)(uintptr_t)user_arg;
    state->v_active = (unsigned short)pty_vt_active();
    state->v_state = (unsigned short)pty_vt_state_bitmap();
    return 0;
}

int task_evdev_ioctl(struct task_file *file, uint64_t request,
                            uint64_t user_arg)
{
    uint32_t device_kind;
    uint32_t size;
    if (!file || !(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
        (file->node.first_cluster != STORAGE_DEV_KIND_KEYBOARD &&
         file->node.first_cluster != STORAGE_DEV_KIND_MOUSE)) {
        return -RELIEFOS_ENOTTY;
    }
    device_kind = file->node.first_cluster;
    if (request == RELIEFOS_EVIOCSVT) {
        uint32_t number;
        if (!user_range_ok(user_arg, sizeof(number))) return -RELIEFOS_EFAULT;
        __builtin_memcpy(&number, (const void *)(uintptr_t)user_arg, sizeof(number));
        if (number && !pty_vt_id(number)) return -RELIEFOS_EINVAL;
        file->input_vt = number;
        file->input_vt_manual = 1;
        file->aux = input_evdev_cursor_now();
        return 0;
    }
    if (request == EVIOCGVERSION) {
        if (!user_range_ok(user_arg, sizeof(int))) return -RELIEFOS_EFAULT;
        *(int *)(uintptr_t)user_arg = 0x00010001;
        return 0;
    }
    if (request == EVIOCGID) {
        struct input_id id = {
            .bustype = BUS_I8042,
            .vendor = 0x0001,
            .product = device_kind == STORAGE_DEV_KIND_KEYBOARD ? 0x0001 : 0x0002,
            .version = 0x0100,
        };
        if (!user_range_ok(user_arg, sizeof(id))) return -RELIEFOS_EFAULT;
        *(struct input_id *)(uintptr_t)user_arg = id;
        return 0;
    }
    if (request == EVIOCGRAB) {
        /* Linux tests the full scalar argument for nonzero without dereferencing. */
        int64_t token;
        token = input_evdev_grab(device_kind, file->aux2, user_arg != 0,
                                 sched_current_pid());
        if (token < 0) return (int)token;
        file->aux2 = (uint64_t)token;
        return 0;
    }
    if (_IOC_TYPE(request) != 'E') {
        return -RELIEFOS_ENOTTY;
    }
    size = _IOC_SIZE(request);
    if (_IOC_NR(request) == 0x03 && _IOC_DIR(request) == _IOC_READ) {
        /* EVIOCGREP: the default key repeat delay and period. */
        unsigned int rep[2] = {250u, 667u};
        if (size < sizeof(rep) || !user_range_writable(user_arg, sizeof(rep)))
            return -RELIEFOS_EFAULT;
        __builtin_memcpy((void *)(uintptr_t)user_arg, rep, sizeof(rep));
        return 0;
    }
    if (_IOC_NR(request) == 0x03 && _IOC_DIR(request) == _IOC_WRITE) {
        /* EVIOCSREP: key repeat parameters are accepted and ignored. */
        return 0;
    }
    if (_IOC_NR(request) == 0x19 && _IOC_DIR(request) == _IOC_READ) {
        uint64_t leds = device_kind == STORAGE_DEV_KIND_KEYBOARD && input_caps_lock_active()
                            ? 1ULL << LED_CAPSL : 0;
        uint32_t count = size < sizeof(leds) ? size : sizeof(leds);
        if (count && !user_range_writable(user_arg, count)) return -RELIEFOS_EFAULT;
        for (uint32_t i = 0; i < count; ++i)
            ((uint8_t *)(uintptr_t)user_arg)[i] = (uint8_t)(leds >> (8U * i));
        return (int)count;
    }
    if ((_IOC_NR(request) == 0x1a || _IOC_NR(request) == 0x1b) &&
        _IOC_DIR(request) == _IOC_READ) {
        /* EVIOCGSND and EVIOCGSW: no sounds or switch states exist on these
         * devices, so the bitmaps are all zeroes. */
        uint32_t count = size < sizeof(uint64_t) ? size : sizeof(uint64_t);
        if (count && !user_range_writable(user_arg, count)) return -RELIEFOS_EFAULT;
        for (uint32_t i = 0; i < count; ++i)
            ((uint8_t *)(uintptr_t)user_arg)[i] = 0;
        return (int)count;
    }
    if (!size || !user_range_ok(user_arg, size)) {
        return -RELIEFOS_EFAULT;
    }
    if (_IOC_NR(request) == 0x06 || _IOC_NR(request) == 0x07 ||
        _IOC_NR(request) == 0x08) {
        const char *value;
        if (_IOC_NR(request) == 0x06) {
            value = device_kind == STORAGE_DEV_KIND_KEYBOARD
                        ? "LeonOS PS/2 Keyboard" : "LeonOS PS/2 Mouse";
        } else if (_IOC_NR(request) == 0x07) {
            value = device_kind == STORAGE_DEV_KIND_KEYBOARD
                        ? "platform/i8042/serio0" : "platform/i8042/serio1";
        } else {
            /* EVIOCGUNIQ: the virtual devices carry no unique serial. */
            value = "";
        }
        evdev_copy_text((char *)(uintptr_t)user_arg, size, value);
        return 0;
    }
    if (_IOC_NR(request) == 0x09 || _IOC_NR(request) == 0x0a) {
        /* EVIOCGPROP and EVIOCGMTSLOTS: no input properties and no
         * multitouch slots are advertised on these devices. */
        for (uint32_t i = 0; i < size; ++i)
            ((uint8_t *)(uintptr_t)user_arg)[i] = 0;
        return (int)size;
    }
    if (_IOC_NR(request) == 0x18) {
        input_evdev_key_state((void *)(uintptr_t)user_arg, size);
        return 0;
    }
    if (_IOC_NR(request) >= 0x40 && _IOC_NR(request) < 0x80) {
        if (device_kind != STORAGE_DEV_KIND_MOUSE || size != sizeof(struct input_absinfo))
            return -RELIEFOS_EINVAL;
        return input_evdev_absinfo(_IOC_NR(request) - 0x40,
                                   (struct input_absinfo *)(uintptr_t)user_arg);
    }
    if (_IOC_NR(request) >= 0x20 && _IOC_NR(request) <= 0x20 + EV_MAX) {
        uint32_t event_type = _IOC_NR(request) - 0x20U;
        input_evdev_capabilities(device_kind, event_type,
                                 (void *)(uintptr_t)user_arg, size);
        return 0;
    }
    return -RELIEFOS_ENOTTY;
}
