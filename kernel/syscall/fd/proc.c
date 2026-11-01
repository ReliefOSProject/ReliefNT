/* SPDX-License-Identifier: Apache-2.0 */
#include "../internal.h"
#include <reliefnt/proc_stats.h>

/** @brief Emit a Linux timerfd duration as a seconds/nanoseconds pair.
 * @param s Required output. @param ticks Monotonic remaining/interval ticks.
 * No allocation, timer update, or reference ownership change.
 */
static void proc_timer_duration(struct text_stream *s, uint64_t ticks)
{
    text_string(s, "(");
    text_unsigned(s, ticks / RELIEFNT_TICK_HZ);
    text_string(s, ", ");
    text_unsigned(s, (ticks % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
    text_string(s, ")\n");
}

/** @brief Emit eventfd, timerfd and epoll metadata from the canonical OFD.
 * @param file Borrowed live description. @param s Required streaming output.
 * @return Zero or backing getattr errno; task execution ownership is required.
 * Does not consume readiness, expire timers, expose pointers, or retain targets.
 */
int proc_fd_details(struct task_file *file, struct text_stream *s)
{
    if (file->flags & TASK_FILE_FLAG_EVENTFD) {
        text_string(s, "eventfd-count: ");
        text_hex(s, file->aux, 16);
        text_string(s, "\neventfd-semaphore: ");
        text_unsigned(s, file->aux2);
        text_string(s, "\n");
    }
    if ((file->flags & TASK_FILE_FLAG_TIMERFD) && file->aux) {
        const struct task_timerfd *t = (const void *)(uintptr_t)file->aux;
        uint64_t now = time_ticks(), expirations = t->expirations,
                 remaining = t->expiry_tick > now ? t->expiry_tick - now : 0;
        if (t->expiry_tick && t->expiry_tick <= now) {
            uint64_t periods =
                t->interval_ticks ? (now - t->expiry_tick) / t->interval_ticks + 1 : 1;
            expirations = periods > UINT64_MAX - expirations ? UINT64_MAX : expirations + periods;
            if (t->interval_ticks)
                remaining = t->interval_ticks - (now - t->expiry_tick) % t->interval_ticks;
        }
        text_string(s, "clockid: ");
        text_signed(s, t->clockid);
        text_string(s, "\nticks: ");
        text_unsigned(s, expirations);
        text_string(s, "\nsettime flags: ");
        text_hex(s, t->flags, 8);
        text_string(s, "\nit_value: ");
        proc_timer_duration(s, remaining);
        text_string(s, "it_interval: ");
        proc_timer_duration(s, t->interval_ticks);
    }
    if ((file->flags & TASK_FILE_FLAG_EPOLL) && file->aux) {
        const struct task_epoll *epoll = (const void *)(uintptr_t)file->aux;
        for (uint32_t i = 0; i < epoll->entries; ++i) {
            const struct task_epoll_entry *item = &epoll->item[i];
            if (!item->active || !item->watch)
                continue;
            const struct task_file *target = item->watch->file;
            const struct vfs_path *path = target             ? &target->path_ref
                                          : item->watch->pty ? &item->watch->pty->path_ref
                                                             : NULL;
            uint64_t ino = target ? target->socket_ino : 0, device = 0;
            if (path && path->mount) {
                struct linux_stat_abi st;
                int rc = vfs_path_stat(path, &st);
                if (rc)
                    return rc;
                ino = st.st_ino;
                device = st.st_dev;
            }
            text_string(s, "tfd: ");
            text_signed(s, item->fd);
            text_string(s, " events: ");
            text_hex(s, item->events, 8);
            text_string(s, " data: ");
            text_hex(s, item->data, 16);
            text_string(s, " pos: ");
            text_unsigned(s, target ? target->offset : 0);
            text_string(s, " ino: ");
            text_hex(s, ino, 16);
            text_string(s, " sdev: ");
            text_hex(s, device, 16);
            text_string(s, "\n");
        }
    }
    return 0;
}
