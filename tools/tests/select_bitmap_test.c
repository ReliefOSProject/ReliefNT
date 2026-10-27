/* Host fixture for the production raw select implementation and fd bitmaps. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/syscall/event/poll.c"

#define TEST_FD_MAX 4096u
#define HIGH_READY_FD 1100u
#define HIGH_WORDS ((HIGH_READY_FD + 1u + 63u) / 64u)

static struct task current;
static struct task_file files[TEST_FD_MAX];
static int allocation_budget = -1;

struct task *sched_current_task(void) { return &current; }

struct task_file *task_file_for_fd(struct task *task, int fd)
{
    (void)task;
    if (fd < 0 || (uint32_t)fd >= TEST_FD_MAX || !files[fd].used) return NULL;
    return &files[fd];
}

struct task_file *task_file_for_io(struct task *task, int fd)
{
    return task_file_for_fd(task, fd);
}

/* This fixture covers select bitmaps and contains no epoll descriptors. */
bool task_epoll_poll_ready(struct task *task, struct task_file *file)
{
    (void)task;
    (void)file;
    abort();
}

struct task_pty_fd *task_pty_fd_for_fd(struct task *task, int fd)
{
    (void)task;
    (void)fd;
    return NULL;
}

struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd)
{
    (void)task;
    (void)fd;
    return NULL;
}

struct task_pty_fd *task_pty_for_io(struct task *task, int fd)
{
    return task_pty_endpoint_for_fd(task, fd);
}

int task_pty_stream_for_fd(struct task *task, int fd)
{
    (void)task;
    (void)fd;
    return -1;
}

bool user_range_ok(uint64_t address, uint64_t length)
{
    return address != 0 && length != 0;
}

bool user_range_writable(uint64_t address, uint64_t length)
{
    return user_range_ok(address, length);
}

void *kernel_malloc(size_t size)
{
    if (allocation_budget == 0) return NULL;
    if (allocation_budget > 0) --allocation_budget;
    return malloc(size);
}

void kernel_free(void *pointer) { free(pointer); }

uint64_t time_ticks(void) { return 100; }
uint64_t kernel_poll_readiness_generation(void) { return 0; }
void kernel_poll_wait_remove(struct task *task) { (void)task; }
int kernel_poll_wait_current(uint64_t generation, uint64_t wake_tick)
{ (void)generation; (void)wake_tick; return 1; }

short task_signalfd_poll(struct task *task, const struct task_file *file)
{ (void)task; (void)file; return 0; }
short task_pipe_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short task_socket_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
short task_inet_poll(const struct task_file *file, short events)
{ (void)file; (void)events; return 0; }
void task_timerfd_update(struct task_timerfd *timer) { (void)timer; }
/* Select fixture has no character providers; generic routing is tested
 * elsewhere, so reaching a provider here must fail instead of faking readiness. */
short character_file_poll(struct character_file *handle,struct task_file *file,short events)
{(void)handle;(void)file;(void)events;assert(!"unexpected character provider");return 0;}
void task_evdev_bind_vt(struct task *task, struct task_file *file)
{ (void)task; (void)file; }
int input_evdev_available_vt(uint32_t device_kind, uint64_t cursor,
                             uint64_t grab_token, uint32_t vt)
{ (void)device_kind; (void)cursor; (void)grab_token; (void)vt; return 0; }
int task_device_is(const struct task_file *file, uint32_t kind)
{ (void)file; (void)kind; return 0; }
int file_can_read(const struct task_file *file) { (void)file; return 1; }
int file_can_write(const struct task_file *file) { (void)file; return 1; }
int pty_is_hungup(uint32_t pty_id) { (void)pty_id; return 0; }
uint32_t pty_input_available(uint32_t pty_id) { (void)pty_id; return 0; }
uint32_t pty_output_available(uint32_t pty_id) { (void)pty_id; return 0; }

static void set_readable_file(uint32_t fd)
{
    files[fd] = (struct task_file){
        .used = 1,
        .node = {.type = RELIEFOS_FS_TYPE_FILE},
    };
}

static int test_high_descriptor_bitmap(void)
{
    uint64_t read_set[HIGH_WORDS] = {0};
    read_set[HIGH_READY_FD >> 6] |= UINT64_C(1) << (HIGH_READY_FD & 63u);
    set_readable_file(HIGH_READY_FD);

    int64_t result = syscall_select_common(HIGH_READY_FD + 1u,
        (uint64_t)(uintptr_t)read_set, 0, 0, 0, false);
    if (result != 1 ||
        read_set[HIGH_READY_FD >> 6] !=
            (UINT64_C(1) << (HIGH_READY_FD & 63u))) {
        fprintf(stderr, "FAIL: raw select rejected ready fd %u (result=%lld)\n",
                HIGH_READY_FD, (long long)result);
        return 1;
    }
    return 0;
}

static int test_error_preserves_masks(void)
{
    uint64_t read_set = (UINT64_C(1) << 5) | (UINT64_C(1) << 9);
    const uint64_t original = read_set;
    set_readable_file(5);

    int64_t result = syscall_select_common(10, (uint64_t)(uintptr_t)&read_set,
                                           0, 0, 0, false);
    if (result != -RELIEFOS_EBADF || read_set != original) {
        fprintf(stderr,
                "FAIL: select error changed fd mask (result=%lld mask=0x%llx)\n",
                (long long)result, (unsigned long long)read_set);
        return 1;
    }
    return 0;
}

static int test_allocation_failure_preserves_masks(void)
{
    uint64_t read_set = UINT64_C(1) << 5;
    const uint64_t original = read_set;
    set_readable_file(5);
    allocation_budget = 1;

    int64_t result = syscall_select_common(6, (uint64_t)(uintptr_t)&read_set,
                                           0, 0, 0, false);
    allocation_budget = -1;
    if (result != -RELIEFOS_ENOMEM || read_set != original) {
        fprintf(stderr,
                "FAIL: select allocation error changed fd mask (result=%lld mask=0x%llx)\n",
                (long long)result, (unsigned long long)read_set);
        return 1;
    }
    return 0;
}

static int test_nofile_bound_and_signed_overflow(void)
{
    current.limits.nofile.rlim_cur = HIGH_READY_FD;
    int64_t over_limit = syscall_select_common(HIGH_READY_FD + 1u, 0, 0, 0, 0, false);
    int64_t signed_overflow = syscall_select_common(UINT64_MAX, 0, 0, 0, 0, false);
    current.limits.nofile.rlim_cur = TEST_FD_MAX;
    if (over_limit != -RELIEFOS_EINVAL || signed_overflow != -RELIEFOS_EINVAL) {
        fprintf(stderr,
                "FAIL: select nfds limits returned %lld and %lld\n",
                (long long)over_limit, (long long)signed_overflow);
        return 1;
    }
    return 0;
}

int main(void)
{
    memset(&current, 0, sizeof(current));
    current.pid = 1;
    current.kind = TASK_KIND_USER;
    current.limits.nofile.rlim_cur = TEST_FD_MAX;

    if (test_high_descriptor_bitmap()) return 1;
    if (test_error_preserves_masks()) return 1;
    if (test_allocation_failure_preserves_masks()) return 1;
    if (test_nofile_bound_and_signed_overflow()) return 1;
    puts("PASS raw select: fd 1100, RLIMIT bound, error and allocation mask preservation");
    return 0;
}
