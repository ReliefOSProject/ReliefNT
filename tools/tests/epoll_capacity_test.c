/* Host fixture for the production epoll implementation's dynamic watch set. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../kernel/syscall/event/epoll.c"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "../../drivers/tty/input/input.c"
#pragma GCC diagnostic pop

#define TEST_FD_MAX 512u
#define TEST_WATCHES 256u

static struct task current;
static struct task_file epoll_file;
static struct task_file targets[TEST_FD_MAX];
static struct task_file *descriptors[TEST_FD_MAX];
static int allocation_budget = -1;
static uint32_t kernel_free_calls;

uint32_t sched_task_file_capacity(const struct task *task)
{
    (void)task;
    return TEST_FD_MAX - 3u;
}

struct task_file *sched_task_file_at(struct task *task, uint32_t index)
{
    (void)task;
    return index < TEST_FD_MAX - 3u ? descriptors[index + 3u] : NULL;
}

struct task *sched_current_task(void) { return &current; }

struct task_file *task_file_for_fd(struct task *task, int fd)
{
    (void)task;
    return fd >= 0 && (uint32_t)fd < TEST_FD_MAX ? descriptors[fd] : NULL;
}

struct task_file *task_file_get(struct task_file *source)
{
    if (!source || !source->used) return NULL;
    ++source->references;
    return source;
}

void task_file_put(struct task_file *description)
{
    assert(description && description->references);
    --description->references;
}

struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd)
{
    (void)task;
    (void)fd;
    return NULL;
}

int task_pty_stream_for_fd(struct task *task, int fd)
{
    (void)task;
    (void)fd;
    return -1;
}

int task_pty_ensure_fd(struct task *task, int fd, struct task_pty_fd **out)
{
    (void)task;
    (void)fd;
    (void)out;
    return -RELIEFOS_EBADF;
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

void kernel_free(void *pointer)
{
    if (pointer) ++kernel_free_calls;
    free(pointer);
}

uint64_t kernel_poll_readiness_generation(void) { return 0; }
void kernel_poll_readiness_notify(void) { }
void kernel_poll_wait_remove(struct task *task) { (void)task; }
uint64_t task_socket_event_generation(const struct task_file *file)
{ (void)file; return 0; }
uint64_t time_ticks(void) { return 0; }
uint64_t time_uptime_us(void) { static uint64_t now; return ++now; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }
int64_t syscall_poll_park(struct task *task, int64_t timeout_ms,
                          uint64_t generation, bool can_wait_for_readiness)
{
    (void)task;
    (void)timeout_ms;
    (void)generation;
    (void)can_wait_for_readiness;
    return 0;
}

int64_t syscall_poll_impl(struct task *task, struct pollfd *fds,
                          uint32_t nfds, int64_t timeout_ms,
                          uint64_t generation)
{
    (void)task;
    (void)timeout_ms;
    (void)generation;
    for (uint32_t i = 0; i < nfds; ++i) {
        struct task_file *file = task_file_for_fd(task, fds[i].fd);
        bool ready = !(file->flags & TASK_FILE_FLAG_DEV_NODE) ||
            input_evdev_available_vt(file->node.first_cluster, file->aux,
                                     file->aux2, file->input_vt);
        fds[i].revents = ready ? fds[i].events & POLLIN : 0;
    }
    return nfds;
}

/** @brief Exercise real evdev refill edges without an intermediate epoll scan. */
static void test_evdev_edges(void)
{
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    struct task_file *mouse = &targets[4];
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data = 91}, output;
    struct input_event records[32];
    assert(epoll);
    current.pid = 1;
    current.kind = TASK_KIND_USER;
    current.limits.nofile.rlim_cur = TEST_FD_MAX;
    epoll_file = (struct task_file){.used = 1, .flags = TASK_FILE_FLAG_EPOLL,
        .aux = (uint64_t)(uintptr_t)epoll};
    *mouse = (struct task_file){.used = 1, .flags = TASK_FILE_FLAG_DEV_NODE,
        .node.first_cluster = STORAGE_DEV_KIND_MOUSE, .input_vt = 1};
    descriptors[3] = &epoll_file; descriptors[4] = mouse;
    input_init(); input_set_graphical_vt(1);
    mouse->aux = input_evdev_cursor_now();
    assert(syscall_epoll_ctl(3, EPOLL_CTL_ADD, 4, (uintptr_t)&event) == 0);
    input_push_mouse(10, 20, 1, 0, 1);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    assert(output.events == EPOLLIN && output.data == 91);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    assert(input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &mouse->aux, records,
                               sizeof(records), 0, 1) > 0);
    /* Xorg can drain and receive another packet before epoll runs again. */
    input_push_mouse(30, 40, 20, 20, 0);
    assert(task_epoll_poll_ready(&current, &epoll_file));
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 0);
    input_push_key(30, 1);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    int64_t grab = input_evdev_grab(STORAGE_DEV_KIND_MOUSE, 0, 1, 77);
    assert(grab > 0);
    input_push_mouse(40, 50, 10, 10, 0);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    input_evdev_release(STORAGE_DEV_KIND_MOUSE, (uint64_t)grab);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    input_set_graphical_vt(2); input_push_mouse(50, 60, 20, 20, 0);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    input_set_graphical_vt(1);
    event.events |= EPOLLONESHOT;
    assert(syscall_epoll_ctl(3, EPOLL_CTL_MOD, 4, (uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    input_push_mouse(70, 80, 20, 20, 1);
    assert(!task_epoll_poll_ready(&current, &epoll_file));
    assert(syscall_epoll_ctl(3, EPOLL_CTL_MOD, 4, (uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    event.events &= ~EPOLLONESHOT;
    assert(syscall_epoll_ctl(3, EPOLL_CTL_MOD, 4, (uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    /* Loss/resynchronization must also wake an ET reader with a stale bit. */
    for (unsigned i = 0; i < 600; ++i) input_push_key(30, 1);
    assert(task_epoll_poll_ready(&current, &epoll_file));
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    assert(input_evdev_read_vt(STORAGE_DEV_KIND_MOUSE, &mouse->aux, records,
                               sizeof(records), 0, 1) == 2 * (int)sizeof(*records));
    assert(records[0].type == EV_SYN && records[0].code == SYN_DROPPED);
    input_push_mouse(90, 100, 20, 20, 0);
    assert(syscall_epoll_wait(3, (uintptr_t)&output, 1, 0) == 1);
    task_epoll_destroy(epoll);
    *mouse = (struct task_file){0}; epoll_file = (struct task_file){0};
    descriptors[3] = descriptors[4] = NULL; kernel_free_calls = 0;
    puts("PASS evdev epoll: refill/loss edges, no duplicate, device/VT/grab isolation and oneshot rearm");
}

int main(void)
{
    struct task_epoll *epoll;
    struct epoll_event event;
    struct epoll_event ready[TEST_WATCHES];

    test_evdev_edges();
    memset(&current, 0, sizeof(current));
    current.pid = 1;
    current.kind = TASK_KIND_USER;
    current.limits.nofile.rlim_cur = TEST_FD_MAX;
    for (uint32_t i = 4; i < TEST_FD_MAX; ++i) {
        targets[i].used = 1;
        targets[i].node.type = RELIEFOS_FS_TYPE_FILE;
        descriptors[i] = &targets[i];
    }
    epoll = malloc(sizeof(*epoll));
    assert(epoll != NULL);
    memset(epoll, 0, sizeof(*epoll));
    epoll_file.used = 1;
    epoll_file.flags = TASK_FILE_FLAG_EPOLL;
    epoll_file.aux = (uint64_t)(uintptr_t)epoll;
    descriptors[3] = &epoll_file;

    for (uint32_t i = 0; i < TEST_WATCHES; ++i) {
        int fd = (int)(i + 4u);
        event = (struct epoll_event){.events = EPOLLIN, .data = i + 1000u};
        int64_t result = syscall_epoll_ctl(3, EPOLL_CTL_ADD, (uint64_t)fd,
                                           (uint64_t)(uintptr_t)&event);
        if (result != 0) {
            fprintf(stderr, "FAIL: epoll rejected watch %u with result %lld\n",
                    i, (long long)result);
            return 1;
        }
    }

    if (syscall_epoll_wait(3, (uint64_t)(uintptr_t)ready, TEST_WATCHES, 0) !=
        TEST_WATCHES) {
        fputs("FAIL: epoll did not return all 256 ready watches\n", stderr);
        return 1;
    }
    for (uint32_t i = 0; i < TEST_WATCHES; ++i) {
        if (ready[i].events != EPOLLIN || ready[i].data != i + 1000u) {
            fprintf(stderr, "FAIL: epoll watch %u lost its event data\n", i);
            return 1;
        }
    }

    task_epoll_remove_file_description(&targets[200]);
    targets[200] = (struct task_file){
        .used = 1, .node = {.type = RELIEFOS_FS_TYPE_FILE},
    };
    descriptors[200] = &targets[200];
    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0xbeef)};
    if (epoll->entries != TEST_WATCHES - 1u ||
        syscall_epoll_ctl(3, EPOLL_CTL_ADD, 200,
                          (uint64_t)(uintptr_t)&event) != 0) {
        fputs("FAIL: closing and reusing an fd did not replace its epoll identity\n", stderr);
        return 1;
    }
    if (syscall_epoll_wait(3, (uint64_t)(uintptr_t)ready, TEST_WATCHES, 0) !=
        TEST_WATCHES) {
        fputs("FAIL: reused descriptor changed the epoll watch count\n", stderr);
        return 1;
    }
    int saw_reused_fd = 0;
    for (uint32_t i = 0; i < TEST_WATCHES; ++i)
        if (ready[i].data == UINT64_C(0xbeef)) saw_reused_fd = 1;
    if (!saw_reused_fd) {
        fputs("FAIL: replacement fd did not return its new epoll payload\n", stderr);
        return 1;
    }

    allocation_budget = 0;
    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0xfeed)};
    int64_t failed_add = syscall_epoll_ctl(3, EPOLL_CTL_ADD, TEST_WATCHES + 4u,
                                           (uint64_t)(uintptr_t)&event);
    allocation_budget = -1;
    if (failed_add != -RELIEFOS_ENOMEM || epoll->entries != TEST_WATCHES) {
        fprintf(stderr,
                "FAIL: failed epoll growth result=%lld entries=%u (expected ENOMEM and 256)\n",
                (long long)failed_add, epoll->entries);
        return 1;
    }
    if (syscall_epoll_wait(3, (uint64_t)(uintptr_t)ready, TEST_WATCHES, 0) !=
        TEST_WATCHES) {
        fputs("FAIL: failed epoll growth corrupted existing watches\n", stderr);
        return 1;
    }

    if (kernel_free_calls != 5u) {
        fprintf(stderr, "FAIL: expected four old vectors and one retired watch freed, saw %u\n",
                kernel_free_calls);
        return 1;
    }
    task_epoll_destroy(epoll);
    if (kernel_free_calls != TEST_WATCHES + 7u) {
        fprintf(stderr, "FAIL: epoll destruction did not free watches, state, and vector (saw %u frees)\n",
                kernel_free_calls);
        return 1;
    }

    puts("PASS epoll dynamic watch capacity: 256 watches, fd reuse, payloads, allocation rollback, and cleanup");
    return 0;
}
