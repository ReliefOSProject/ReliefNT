#include "../internal.h"


static uint16_t reliefos_fb_palette[4][256];
static uint32_t reliefos_fb_blank;


/**
 * @brief Dispatch an already-normalised syscall within this ABI category.
 * @param number Linux or ReliefOS syscall number selected by the common dispatcher.
 * @param a0 First native ABI argument; pointers remain subject to the original checks.
 * @param a1 Second native ABI argument.
 * @param a2 Third native ABI argument.
 * @param a3 Fourth native ABI argument.
 * @param a4 Fifth native ABI argument.
 * @param a5 Sixth native ABI argument.
 * @return Original result, negative errno, or internal blocking/retry status.
 * @note The caller retains the kernel execution transaction and descriptor pins.
 */
int64_t syscall_legacy_ioctl(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5;

    if (number == LINUX_SYS_IOCTL) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_io(task, (int)a0);
        if (file && file->kind == TASK_FILE_KIND_SIGNALFD) {
            /* Generic ioctl requests have already been handled. */
            return -LINUX_ENOTTY;
        }
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_UNIX))
            return task_socket_ioctl(file, a1, a2);
        if (file && (file->flags & TASK_FILE_FLAG_SOCKET_INET))
            return task_net_control(file, (uint32_t)a1, a2);
        if (file && (file->node.flags & STORAGE_NODE_FLAG_DEV_NODE) &&
            file->node.first_cluster == STORAGE_DEV_KIND_DRIVERCTL) {
            return syscall_driver_control((uint32_t)a1, a2);
        }
        int evdev_ret = task_evdev_ioctl(file, a1, a2);
        if (evdev_ret != -RELIEFOS_ENOTTY) {
            return evdev_ret;
        }
        if (file && (file->node.flags & STORAGE_NODE_FLAG_AUDIO_CONTROL)) {
            return audio_control_ioctl(task, file, a1, a2);
        }
        if (file && (file->node.flags & STORAGE_NODE_FLAG_AUDIO_MIXER)) {
            return audio_mixer_ioctl(task, file, a1, a2);
        }
        if (file && (file->node.flags & STORAGE_NODE_FLAG_AUDIO_PCM)) {
            return audio_device_ioctl(task, file, a1, a2);
        }
        if (file && (file->node.flags & STORAGE_NODE_FLAG_AUDIO_TIMER)) {
            return audio_timer_ioctl(task, file, a1, a2);
        }
        if (file && task_device_is(file, STORAGE_DEV_KIND_AUDIO)) {
            return audio_oss_ioctl(task, file, a1, a2);
        }
    }

    if (number == LINUX_SYS_IOCTL) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        uint32_t disk_id;
        int32_t partition_index;
        uint64_t first_lba;
        uint64_t sector_count;
        if (task_block_device(file, &disk_id, &partition_index)) {
            if (a1 == BLKGETSIZE64 || a1 == BLKGETSIZE) {
                if (!a2 || !user_range_ok(a2, sizeof(uint64_t))) {
                    return -RELIEFOS_EFAULT;
                }
                {
                    int ret = storage_disk_block_info(disk_id, partition_index,
                                                      &first_lba, &sector_count);
                    if (ret < 0) return ret;
                }
                *(uint64_t *)(uintptr_t)a2 = a1 == BLKGETSIZE64
                                                  ? sector_count * 512ULL
                                                  : sector_count;
                return 0;
            }
            if (a1 == BLKSSZGET) {
                if (!a2 || !user_range_ok(a2, sizeof(int))) return -RELIEFOS_EFAULT;
                *(int *)(uintptr_t)a2 = 512;
                return 0;
            }
            if (a1 == BLKROGET) {
                if (!a2 || !user_range_ok(a2, sizeof(int))) return -RELIEFOS_EFAULT;
                *(int *)(uintptr_t)a2 = 0;
                return 0;
            }
            if (a1 == BLKRRPART) {
                /* Linux requires CAP_SYS_ADMIN for partition-table rescans. */
                if (!task || !(task->cap_effective & (1ULL << CAP_SYS_ADMIN))) {
                    return -RELIEFOS_EACCES;
                }
                if (partition_index >= 0) return -RELIEFOS_EINVAL;
                return storage_disk_block_reread(disk_id);
            }
            if (a1 == BLKROSET) {
                return -RELIEFOS_EPERM;
            }
        }
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == FBIOGET_VSCREENINFO || a1 == FBIOPUT_VSCREENINFO ||
         a1 == FBIOGET_FSCREENINFO || a1 == FBIOPAN_DISPLAY ||
         a1 == FBIOGETCMAP || a1 == FBIOPUTCMAP || a1 == FBIOBLANK ||
         a1 == RELIEFOS_FBIOGET_CAPABILITIES || a1 == RELIEFOS_FBIOUPDATE_REGION ||
         a1 == RELIEFOS_FBIOBLIT)) {
        struct task *task = sched_current_task();
        struct task_file *file = task_file_for_fd(task, (int)a0);
        const struct framebuffer *fb = framebuffer_get();
        if (!file || !(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
            file->node.first_cluster != STORAGE_DEV_KIND_FB0 || !fb ||
            !fb->available) {
            return -RELIEFOS_ENOTTY;
        }
        /* The native present path requires a live claim on the display: a
         * controlling VT, the active one, claimed in graphics mode.  The
         * standard fbdev mode set has no such gate in Linux — Xorg calls
         * FBIOPUT_VSCREENINFO during ScreenInit from a server process with
         * no controlling terminal — so it must keep working on any open
         * /dev/fb0 descriptor. */
        if (a1 == RELIEFOS_FBIOBLIT) {
            if (!task || !pty_vt_number(task->controlling_pty_id)) return -RELIEFOS_EPERM;
            if (pty_vt_active() != task->controlling_pty_id) return -RELIEFOS_EAGAIN;
            if (!pty_vt_graphical_active())
                return -RELIEFOS_EAGAIN;
        }
        if (a1 == RELIEFOS_FBIOBLIT) {
            struct reliefos_fb_present update;
            if (!user_range_ok(a2, sizeof(update))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(&update, (const void *)(uintptr_t)a2, sizeof(update));
            if (update.pixels && update.stride < update.width) return -RELIEFOS_EINVAL;
            if (!update.width || !update.height || update.x >= fb->width || update.y >= fb->height)
                return 0;
            if (update.width > fb->width - update.x) update.width = fb->width - update.x;
            if (update.height > fb->height - update.y) update.height = fb->height - update.y;
            if (update.pixels) {
                uint64_t bytes = ((uint64_t)(update.height - 1) * update.stride + update.width) * 4;
                if (!user_range_ok(update.pixels, bytes)) return -RELIEFOS_EFAULT;
                framebuffer_blit(update.x, update.y, update.width, update.height,
                                 update.stride, (const uint32_t *)(uintptr_t)update.pixels);
            } else {
                framebuffer_rect(update.x, update.y, update.width, update.height, update.color);
            }
            framebuffer_present_region(update.x, update.y, update.width, update.height);
            return 0;
        }
        if (a1 == FBIOBLANK) {
            if (a2 > FB_BLANK_POWERDOWN) return -RELIEFOS_EINVAL;
            reliefos_fb_blank = (uint32_t)a2;
            if (a2 == FB_BLANK_UNBLANK)
                framebuffer_present_region(0, 0, fb->width, fb->height);
            return 0;
        }
        if (a1 == FBIOGETCMAP || a1 == FBIOPUTCMAP) {
            struct fb_cmap cmap;
            uint32_t end;
            if (!user_range_ok(a2, sizeof(cmap))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(&cmap, (const void *)(uintptr_t)a2, sizeof(cmap));
            if (cmap.start > 256u || cmap.len > 256u - cmap.start)
                return -RELIEFOS_EINVAL;
            end = cmap.start + cmap.len;
            if (!cmap.len) return 0;
            if (!cmap.red || !cmap.green || !cmap.blue)
                return -RELIEFOS_EFAULT;
            if (a1 == FBIOPUTCMAP) {
                if (!user_range_ok(cmap.red, cmap.len * sizeof(uint16_t)) ||
                    !user_range_ok(cmap.green, cmap.len * sizeof(uint16_t)) ||
                    !user_range_ok(cmap.blue, cmap.len * sizeof(uint16_t)) ||
                    (cmap.transp && !user_range_ok(cmap.transp,
                                                    cmap.len * sizeof(uint16_t))))
                    return -RELIEFOS_EFAULT;
                for (uint32_t i = cmap.start; i < end; ++i) {
                    reliefos_fb_palette[0][i] = ((const uint16_t *)(uintptr_t)cmap.red)[i - cmap.start];
                    reliefos_fb_palette[1][i] = ((const uint16_t *)(uintptr_t)cmap.green)[i - cmap.start];
                    reliefos_fb_palette[2][i] = ((const uint16_t *)(uintptr_t)cmap.blue)[i - cmap.start];
                    reliefos_fb_palette[3][i] = cmap.transp
                        ? ((const uint16_t *)(uintptr_t)cmap.transp)[i - cmap.start] : 0;
                }
            } else {
                if (!user_range_writable(cmap.red, cmap.len * sizeof(uint16_t)) ||
                    !user_range_writable(cmap.green, cmap.len * sizeof(uint16_t)) ||
                    !user_range_writable(cmap.blue, cmap.len * sizeof(uint16_t)) ||
                    (cmap.transp && !user_range_writable(cmap.transp,
                                                          cmap.len * sizeof(uint16_t))))
                    return -RELIEFOS_EFAULT;
                for (uint32_t i = cmap.start; i < end; ++i) {
                    ((uint16_t *)(uintptr_t)cmap.red)[i - cmap.start] = reliefos_fb_palette[0][i];
                    ((uint16_t *)(uintptr_t)cmap.green)[i - cmap.start] = reliefos_fb_palette[1][i];
                    ((uint16_t *)(uintptr_t)cmap.blue)[i - cmap.start] = reliefos_fb_palette[2][i];
                    if (cmap.transp)
                        ((uint16_t *)(uintptr_t)cmap.transp)[i - cmap.start] = reliefos_fb_palette[3][i];
                }
            }
            return 0;
        }
        /* Panning display is the Linux fbdev flush request: VMware SVGA
         * scanout only refreshes from VRAM when the FIFO receives an
         * update command, and mmap writers bypass every other path. */
        if (a1 == FBIOPAN_DISPLAY) {
            framebuffer_present_region(0, 0, fb->width, fb->height);
            return 0;
        }
        if (a1 == RELIEFOS_FBIOUPDATE_REGION) {
            uint32_t region[4];
            if (!user_range_ok(a2, sizeof(region))) return -RELIEFOS_EFAULT;
            __builtin_memcpy(region, (const void *)(uintptr_t)a2, sizeof(region));
            framebuffer_present_region(region[0], region[1], region[2], region[3]);
            return 0;
        }
        if (!a2) {
            return -RELIEFOS_ENOTTY;
        }
        if (a1 == RELIEFOS_FBIOGET_CAPABILITIES) {
            if (!user_range_ok(a2, sizeof(struct reliefos_fb_capabilities)))
                return -RELIEFOS_EFAULT;
            *(struct reliefos_fb_capabilities *)(uintptr_t)a2 =
                (struct reliefos_fb_capabilities){
                    .bytes_per_pixel = fb->bytes_per_pixel,
                    .capabilities = fb->capabilities,
                    .max_width = fb->max_width,
                    .max_height = fb->max_height,
                    .max_bytes = fb->max_bytes,
                    .backend = fb->backend,
                };
            return 0;
        }
        if (a1 == FBIOGET_VSCREENINFO) {
            struct fb_var_screeninfo info;
            if (!user_range_ok(a2, sizeof(info))) return -RELIEFOS_EFAULT;
            info = (struct fb_var_screeninfo){
                .xres = fb->width,
                .yres = fb->height,
                .xres_virtual = fb->width,
                .yres_virtual = fb->height,
                .bits_per_pixel = fb->bpp,
                .red = { .offset = fb->red_field_position,
                         .length = fb->red_mask_size },
                .green = { .offset = fb->green_field_position,
                           .length = fb->green_mask_size },
                .blue = { .offset = fb->blue_field_position,
                          .length = fb->blue_mask_size },
            };
            *(struct fb_var_screeninfo *)(uintptr_t)a2 = info;
            return 0;
        }
        if (a1 == FBIOGET_FSCREENINFO) {
            struct fb_fix_screeninfo info;
            if (!user_range_ok(a2, sizeof(info))) return -RELIEFOS_EFAULT;
            if (!framebuffer_fbdev_get_fix_info(fb, &info))
                return -RELIEFOS_EIO;
            *(struct fb_fix_screeninfo *)(uintptr_t)a2 = info;
            return 0;
        }
        return syscall_fbdev_put_var(a2);
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == VT_OPENQRY || a1 == VT_GETMODE || a1 == VT_SETMODE ||
         a1 == VT_GETSTATE || a1 == VT_RELDISP ||
         a1 == RELIEFOS_VT_GETGENERATION || a1 == VT_ACTIVATE ||
         a1 == VT_WAITACTIVE || a1 == KDGETMODE || a1 == KDSETMODE ||
         a1 == KDGKBMODE || a1 == KDSKBMODE)) {
        struct task *task = sched_current_task();
        struct task_pty_fd *endpoint = task_pty_endpoint_for_fd(task, (int)a0);
        struct task_file *file = task_file_for_fd(task, (int)a0);
        uint32_t id = endpoint && endpoint->endpoint == TASK_PTY_ENDPOINT_SLAVE
                          ? endpoint->pty_id : task_device_is(file, STORAGE_DEV_KIND_CONSOLE)
                          ? (uint32_t)file->aux : 0u;
        if (!pty_vt_number(id)) return -RELIEFOS_ENOTTY;
        if (a1 == VT_OPENQRY) {
            return task_vt_query_ioctl(a1, a2);
        }
        if (a1 == RELIEFOS_VT_GETGENERATION) {
            if (!user_range_writable(a2, sizeof(uint64_t))) return -RELIEFOS_EFAULT;
            *(uint64_t *)(uintptr_t)a2 = pty_vt_generation();
            return 0;
        }
        if (a1 == VT_GETSTATE) {
            return task_vt_query_ioctl(a1, a2);
        }
        if (a1 == VT_GETMODE) {
            if (!user_range_writable(a2, sizeof(struct vt_mode))) return -RELIEFOS_EFAULT;
            return pty_vt_get_mode(id, (struct vt_mode *)(uintptr_t)a2);
        }
        if (a1 == VT_SETMODE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            if (!user_range_ok(a2, sizeof(struct vt_mode))) return -RELIEFOS_EFAULT;
            return pty_vt_set_mode(id, (const struct vt_mode *)(uintptr_t)a2);
        }
        if (a1 == VT_RELDISP) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            return pty_vt_release_display(id, (int)a2);
        }
        if (a1 == VT_ACTIVATE || a1 == VT_WAITACTIVE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            if (a2 < 1 || a2 > 6) return -RELIEFOS_EINVAL;
            return a1 == VT_ACTIVATE ? pty_vt_switch((uint32_t)a2)
                                    : pty_vt_wait_active((uint32_t)a2);
        }
        if (a1 == KDGETMODE) {
            if (!user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
            *(int *)(uintptr_t)a2 = pty_vt_graphical(id) ? KD_GRAPHICS : KD_TEXT;
            return 0;
        }
        if (a1 == KDGKBMODE) {
            if (!user_range_writable(a2, sizeof(int))) return -RELIEFOS_EFAULT;
            return pty_vt_get_keyboard_mode(id, (int *)(uintptr_t)a2);
        }
        if (a1 == KDSKBMODE) {
            if (task->controlling_pty_id != id &&
                !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG))) return -RELIEFOS_EPERM;
            return pty_vt_set_keyboard_mode(id, (int)a2);
        }
        /* KDSETMODE changes who owns the display, so it follows the same rule
         * as the other VT ioctls: the controlling terminal or the
         * CAP_SYS_TTY_CONFIG capability. */
        if (task->controlling_pty_id != id &&
            !(task->cap_effective & (1ULL << CAP_SYS_TTY_CONFIG)))
            return -RELIEFOS_EPERM;
        if (a2 != KD_TEXT && a2 != KD_GRAPHICS) return -RELIEFOS_EINVAL;
        return pty_vt_set_graphics(id, a2 == KD_GRAPHICS);
    }

    if (number == LINUX_SYS_IOCTL &&
        (a1 == TCGETS || a1 == TCSETS || a1 == TCSETSW || a1 == TCSETSF ||
         a1 == LINUX_TCGETS2 || a1 == LINUX_TCSETS2 ||
         a1 == LINUX_TCSETSW2 || a1 == LINUX_TCSETSF2 ||
         a1 == TIOCGWINSZ || a1 == TIOCSWINSZ || a1 == TIOCGPTN ||
         a1 == TIOCSPTLCK || a1 == TIOCGPTLCK || a1 == TIOCSCTTY ||
         a1 == TIOCGPGRP || a1 == TIOCSPGRP || a1 == TIOCGSID || a1 == TIOCNOTTY)) {
        struct task *task = sched_current_task();
        struct task_pty_fd *endpoint = task_pty_endpoint_for_fd(task, (int)a0);
        struct task_file *file = task_file_for_fd(task, (int)a0);
        int stream = task_pty_stream_for_fd(task, (int)a0);
        uint32_t pty_id = endpoint ? endpoint->pty_id :
            task_device_is(file, STORAGE_DEV_KIND_CONSOLE) ? (uint32_t)file->aux :
            (stream >= 0 && task ? task->pty_id : 0);
        if (!pty_id || !pty_is_active(pty_id)) return -RELIEFOS_ENOTTY;
        int master = endpoint && endpoint->endpoint == TASK_PTY_ENDPOINT_MASTER;
        if (!master && pty_is_hungup(pty_id))
            return a1 == TIOCSPGRP ? -RELIEFOS_ENOTTY : -RELIEFOS_EIO;
        if (a1 == TIOCNOTTY) {
            if (master) return -RELIEFOS_ENOTTY;
            return pty_detach_controlling(pty_id, task->pid);
        }
        if (a1 == TIOCSCTTY) {
            uint32_t access = endpoint ? task_pty_status(endpoint) & RELIEFOS_O_ACCMODE :
                file ? file->flags & RELIEFOS_O_ACCMODE : RELIEFOS_O_RDWR;
            return pty_acquire_controlling(pty_id, task->pid, (int32_t)a2,
                                           access != RELIEFOS_O_WRONLY);
        }
        if (a1 == TIOCGPGRP || a1 == TIOCGSID) {
            if (!master && task->controlling_pty_id != pty_id) return -RELIEFOS_ENOTTY;
            uint32_t value = 0;
            int result = a1 == TIOCGSID ? pty_get_session(pty_id, &value) :
                pty_get_foreground_pgid(pty_id, &value);
            if (result < 0) return result;
            if (!user_range_writable(a2, sizeof(value))) return -RELIEFOS_EFAULT;
            *(uint32_t *)(uintptr_t)a2 = value;
            return 0;
        }
        if (a1 == TIOCSPGRP) {
            int64_t change = pty_check_change(pty_id, task->pid, 22);
            if (change) return change == -RELIEFOS_EIO ? -RELIEFOS_ENOTTY : change;
            if (!user_range_ok(a2, sizeof(uint32_t))) return -RELIEFOS_EFAULT;
            return pty_set_foreground_pgid(pty_id, task->pid, *(uint32_t *)(uintptr_t)a2);
        }
        if (a1 == TIOCGPTN) {
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER ||
                !user_range_ok(a2, sizeof(uint32_t))) return -RELIEFOS_EINVAL;
            *(uint32_t *)(uintptr_t)a2 = pty_id;
            return 0;
        }
        if (a1 == TIOCSPTLCK) {
            int locked;
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER ||
                !user_range_ok(a2, sizeof(int))) return -RELIEFOS_EINVAL;
            locked = *(int *)(uintptr_t)a2;
            if (locked != 0 && locked != 1) return -RELIEFOS_EINVAL;
            return pty_set_lock(pty_id, locked);
        }
        if (a1 == TIOCGPTLCK) {
            int locked;
            if (!endpoint || endpoint->endpoint != TASK_PTY_ENDPOINT_MASTER ||
                !user_range_ok(a2, sizeof(int))) return -RELIEFOS_EINVAL;
            int ret = pty_get_lock(pty_id, &locked);
            if (ret < 0) return ret;
            *(int *)(uintptr_t)a2 = locked;
            return 0;
        }
        if (a1 == TIOCGWINSZ || a1 == TIOCSWINSZ) {
            struct linux_winsize native;
            if (a1 == TIOCGWINSZ) {
                if (!user_range_writable(a2, sizeof(native))) return -RELIEFOS_EFAULT;
                int ret = pty_get_linux_winsize(pty_id, &native);
                if (ret < 0) return ret;
                *(struct linux_winsize *)(uintptr_t)a2 = native;
                return 0;
            }
            if (!user_range_ok(a2, sizeof(native))) return -RELIEFOS_EFAULT;
            native = *(const struct linux_winsize *)(uintptr_t)a2;
            return pty_set_linux_winsize(pty_id, &native);
        }
        {
            struct linux_termios2 termios;
            int extended = a1 == LINUX_TCGETS2 || a1 == LINUX_TCSETS2 ||
                           a1 == LINUX_TCSETSW2 || a1 == LINUX_TCSETSF2;
            int get = a1 == TCGETS || a1 == LINUX_TCGETS2;
            size_t length = extended ? sizeof(termios) : sizeof(struct linux_termios);
            int ret = pty_get_termios(pty_id, &termios);
            if (ret < 0) return ret;
            uint8_t *user = (uint8_t *)(uintptr_t)a2;
            uint8_t *native = (uint8_t *)&termios;
            if (get) {
                if (!user_range_writable(a2, length)) return -RELIEFOS_EFAULT;
                for (size_t i = 0; i < length; ++i) user[i] = native[i];
                return 0;
            }
            int64_t change = pty_check_change(pty_id, task->pid, 22);
            if (change) return change;
            if (!user_range_ok(a2, length)) return -RELIEFOS_EFAULT;
            for (size_t i = 0; i < length; ++i) native[i] = user[i];
            if (a1 == TCSETSF || a1 == LINUX_TCSETSF2) pty_flush_input(pty_id);
            return pty_set_termios(pty_id, &termios);
        }
    }

    if (number == LINUX_SYS_IOCTL) {
        /* Linux reports ENOTTY when neither do_vfs_ioctl() nor the descriptor's
         * ->unlocked_ioctl/->compat_ioctl handler understands a request, and
         * every device backend above uses the same code.  ENOSYS would claim
         * the ioctl syscall itself does not exist. */
        return -RELIEFOS_ENOTTY;
    }

    return -RELIEFOS_ENOSYS;
}
