#include "../internal.h"

static struct task_timerfd *task_timerfd_for_fd(struct task *task, int fd);


int64_t syscall_eventfd_create(uint64_t initial, uint32_t flags)
{
    struct task *task = sched_current_task();
    struct storage_node node = {
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = 0,
    };
    struct task_file *file;
    int fd;
    if (!task || initial > UINT64_MAX - 1ULL ||
        (flags & ~(LINUX_EFD_SEMAPHORE | LINUX_EFD_CLOEXEC | LINUX_EFD_NONBLOCK))) {
        return -RELIEFOS_EINVAL;
    }
    fd = alloc_task_fd(task, &node,
                       TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR |
                       (flags & (LINUX_EFD_CLOEXEC | LINUX_EFD_NONBLOCK)), NULL);
    if (fd < 0) return fd;
    file = task_file_for_fd(task, fd);
    if (!file) return -RELIEFOS_EMFILE;
    file->aux = initial;
    file->aux2 = (flags & LINUX_EFD_SEMAPHORE) != 0;
    return fd;
}

static struct task_timerfd *task_timerfd_for_fd(struct task *task, int fd)
{
    struct task_file *file = task_file_for_fd(task, fd);
    if (!file || !(file->flags & TASK_FILE_FLAG_TIMERFD) || !file->aux) return NULL;
    return (struct task_timerfd *)(uintptr_t)file->aux;
}

void task_timerfd_update(struct task_timerfd *timer)
{
    uint64_t now = time_ticks();
    if (!timer || !timer->expiry_tick || timer->expiry_tick > now) return;
    if (!timer->interval_ticks) {
        timer->expirations = 1;
        timer->expiry_tick = 0;
        return;
    }
    uint64_t periods = (now - timer->expiry_tick) / timer->interval_ticks + 1;
    if (UINT64_MAX - timer->expirations < periods) timer->expirations = UINT64_MAX;
    else timer->expirations += periods;
    timer->expiry_tick += periods * timer->interval_ticks;
}

int64_t syscall_timerfd_create(uint64_t clockid, uint32_t flags)
{
    struct task *task = sched_current_task();
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_DEVICE,
                                .flags = STORAGE_NODE_FLAG_DEV_NODE,
                                .first_cluster = 0};
    struct task_timerfd *timer;
    struct task_file *file;
    int fd;
    if (!task || (clockid != LINUX_CLOCK_REALTIME && clockid != LINUX_CLOCK_MONOTONIC) ||
        (flags & ~(TFD_CLOEXEC | TFD_NONBLOCK))) return -RELIEFOS_EINVAL;
    timer = kernel_malloc(sizeof(*timer));
    if (!timer) return -RELIEFOS_ENOMEM;
    *timer = (struct task_timerfd){.clockid = (int32_t)clockid, .flags = flags};
    fd = alloc_task_fd(task, &node, TASK_FILE_FLAG_TIMERFD | RELIEFOS_O_RDWR |
                       (flags & (TFD_CLOEXEC | TFD_NONBLOCK)), NULL);
    if (fd < 0) { kernel_free(timer); return fd; }
    file = task_file_for_fd(task, fd);
    if (!file) { kernel_free(timer); return -RELIEFOS_EMFILE; }
    file->aux = (uint64_t)(uintptr_t)timer;
    return fd;
}

int64_t syscall_timerfd_settime(uint64_t fd_arg, uint32_t flags,
                                       uint64_t value_ptr, uint64_t old_ptr)
{
    struct task *task = sched_current_task();
    struct task_timerfd *timer = task_timerfd_for_fd(task, (int)fd_arg);
    struct linux_itimerspec value, old = {0};
    uint64_t initial, interval;
    if (!timer) return -RELIEFOS_EBADF;
    if (flags & ~(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET)) return -RELIEFOS_EINVAL;
    if (!user_range_ok(value_ptr, sizeof(value))) return -RELIEFOS_EFAULT;
    value = *(const struct linux_itimerspec *)(uintptr_t)value_ptr;
    if (value.it_value.tv_sec < 0 || value.it_value.tv_nsec < 0 || value.it_value.tv_nsec >= 1000000000LL ||
        value.it_interval.tv_sec < 0 || value.it_interval.tv_nsec < 0 || value.it_interval.tv_nsec >= 1000000000LL)
        return -RELIEFOS_EINVAL;
    task_timerfd_update(timer);
    if (timer->expiry_tick) {
        uint64_t remaining = timer->expiry_tick > time_ticks() ? timer->expiry_tick - time_ticks() : 0;
        old.it_value.tv_sec = (int64_t)(remaining / RELIEFNT_TICK_HZ);
        old.it_value.tv_nsec = (int64_t)((remaining % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
    }
    old.it_interval.tv_sec = (int64_t)(timer->interval_ticks / RELIEFNT_TICK_HZ);
    old.it_interval.tv_nsec = (int64_t)((timer->interval_ticks % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
    if (flags & TFD_TIMER_ABSTIME) {
        struct linux_timespec now = {0};
        if (time_clock_get(timer->clockid, &now) < 0) return -RELIEFOS_EINVAL;
        if (value.it_value.tv_sec < now.tv_sec ||
            (value.it_value.tv_sec == now.tv_sec && value.it_value.tv_nsec <= now.tv_nsec)) {
            initial = 1;
        } else {
            uint64_t sec = (uint64_t)(value.it_value.tv_sec - now.tv_sec);
            int64_t nsec = value.it_value.tv_nsec - now.tv_nsec;
            if (nsec < 0) { --sec; nsec += 1000000000LL; }
            initial = sec * RELIEFNT_TICK_HZ + ((uint64_t)nsec + 9999999ULL) / 10000000ULL;
        }
    } else {
        initial = (uint64_t)value.it_value.tv_sec * RELIEFNT_TICK_HZ +
                  ((uint64_t)value.it_value.tv_nsec + 9999999ULL) / 10000000ULL;
    }
    interval = (uint64_t)value.it_interval.tv_sec * RELIEFNT_TICK_HZ +
               ((uint64_t)value.it_interval.tv_nsec + 9999999ULL) / 10000000ULL;
    timer->interval_ticks = interval;
    timer->expirations = 0;
    timer->expiry_tick = initial ? time_ticks() + initial : 0;
    if (old_ptr) {
        if (!user_range_writable(old_ptr, sizeof(old))) return -RELIEFOS_EFAULT;
        *(struct linux_itimerspec *)(uintptr_t)old_ptr = old;
    }
    return 0;
}

int64_t syscall_timerfd_gettime(uint64_t fd_arg, uint64_t value_ptr)
{
    struct task *task = sched_current_task();
    struct task_timerfd *timer = task_timerfd_for_fd(task, (int)fd_arg);
    struct linux_itimerspec value = {0};
    if (!timer) return -RELIEFOS_EBADF;
    if (!user_range_writable(value_ptr, sizeof(value))) return -RELIEFOS_EFAULT;
    task_timerfd_update(timer);
    if (timer->expiry_tick) {
        uint64_t remaining = timer->expiry_tick > time_ticks() ? timer->expiry_tick - time_ticks() : 0;
        value.it_value.tv_sec = (int64_t)(remaining / RELIEFNT_TICK_HZ);
        value.it_value.tv_nsec = (int64_t)((remaining % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
    }
    value.it_interval.tv_sec = (int64_t)(timer->interval_ticks / RELIEFNT_TICK_HZ);
    value.it_interval.tv_nsec = (int64_t)((timer->interval_ticks % RELIEFNT_TICK_HZ) * (1000000000ULL / RELIEFNT_TICK_HZ));
    *(struct linux_itimerspec *)(uintptr_t)value_ptr = value;
    return 0;
}

int64_t syscall_memfd_create(uint64_t name_ptr, uint32_t flags)
{
    struct task *task = sched_current_task();
    struct storage_node node = {
        .type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = STORAGE_DEV_KIND_SHM,
    };
    struct task_file *file;
    char name[256];
    int fd;
    int ret;
    if (!task || !name_ptr ||
        (flags & ~(LINUX_MFD_CLOEXEC | LINUX_MFD_ALLOW_SEALING)))
        return -RELIEFOS_EINVAL;
    ret = copy_user_string_fixed(name, sizeof(name), name_ptr, NULL);
    if (ret < 0) return ret;
    if (!name[0]) return -RELIEFOS_EINVAL;
    fd = alloc_task_fd(task, &node, RELIEFOS_O_RDWR, NULL);
    if (fd < 0) return fd;
    file = task_file_for_fd(task, fd);
    if (!file || task_shm_attach(file, flags) < 0) {
        if (file) clear_task_file(file);
        return -RELIEFOS_ENOMEM;
    }
    if (task_shm_truncate(file, 0) < 0) {
        clear_task_file(file);
        return -RELIEFOS_ENOMEM;
    }
    if (flags & LINUX_MFD_CLOEXEC) file->fd_flags = RELIEFOS_FD_CLOEXEC;
    return fd;
}
