/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/sched.h>
#include <reliefnt/fsnotify.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/swap.h>
#include <reliefnt/futex.h>
#include <linux/errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Deliberately fake only the queue and user-memory boundary; read handling is
 * linked from the production adapter with dead unrelated sections discarded. */
struct record {
    int32_t wd;
    uint32_t mask, cookie, len;
    char name[16];
};
struct fsnotify_instance {
    struct record events[2];
    unsigned first, count;
};

static unsigned prepare_calls;
static unsigned fail_call;
static int prepare_error;
static unsigned blocked_count;
static int writable = 1;

size_t fsnotify_peek(struct fsnotify_instance *instance, const struct inotify_event **out)
{
    if (!instance->count) { *out = NULL; return 0; }
    *out = (const struct inotify_event *)&instance->events[instance->first];
    return sizeof(struct record);
}
void fsnotify_pop(struct fsnotify_instance *instance)
{ ++instance->first; --instance->count; }
bool fsnotify_ready(const struct fsnotify_instance *instance)
{ return instance->count != 0; }
size_t fsnotify_pending_bytes(const struct fsnotify_instance *instance)
{ return instance->count * sizeof(struct record); }
uint64_t kernel_poll_readiness_generation(void) { return 1; }
void kernel_poll_wait_remove(struct task *task) { (void)task; }
int kernel_poll_wait_current(uint64_t generation, uint64_t tick)
{ (void)generation; (void)tick; ++blocked_count; return 1; }
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length, bool output)
{
    (void)task; (void)address; (void)length; (void)output;
    ++prepare_calls;
    return fail_call == prepare_calls ? prepare_error : 0;
}
bool user_range_writable(uint64_t address, uint64_t length)
{ (void)length; return writable && address != 0; }

extern int64_t task_inotify_read(struct task *, struct task_file *, uint64_t, uint64_t);
extern short task_inotify_poll(const struct task_file *);
extern int64_t task_inotify_fionread(struct task *, const struct task_file *, uint64_t);

static void reset_queue(struct fsnotify_instance *instance)
{
    memset(instance, 0, sizeof(*instance));
    instance->events[0] = (struct record){.wd = 7, .mask = IN_CREATE, .len = 16, .name = "one"};
    instance->events[1] = (struct record){.wd = 7, .mask = IN_CREATE, .len = 16, .name = "two"};
    instance->count = 2;
    prepare_calls = fail_call = 0;
    prepare_error = 0;
    writable = 1;
}

int main(void)
{
    struct task task = {0};
    struct fsnotify_instance instance;
    struct task_file file = {.used = 1, .kind = 5, .flags = LINUX_O_NONBLOCK,
                             .aux = (uint64_t)(uintptr_t)&instance};
    unsigned char buffer[64] = {0};
    int available = -1;
    reset_queue(&instance);
    assert(task_inotify_poll(&file) & POLLIN);
    assert(task_inotify_fionread(&task, &file, (uintptr_t)&available) == 0 && available == 64);
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, 16) == -EINVAL);
    assert(instance.first == 0 && instance.count == 2);
    assert(task_inotify_read(&task, &file, UINT64_C(1) << 63, 32) == -EFAULT);
    assert(instance.first == 0 && instance.count == 2);
    writable = 0;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == -EFAULT);
    assert(instance.first == 1 && instance.count == 1);
    reset_queue(&instance);
    prepare_calls = 0; fail_call = 1; prepare_error = SWAP_STORAGE_CONTENDED;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(instance.first == 0 && instance.count == 2);
    prepare_calls = 0; fail_call = 2;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == 32);
    assert(instance.first == 1 && instance.count == 1 && !strcmp((char *)buffer + 16, "one"));
    fail_call = 0;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == 32);
    assert(instance.count == 0 && !strcmp((char *)buffer + 16, "two"));
    reset_queue(&instance);
    fail_call = 2; prepare_error = -EFAULT;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == -EFAULT);
    assert(instance.first == 2 && instance.count == 0 && !strcmp((char *)buffer + 16, "one"));
    assert(task_inotify_poll(&file) == 0);
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == -EAGAIN);
    file.flags = 0;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == KERNEL_SYSCALL_BLOCKED);
    assert(blocked_count == 1);
    task.pending_signals = 1;
    assert(task_inotify_read(&task, &file, (uintptr_t)buffer, sizeof(buffer)) == -EINTR);
    puts("[inotify-adapter] PASS");
    return 0;
}
