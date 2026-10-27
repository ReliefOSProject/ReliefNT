/* Production read/write bodies plus production pipe/wait code. Hardware and
 * unrelated storage backends are unavailable on the host and fail if reached.
 * The fixture checks descriptor selection, validation and pipe/eventfd/null
 * results; full dispatcher routing is covered separately in the guest. */
#include <assert.h>
#define main pipe_wake_regressions
#include "pipe_wake_test.c"
#undef main
#include <reliefos/auth_abi.h>
#include <reliefos/text.h>

static _Noreturn int io_unexpected(const char *name, ...)
{
    fprintf(stderr, "FAIL: unexpected backend %s\n", name);
    abort();
}
#define IO_UNAVAILABLE(...) io_unexpected(__func__, ##__VA_ARGS__)
#define task_signalfd_read IO_UNAVAILABLE
#define character_file_read(...) io_unexpected(__func__, ##__VA_ARGS__)
#define character_file_write(...) io_unexpected(__func__, ##__VA_ARGS__)
#define pty_write_input IO_UNAVAILABLE
#define pty_check_change IO_UNAVAILABLE
#define pty_write_output IO_UNAVAILABLE
#define pty_output_available IO_UNAVAILABLE
#define pty_is_hungup IO_UNAVAILABLE
#define pty_read_output IO_UNAVAILABLE
#define pty_input_available IO_UNAVAILABLE
#define pty_read_input IO_UNAVAILABLE
#define task_socket_write IO_UNAVAILABLE
#define task_socket_read IO_UNAVAILABLE
#define task_inet_write IO_UNAVAILABLE
#define task_inet_read IO_UNAVAILABLE
#define console_write_len IO_UNAVAILABLE
#define task_shm_write IO_UNAVAILABLE
#define task_shm_read IO_UNAVAILABLE
#define task_effective_role IO_UNAVAILABLE
#define storage_installer_root_active IO_UNAVAILABLE
static struct task_file *io_expected_regular;
static bool io_expected_writing;
static uint8_t user_bytes[131072];
/* This fixture observes dispatch only; actual provider reads and per-open
 * capture bytes are exercised by regular_file_io_test.c and native probes. */
static int64_t io_regular_backend(struct task *task,struct task_file *file,uint64_t buffer,
                                     uint64_t count,uint64_t position,bool writing,bool positional)
{
    assert(task==current_task && file==io_expected_regular && writing==io_expected_writing && !positional && !position);
    assert(buffer==(uintptr_t)user_bytes);return (int64_t)count;
}
#define syscall_regular_io io_regular_backend
#define task_device_write_length IO_UNAVAILABLE
#define task_device_write IO_UNAVAILABLE
#define task_device_read IO_UNAVAILABLE
#define task_timerfd_update IO_UNAVAILABLE
#define proc_readdir IO_UNAVAILABLE
#define proc_read IO_UNAVAILABLE
#define storage_readdir_node IO_UNAVAILABLE

/* Descriptor tables are empty: only the actual entry-pinned description is
 * available. Accidentally falling back to a stale fd must fail with EBADF. */
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }
static struct task_pty_fd *task_pty_endpoint_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }
static int task_pty_stream_for_fd(struct task *task, int fd)
{ (void)task; (void)fd; return -1; }

static uint64_t user_range_bytes = sizeof(user_bytes);
static bool user_writable = true;
bool user_range_ok(uint64_t address, uint64_t length)
{
    uintptr_t base = (uintptr_t)user_bytes;
    return address >= base && address <= base + user_range_bytes &&
           length <= base + user_range_bytes - address;
}
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_writable && user_range_ok(address, length); }

#include "syscall_io_body.inc"

static void pin(struct task *task, int fd, struct task_file *file)
{
    current_task = task;
    task->syscall_fd = fd;
    task->syscall_file = file;
}

static void test_redirected_stdio_and_validation(void)
{
    reset_pipe();
    for (unsigned i = 0; i < 512; ++i) user_bytes[i] = (uint8_t)i;
    pin(&test_tasks[0], 1, &write_end);
    assert(syscall_io_write(1, (uintptr_t)user_bytes, 512) == 512);
    assert(syscall_io_write(1, 0, 1) == -RELIEFOS_EFAULT);
    pin(&test_tasks[READER_INDEX], 0, &read_end);
    user_writable = false;
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 512) == -RELIEFOS_EFAULT);
    user_writable = true;
    memset(user_bytes, 0, 512);
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 512) == 512);
    for (unsigned i = 0; i < 512; ++i) assert(user_bytes[i] == (uint8_t)i);
    assert(syscall_io_read(0, (uintptr_t)user_bytes, 0) == 0);
    assert(syscall_io_read(7, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
}

static void test_path_descriptor_rejects_io(void)
{
    struct task_file file={.used=1,.flags=TASK_FILE_FLAG_PATH|TASK_FILE_FLAG_DEV_NULL};
    pin(&test_tasks[0],9,&file);
    assert(syscall_io_read(9,(uintptr_t)user_bytes,1)==-RELIEFOS_EBADF);
    assert(syscall_io_write(9,(uintptr_t)user_bytes,1)==-RELIEFOS_EBADF);
    assert(syscall_io_read(9,(uintptr_t)user_bytes,0)==-RELIEFOS_EBADF);
}

static void test_pipe_wait_retry_eof_and_signal(void)
{
    reset_pipe();
    pin(&test_tasks[READER_INDEX], 3, &read_end);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 512) == -RELIEFOS_EAGAIN);
    assert(current_task->state == TASK_BLOCKED);
    pin(&test_tasks[0], 4, &write_end);
    memset(user_bytes, 0xa5, 512);
    assert(syscall_io_write(4, (uintptr_t)user_bytes, 512) == 512);
    assert(test_tasks[READER_INDEX].state == TASK_READY);
    pin(&test_tasks[READER_INDEX], 3, &read_end);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 512) == 512);
    read_end.flags |= RELIEFOS_O_NONBLOCK;
    current_task->state = TASK_RUNNING;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EAGAIN);
    assert(current_task->state == TASK_RUNNING);
    pipe_fixture.writers = 0;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == 0);
    pipe_fixture.readers = 0;
    pin(&test_tasks[0], 4, &write_end);
    assert(syscall_io_write(4, (uintptr_t)user_bytes, 1) == -RELIEFOS_EPIPE);
    assert(signal_count == 1 && signal_pid == test_tasks[0].pid && signal_number == 13);
}

static void test_null_access_and_full_count(void)
{
    reset_pipe();
    struct task_file file = {.used = 1, .flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_RDWR};
    pin(&test_tasks[0], 3, &file);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, sizeof(user_bytes)) == sizeof(user_bytes));
    assert(syscall_io_read(3, (uintptr_t)user_bytes, sizeof(user_bytes)) == 0);
    file.flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_RDONLY;
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
    file.flags = TASK_FILE_FLAG_DEV_NULL | RELIEFOS_O_WRONLY;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 1) == -RELIEFOS_EBADF);
    file.kind = TASK_FILE_KIND_SIGNALFD;
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -LINUX_EINVAL);
}

static void test_null_device_large_writes(void)
{
    reset_pipe();
    struct task_file file = {
        .used = 1,
        .node = {.type = RELIEFOS_FS_TYPE_DEVICE,
                 .flags = STORAGE_NODE_FLAG_DEV_NODE,
                 .first_cluster = STORAGE_DEV_KIND_NULL},
    };
    const uint32_t kinds[] = {
        TASK_FILE_FLAG_DEV_NODE | TASK_FILE_FLAG_DEV_NULL,
        TASK_FILE_FLAG_DEV_NODE,
        TASK_FILE_FLAG_DEV_NULL,
    };
    const uint64_t sizes[] = {65536, sizeof(user_bytes), 4097, 4096, 1, 0};
    pin(&test_tasks[0], 3, &file);
    for (size_t kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); ++kind) {
        file.flags = kinds[kind] | RELIEFOS_O_RDWR;
        for (size_t size = 0; size < sizeof(sizes) / sizeof(sizes[0]); ++size)
            assert(syscall_io_write(3, (uintptr_t)user_bytes, sizes[size]) == (int64_t)sizes[size]);
        assert(syscall_io_write(3, 0, 1) == -RELIEFOS_EFAULT);
        file.flags = kinds[kind] | RELIEFOS_O_RDONLY;
        assert(syscall_io_write(3, (uintptr_t)user_bytes, 65536) == -RELIEFOS_EBADF);
        assert(syscall_io_write(3, (uintptr_t)user_bytes, 0) == -RELIEFOS_EBADF);
        file.flags = kinds[kind] | RELIEFOS_O_WRONLY | RELIEFOS_O_NONBLOCK;
        assert(syscall_io_write(3, (uintptr_t)user_bytes, sizeof(user_bytes)) == sizeof(user_bytes));
        file.flags |= TASK_FILE_FLAG_PATH;
        assert(syscall_io_write(3, (uintptr_t)user_bytes, 65536) == -RELIEFOS_EBADF);

        /* Null writes must not access source bytes, so model a large valid
         * user span without allocating or touching multiple GiB on the host. */
        user_range_bytes = UINT64_C(0x100000000);
        file.flags = kinds[kind] | RELIEFOS_O_WRONLY;
        assert(syscall_io_write(3, (uintptr_t)user_bytes, UINT64_C(0x7fffefff)) == INT64_C(0x7fffefff));
        assert(syscall_io_write(3, (uintptr_t)user_bytes, UINT64_C(0x7ffff000)) == INT64_C(0x7ffff000));
        assert(syscall_io_write(3, (uintptr_t)user_bytes, UINT64_C(0x7ffff001)) == INT64_C(0x7ffff000));
        assert(syscall_io_write(3, (uintptr_t)user_bytes, UINT64_C(0x100000000)) == INT64_C(0x7ffff000));
        user_range_bytes = sizeof(user_bytes);
    }
}

static void test_eventfd_results(void)
{
    reset_pipe();
    struct task_file file = {.used = 1, .flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR};
    pin(&test_tasks[0], 3, &file);
    uint64_t value = 7;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 7) == -RELIEFOS_EINVAL);
    assert(file.aux == 0);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == 8);
    assert(file.aux == 7);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == 8);
    memcpy(&value, user_bytes, sizeof(value));
    assert(value == 7 && file.aux == 0);
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EAGAIN);
    file.aux = 3; file.aux2 = 1;
    assert(syscall_io_read(3, (uintptr_t)user_bytes, 8) == 8);
    memcpy(&value, user_bytes, sizeof(value));
    assert(value == 1 && file.aux == 2);
    value = UINT64_MAX;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EINVAL);
    value = UINT64_MAX - 1;
    memcpy(user_bytes, &value, sizeof(value));
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 8) == -RELIEFOS_EAGAIN);
    assert(file.aux == 2);
}

static void test_path_descriptor_write(void)
{
    reset_pipe();
    struct task_file file = {.used = 1, .flags = TASK_FILE_FLAG_PATH |
        TASK_FILE_FLAG_DEV_NODE | TASK_FILE_FLAG_DEV_BLOCK | RELIEFOS_O_WRONLY};
    pin(&test_tasks[0], 3, &file);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 512) == -RELIEFOS_EBADF);
    assert(syscall_io_write(3, (uintptr_t)user_bytes, 0) == -RELIEFOS_EBADF);
}

static void test_all_regular_providers_use_one_read_path(void)
{
    const uint32_t kinds[]={STORAGE_NODE_FLAG_PROC,STORAGE_NODE_FLAG_SYSFS,0};
    reset_pipe();
    for(unsigned i=0;i<sizeof(kinds)/sizeof(kinds[0]);++i) {
        struct task_file file={.used=1,.flags=RELIEFOS_O_RDWR,
            .node={.type=RELIEFOS_FS_TYPE_FILE,.flags=kinds[i]}};
        pin(&test_tasks[0],3,&file);io_expected_regular=&file;io_expected_writing=false;
        assert(syscall_io_read(3,(uintptr_t)user_bytes,23)==23);
        io_expected_writing=true;
        assert(syscall_io_write(3,(uintptr_t)user_bytes,23)==23); /* Diagnostic name is empty. */
        file.node.type=RELIEFOS_FS_TYPE_DIR;io_expected_writing=false;
        assert(syscall_io_read(3,(uintptr_t)user_bytes,sizeof(struct reliefos_dir_entry))==-RELIEFOS_EISDIR);
        file.flags=RELIEFOS_O_WRONLY;
        assert(syscall_io_read(3,(uintptr_t)user_bytes,1)==-RELIEFOS_EBADF);
    }
    io_expected_regular=NULL;
}

int main(void)
{
    assert(pipe_wake_regressions() == 0);
    test_redirected_stdio_and_validation();
    test_pipe_wait_retry_eof_and_signal();
    test_null_access_and_full_count();
    test_null_device_large_writes();
    test_eventfd_results();
    test_path_descriptor_write();
    test_path_descriptor_rejects_io();
    test_all_regular_providers_use_one_read_path();
    puts("syscall I/O: redirected stdio, EFAULT, EBADF, pipe wait/retry/EOF/SIGPIPE, null and eventfd passed");
    return 0;
}
