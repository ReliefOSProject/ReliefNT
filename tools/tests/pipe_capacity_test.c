#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include "../../kernel/syscall/ipc/ipc.c"

static struct task current;
static struct task_file descriptors[640];
extern int64_t task_pipe_capacity(struct task_file *file, bool setting, uint32_t bytes);
void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *memory) { free(memory); }
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state = 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags) { (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags) { (void)lock; (void)flags; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue) { *queue = (struct kernel_wait_queue){0}; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue) { (void)queue; return 0; }
void kernel_poll_readiness_notify(void) { }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task) { (void)queue; (void)task; }
uint32_t kernel_wait_queue_wake_satisfied(struct kernel_wait_queue *queue, uint32_t available)
{ (void)queue; (void)available; return 0; }
void kernel_wait_queue_block_current(struct kernel_wait_queue *queue) { (void)queue; abort(); }
void kernel_wait_queue_block_current_minimum(struct kernel_wait_queue *queue, uint32_t minimum)
{ (void)queue; (void)minimum; abort(); }
uint32_t sched_current_pid(void) { return 17; }
int sched_signal_user_task(uint32_t pid, int signal) { (void)pid; (void)signal; return 0; }
struct task *sched_current_task(void) { return &current; }
bool user_range_writable(uint64_t address, uint64_t bytes) { return address && bytes; }
int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    (void)task;
    for (unsigned i = minimum; i < 640; ++i) {
        if (descriptors[i].used) continue;
        descriptors[i] = (struct task_file){.used = 1};
        *out = &descriptors[i];
        return (int)i;
    }
    return -RELIEFOS_EMFILE;
}
void task_discard_file_fd(struct task *task, int fd)
{
    (void)task;
    task_pipe_release(&descriptors[fd]);
    descriptors[fd] = (struct task_file){0};
}

int main(void)
{
    kernel_objects_init();
    current.uid = 1000;
    for (unsigned i = 0; i < 300; ++i) {
        int pair[2] = {-1, -1};
        int ret = syscall_ipc_pipe((uintptr_t)pair);
        if (ret) fprintf(stderr, "pipe %u: %d\n", i, ret);
        assert(!ret && pair[0] == (int)i * 2 && pair[1] == (int)i * 2 + 1);
    }
    struct task_file *read_end = &descriptors[0], *write_end = &descriptors[1];
    assert(task_pipe_capacity(read_end, false, 0) == 65536);
    assert(task_pipe_capacity(write_end, true, 5000) == 8192);
    unsigned char bytes[7000], result[7000];
    memset(bytes, 0x5a, sizeof(bytes));
    assert(task_pipe_write(write_end, bytes, sizeof(bytes)) == sizeof(bytes));
    assert(task_pipe_capacity(read_end, true, 4096) == -RELIEFOS_EBUSY);
    assert(task_pipe_capacity(read_end, true, 20000) == 32768);
    assert(task_pipe_read(read_end, result, sizeof(result)) == sizeof(result));
    assert(!memcmp(bytes, result, sizeof(bytes)));
    assert(task_pipe_capacity(write_end, true, 0) == 4096);
    assert(task_pipe_capacity(write_end, true, 1048577) == -RELIEFOS_EPERM);
    assert(task_pipe_capacity(write_end, true, UINT32_MAX) == -RELIEFOS_EINVAL);
    current.cap_effective = 1ULL << CAP_SYS_RESOURCE;
    assert(task_pipe_capacity(write_end, true, 1048577) == 2097152);
    assert(task_pipe_capacity(write_end, true, 4096) == 4096);
    current.cap_effective = 0;
    write_end->flags |= RELIEFOS_O_NONBLOCK;
    assert(task_pipe_write(write_end, bytes, 3000) == 3000);
    assert(task_pipe_write(write_end, bytes, 2000) == -RELIEFOS_EAGAIN);
    assert(task_pipe_read(read_end, result, sizeof(result)) == 3000);
    for (unsigned i = 0; i < 600; ++i) task_discard_file_fd(&current, (int)i);
    puts("PASS pipe resources: 300 live pipes and final-close reclamation");
}
