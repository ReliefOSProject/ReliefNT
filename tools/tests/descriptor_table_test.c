#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <sound/asound.h>
#include <linux/soundcard.h>
#include <linux/poll.h>
#include <reliefnt/sched.h>

static struct task *descriptor_dispatch_task;
static unsigned descriptor_dispatch_sleep_count;
static struct task *descriptor_dispatch_current(void)
{ return descriptor_dispatch_task; }
static uint32_t descriptor_dispatch_pid(void)
{ return descriptor_dispatch_task ? descriptor_dispatch_task->pid : 0; }
static void descriptor_dispatch_sleep(uint64_t deadline)
{ (void)deadline; ++descriptor_dispatch_sleep_count; }
static int64_t descriptor_dispatch_regs(struct task *task, uint64_t number,
                                        uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5);
static int64_t descriptor_dispatch_clone(const struct trap_frame *frame,
                                         uint64_t flags, uint64_t stack,
                                         uint64_t parent_tid, uint64_t child_tid,
                                         uint64_t tls);
static int64_t descriptor_dispatch_fork(const struct trap_frame *frame);
static int64_t descriptor_dispatch_vfork(const struct trap_frame *frame);

#define sched_current_task descriptor_dispatch_current
#define sched_current_pid descriptor_dispatch_pid
#define sched_sleep_current_until descriptor_dispatch_sleep
#define sched_clone_current descriptor_dispatch_clone
#define sched_fork_current descriptor_dispatch_fork
#define sched_vfork_current descriptor_dispatch_vfork
#define RELIEFOS_SYSCALL_FRAME_DISPATCH descriptor_dispatch_regs
#include "modules/syscall_dispatch.h"
#undef RELIEFOS_SYSCALL_FRAME_DISPATCH
#undef sched_vfork_current
#undef sched_fork_current
#undef sched_clone_current
#undef sched_current_task
#undef sched_current_pid
#undef sched_sleep_current_until

/* This descriptor fixture never registers a DRM node or PRIME surface. */
void drm_file_close(struct task_file *file)
{ if (file) file->drm_file = NULL; }
void drm_prime_file_close(struct task_file *file) { (void)file; }
void drm_vmwgfx_cancel_task_continuation(struct task *task) { (void)task; }
uint32_t drm_vmwgfx_process_retirements(uint32_t budget)
{ (void)budget; return 0; }

static unsigned descriptor_drm_read_count;
int drm_file_read(struct task *task, struct task_file *file,
                  void *buffer, uint32_t length)
{
    assert(task == descriptor_dispatch_task && file && file->drm_file);
    assert(buffer && length == 1);
    ++descriptor_drm_read_count;
    return (file->flags & RELIEFOS_O_NONBLOCK) ? -RELIEFOS_EAGAIN
                                                : KERNEL_SYSCALL_BLOCKED;
}
int64_t drm_file_ioctl(struct task *task, struct task_file *file,
                       uint32_t command, uint64_t argument)
{ (void)task; (void)file; (void)command; (void)argument; abort(); }
uint32_t drm_file_poll(struct task_file *file, uint32_t events)
{ (void)file; (void)events; abort(); }


int64_t syscall_clone3(const struct trap_frame *frame, uint64_t arguments,
                       uint64_t size)
{ (void)frame; (void)arguments; (void)size; abort(); }
int64_t kernel_signal_rt_sigreturn(struct task *task, struct trap_frame *frame)
{ (void)task; (void)frame; abort(); }
void net_close_owner_sockets(uint32_t owner_pid) { (void)owner_pid; }
void net_tcp_maintenance(void) { }

static int64_t descriptor_dispatch_regs(struct task *task, uint64_t number,
                                        uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    assert((number == LINUX_SYS_READ || number == LINUX_SYS_READV) &&
           task == descriptor_dispatch_task);
    struct task_file *file = task_file_for_io(task, (int)a0);
    assert(file && file->drm_file);
    if (number == LINUX_SYS_READV) {
        assert(a2 == 1);
        const struct iovec *vector = (const struct iovec *)(uintptr_t)a1;
        return drm_file_read(task, file, vector[0].iov_base,
                             (uint32_t)vector[0].iov_len);
    }
    return drm_file_read(task, file, (void *)(uintptr_t)a1, (uint32_t)a2);
}
static int64_t descriptor_dispatch_clone(const struct trap_frame *frame,
                                         uint64_t flags, uint64_t stack,
                                         uint64_t parent_tid, uint64_t child_tid,
                                         uint64_t tls)
{ (void)frame; (void)flags; (void)stack; (void)parent_tid; (void)child_tid; (void)tls; abort(); }
static int64_t descriptor_dispatch_fork(const struct trap_frame *frame)
{ (void)frame; abort(); }
static int64_t descriptor_dispatch_vfork(const struct trap_frame *frame)
{ (void)frame; abort(); }

void driver_manager_process_audio_faults(void) { }
void input_process_pending(void) { }
/* No task in this descriptor fixture owns a live CPU return reservation. */
void arch_set_user_fs(uint64_t base) { (void)base; abort(); }
void storage_set_io_async_context(bool enabled) { (void)enabled; }
void storage_release_task_io(uint32_t pid) { (void)pid; }
int kernel_signal_queue_task(struct task *task, int signal_number)
{ (void)task; (void)signal_number; abort(); }

#define AUDIO_FIXTURE_REAL_WAIT
#define AUDIO_FIXTURE_NO_PTHREAD
#include "audio_fixture.h"
#include "audio_fake_card.h"
#include "audio_device_fixture.h"
#include "../../sound/core/pcm_internal.h"
extern struct audio_pcm pcm_slots[AUDIO_PCM_MAX];

/** @brief Map only PCM static storage through this fixture's identity alias.
 * @param address Candidate CPU alias.
 * @return Identity physical address inside pcm_slots, or zero for unrelated
 * host/user addresses.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    uintptr_t start = (uintptr_t)&pcm_slots[0];
    size_t bytes = sizeof(pcm_slots);
    return value >= start && value - start < bytes ? (uint64_t)value : 0;
}

/* Socket scheduling selects a host task; OFDs, queues, object handles and
 * sendmsg/recvmsg are production implementations. No case may park or signal. */
static struct task *descriptor_socket_task;
static struct task *descriptor_socket_current(void)
{ assert(descriptor_socket_task); return descriptor_socket_task; }
static void descriptor_socket_park(void) { abort(); }
static void descriptor_socket_sleep(uint64_t deadline) { (void)deadline; abort(); }
static int descriptor_socket_signal(uint32_t pid, int signal)
{ (void)pid; (void)signal; abort(); }
#define sched_current_task descriptor_socket_current
#define sched_block_current descriptor_socket_park
#define sched_sleep_current_until descriptor_socket_sleep
#define sched_signal_user_task descriptor_socket_signal
#include "../../kernel/syscall/ipc/socket.c"
#undef sched_current_task
#undef sched_block_current
#undef sched_sleep_current_until
#undef sched_signal_user_task

static int fail_allocation;
static unsigned pty_references;
static unsigned console_references;
void *kernel_malloc(size_t size) { return fail_allocation ? NULL : malloc(size); }
void kernel_free(void *memory) { free(memory); }
/* Descriptor scheduling fixtures model a single active CPU, the BSP. */
uint32_t smp_cpu_count(void) { return 1; }
bool smp_cpu_online(uint32_t cpu) { return cpu == 0; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
/* OSS tests bind a registered PCM. Legacy discovery reports no hardware;
 * entering a legacy backend after that result is a failure. */
void driver_manager_audio_get_state(struct reliefos_audio_state *state)
{ *state = (struct reliefos_audio_state){0}; }
int driver_manager_audio_configure(const struct reliefos_audio_format *format)
{ (void)format; abort(); }
long driver_manager_audio_write(const void *data, uint32_t count, uint32_t *status)
{ (void)data; (void)count; (void)status; abort(); }
int driver_manager_audio_acquire(uint32_t *generation, struct reliefos_audio_state *state)
{ assert(generation && state); return -ENODEV; }
void driver_manager_audio_release(uint32_t generation)
{ (void)generation; abort(); }
int driver_manager_audio_configure_bound(uint32_t generation,
                                         const struct reliefos_audio_format *format)
{ (void)generation; (void)format; abort(); }
long driver_manager_audio_write_bound(uint32_t generation, const void *data,
                                      uint32_t count, uint32_t *status)
{ (void)generation; (void)data; (void)count; (void)status; abort(); }
int driver_manager_audio_state_bound(uint32_t generation, struct reliefos_audio_state *state)
{ (void)generation; (void)state; abort(); }
/* This descriptor test uses anonymous objects, never storage-backed files. */
int storage_inode_put(struct storage_inode_ref *inode) { assert(!inode); return 0; }
int storage_disk_block_write(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written)
{ (void)disk_id; (void)partition_index; (void)offset; (void)buffer; (void)length; (void)out_written; abort(); }
int64_t pty_write_input(uint32_t owner_pid, uint32_t pty_id,
                        const char *buffer, uint32_t length)
{ (void)owner_pid; (void)pty_id; (void)buffer; (void)length; abort(); }
int64_t pty_write_output(uint32_t pty_id, const char *buffer, uint32_t length)
{ (void)pty_id; (void)buffer; (void)length; abort(); }
int64_t pty_check_change(uint32_t pty_id, uint32_t caller_pid, int signal_number)
{ (void)pty_id; (void)caller_pid; (void)signal_number; abort(); }
int input_evdev_write(uint32_t device_kind, const void *buffer, uint32_t length)
{ (void)device_kind; (void)buffer; (void)length; abort(); }
void console_serial_write_len(const char *text, size_t length)
{ (void)text; (void)length; abort(); }
void console_write_len(const char *text, size_t length)
{ (void)text; (void)length; abort(); }
void serial_write(const char *text) { (void)text; abort(); }
int task_pipe_write(struct task_file *file, const void *buffer, uint32_t length)
{ (void)file; (void)buffer; (void)length; abort(); }
int64_t drm_prime_file_rw(const struct task_file *file, bool writing)
{ (void)file; (void)writing; abort(); }
int task_shm_write(struct task_file *file, const void *buffer, uint32_t length)
{ (void)file; (void)buffer; (void)length; abort(); }
int64_t syscall_regular_io(struct task *task, struct task_file *file,
                           uint64_t buffer, uint64_t count, uint64_t position,
                           bool writing, bool positional)
{ (void)task; (void)file; (void)buffer; (void)count; (void)position; (void)writing; (void)positional; abort(); }
int fs_permissions_resolve_flags(const struct task *task, const char *base, const char *input,
                                char *out, uint32_t cap, bool real_ids, uint32_t flags)
{ (void)task; (void)base; (void)input; (void)out; (void)cap; (void)real_ids; (void)flags; abort(); }
void task_pipe_release(struct task_file *file) { (void)file; }
void task_shm_release(struct task_file *file) { (void)file; }
/* AF_UNIX audio transfers must not enter storage or IPv4 backends. */
int fs_permissions_check(const struct task *task, const char *path, uint32_t access, bool real_ids)
{ (void)task; (void)path; (void)access; (void)real_ids; abort(); }
void task_packet_release(struct task_file *file) { (void)file; abort(); }
void task_udp_release(struct task_file *file) { (void)file; abort(); }
void net_socket_release_fd(int32_t handle) { (void)handle; abort(); }
int64_t syscall_socket_mmsg(bool receiving, int fd, uint64_t vector,
                            uint32_t length, uint32_t flags, uint64_t timeout)
{ (void)receiving; (void)fd; (void)vector; (void)length; (void)flags; (void)timeout; abort(); }
int64_t syscall_udp(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5)
{ (void)number; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; abort(); }
int64_t syscall_packet(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5)
{ (void)number; (void)a0; (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; abort(); }
int net_socket_open(struct reliefos_net_socket_open *request, uint32_t pid, uint32_t uid)
{ (void)request; (void)pid; (void)uid; abort(); }
int net_socket_connect_fd(int32_t handle, uint32_t ip, uint16_t port, bool nonblock)
{ (void)handle; (void)ip; (void)port; (void)nonblock; abort(); }
int net_socket_address(int32_t handle, uint32_t owner, uint32_t *local_ip,
                       uint16_t *local_port, uint32_t *remote_ip, uint16_t *remote_port)
{ (void)handle; (void)owner; (void)local_ip; (void)local_port; (void)remote_ip; (void)remote_port; abort(); }
int net_socket_shutdown_fd(int32_t handle, int how)
{ (void)handle; (void)how; abort(); }
int net_socket_close(struct reliefos_net_socket_close *request, uint32_t owner)
{ (void)request; (void)owner; abort(); }
void net_socket_pin_fd(int32_t handle) { (void)handle; abort(); }
int storage_lookup_path(const char *path, struct storage_node *out)
{ (void)path; (void)out; abort(); }
int fs_permissions_parent(const struct task *task, const char *path, bool deleting)
{ (void)task; (void)path; (void)deleting; abort(); }
int storage_create_socket(const char *path, struct storage_node *out)
{ (void)path; (void)out; abort(); }
int fs_permissions_create(const struct task *task, const char *path,
                          const struct storage_node *node, uint32_t mode)
{ (void)task; (void)path; (void)node; (void)mode; abort(); }
int storage_unlink(const char *path) { (void)path; abort(); }
/* The direct CLOSE regression retains the legacy descriptor dispatcher; these
 * unrelated branches remain unreachable in this fixture. */
int pty_lookup_vt_path(const char *path, struct storage_node *node)
{ (void)path; (void)node; return -2; }
int task_fifo_open(struct task *task, const struct storage_node *node,
                   uint32_t flags, const char *path)
{ (void)task; (void)node; (void)flags; (void)path; abort(); }
int syscall_ipc_pipe(uint64_t user_ptr) { (void)user_ptr; abort(); }
int syscall_ipc_pipe2(uint64_t user_ptr, uint64_t flags)
{ (void)user_ptr; (void)flags; abort(); }
int storage_truncate_held_node(struct storage_node *node, uint64_t length)
{ (void)node; (void)length; abort(); }
int task_shm_truncate(struct task_file *file, uint64_t size)
{ (void)file; (void)size; abort(); }
int64_t task_pipe_capacity(struct task_file *file, bool setting, uint32_t bytes)
{ (void)file; (void)setting; (void)bytes; abort(); }
int fs_permissions_search(const struct task *task, const char *path, bool real_ids)
{ (void)task; (void)path; (void)real_ids; abort(); }
int storage_truncate_file(const char *path, uint64_t length)
{ (void)path; (void)length; abort(); }
int64_t task_shm_get_seals(const struct task_file *file) { (void)file; abort(); }
int task_shm_add_seals(struct task_file *file, uint32_t seals)
{ (void)file; (void)seals; abort(); }
int proc_lookup(const char *path, struct storage_node *out)
{ (void)path; (void)out; abort(); }
size_t user_strlen(const char *text, size_t maximum)
{ (void)text; (void)maximum; abort(); }
int storage_inode_refresh(struct storage_node *node) { (void)node; abort(); }
int task_packet_recv(struct task_file *file, void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_packet_send(struct task_file *file, const void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_udp_recv(struct task_file *file, void *data, uint32_t length,
                  uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
int task_udp_send(struct task_file *file, const void *data, uint32_t length,
                  uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)data; (void)length; (void)flags; (void)address; (void)address_length; abort(); }
short net_socket_poll_fd(int32_t handle, uint32_t owner, short events)
{ (void)handle; (void)owner; (void)events; abort(); }
uint32_t drm_prime_file_poll(const struct task_file *file, uint32_t events)
{ (void)file; (void)events; abort(); }
short task_pipe_poll(const struct task_file *file, short events)
{ (void)file; (void)events; abort(); }
short task_packet_poll(struct task_file *file, short events)
{ (void)file; (void)events; abort(); }
short task_udp_poll(struct task_file *file, short events)
{ (void)file; (void)events; abort(); }
short task_signalfd_poll(struct task *task, const struct task_file *file)
{ (void)task; (void)file; abort(); }
int input_evdev_available_vt(uint32_t kind, uint64_t cursor,
                             uint64_t grab_token, uint32_t vt)
{ (void)kind; (void)cursor; (void)grab_token; (void)vt; abort(); }
int pty_is_hungup(uint32_t pty_id) { (void)pty_id; return 0; }
static uint32_t descriptor_pty_input_available;
uint32_t pty_input_available(uint32_t pty_id)
{ return pty_id == 7 ? descriptor_pty_input_available : 0; }
uint32_t pty_output_available(uint32_t pty_id) { (void)pty_id; return 0; }
uint32_t pty_vt_number(uint32_t pty_id) { (void)pty_id; return 1; }
uint64_t net_socket_get_timeout_ticks(int32_t handle, bool send)
{ (void)handle; (void)send; abort(); }
int net_socket_set_timeout_ticks(int32_t handle, bool send, uint64_t ticks)
{ (void)handle; (void)send; (void)ticks; abort(); }
int net_socket_error(int32_t handle, bool clear) { (void)handle; (void)clear; abort(); }
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner)
{ (void)request; (void)owner; abort(); }
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner)
{ (void)request; (void)owner; abort(); }
uint64_t time_ticks(void) { return 100; }
void input_evdev_release(uint32_t kind, uint64_t token)
{ (void)kind; (void)token; }
void pty_reap_hungup(uint32_t id) { (void)id; }
static uint32_t descriptor_active_vt = 1;
uint32_t pty_vt_active(void) { return descriptor_active_vt; }
uint32_t pty_vt_id(uint32_t number) { return number >= 1 && number <= 6 ? number : 0; }
int pty_is_active(uint32_t id)
{ return id == 7 || id == 65536u || id == 0x7fffffffu; }
int pty_slave_open_allowed(uint32_t id) { return id == 7; }
int pty_transfer_get(uint32_t id, uint32_t endpoint)
{
    assert(endpoint == TASK_PTY_ENDPOINT_SLAVE);
    if (id >= 1 && id <= 6) ++console_references;
    else { assert(id == 7); ++pty_references; }
    return 0;
}
void pty_transfer_put(uint32_t id, uint32_t endpoint)
{
    assert(endpoint == TASK_PTY_ENDPOINT_SLAVE);
    if (id >= 1 && id <= 6) { assert(console_references); --console_references; }
    else { assert(id == 7 && pty_references); --pty_references; }
}
bool user_range_ok(uint64_t address, uint64_t length)
{ return address >= 4096 && length <= UINT64_MAX - address; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_range_ok(address, length); }

static int64_t descriptor_control_value;
static uint32_t descriptor_control_count(void *opaque)
{ (void)opaque; return 1; }
static int descriptor_control_info(void *opaque, uint32_t control,
                                    struct audio_control_info *info)
{
    (void)opaque;
    assert(control == 0);
    *info = (struct audio_control_info){.id = 1, .type = AUDIO_CONTROL_INTEGER,
        .count = 1, .min = 0, .max = 40, .step = 1,
        .access = AUDIO_CONTROL_READ | AUDIO_CONTROL_WRITE};
    strcpy(info->name, "Playback Volume");
    return 0;
}
static int descriptor_control_read(void *opaque, uint32_t control,
                                    struct audio_control_value *value)
{
    (void)opaque; assert(control == 0);
    *value = (struct audio_control_value){.values = {descriptor_control_value}};
    return 0;
}
static int descriptor_control_write(void *opaque, uint32_t control,
                                     const struct audio_control_value *value)
{
    (void)opaque; assert(control == 0);
    descriptor_control_value = value->values[0];
    return 0;
}

static struct task *descriptor_audio_task(uint32_t pid)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = pid;
    task->limits.nofile.rlim_cur = 64;
    return task;
}

/** @brief Verify AF_UNIX socket allocation is not capped by the legacy slot count. */
static void test_unix_socket_registry_growth(void)
{
    enum { SOCKETS = 40 };
    struct unix_socket *sockets[SOCKETS];
    for (unsigned i = 0; i < SOCKETS; ++i) {
        sockets[i] = unix_alloc(1000u + i, SOCK_DGRAM);
        assert(sockets[i]);
        for (unsigned j = 0; j < i; ++j) {
            assert(sockets[i] != sockets[j]);
            assert(sockets[i]->handle != sockets[j]->handle);
        }
    }
    for (unsigned i = 0; i < SOCKETS; ++i)
        unix_release_handle(sockets[i]->handle);
    assert(!unix_sockets);
    puts("PASS AF_UNIX registry grows beyond the former 32 sockets and releases stable handles");
}

/** @brief Verify Linux AF_UNIX backlog zero, one, and negative boundaries. */
static void test_unix_listener_backlog_boundaries(void)
{
    struct task *task = descriptor_audio_task(1801);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    descriptor_socket_task = task;

    const char zero_name[] = "net-backlog-zero";
    __builtin_memcpy(address.sun_path + 1, zero_name, sizeof(zero_name) - 1);
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                           1 + sizeof(zero_name) - 1);
    int listener_fd = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM, 0, 0, 0, 0);
    assert(listener_fd >= 0);
    assert(syscall_socket_dispatch(__NR_bind, listener_fd,
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd, 0, 0, 0, 0, 0) == 0);
    struct unix_socket *listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->backlog == 0 && listener->pending_limit == 1);
    int zero_clients[2];
    for (unsigned i = 0; i < 2; ++i) {
        zero_clients[i] = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
        assert(zero_clients[i] >= 0);
    }
    assert(syscall_socket_dispatch(__NR_connect, zero_clients[0],
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->pending_count == 1);
    assert(syscall_socket_dispatch(__NR_connect, zero_clients[1],
        (uintptr_t)&address, address_length, 0, 0, 0) == -RELIEFOS_EAGAIN);
    int accepted = (int)syscall_socket_dispatch(__NR_accept, listener_fd,
        0, 0, 0, 0, 0);
    assert(accepted >= 0);
    task_discard_file_fd(task, accepted);
    task_discard_file_fd(task, zero_clients[0]);
    task_discard_file_fd(task, zero_clients[1]);
    task_discard_file_fd(task, listener_fd);

    const char one_name[] = "net-backlog-one";
    address = (struct sockaddr_un){.sun_family = AF_UNIX};
    __builtin_memcpy(address.sun_path + 1, one_name, sizeof(one_name) - 1);
    address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 1 + sizeof(one_name) - 1);
    listener_fd = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM, 0, 0, 0, 0);
    assert(listener_fd >= 0);
    assert(syscall_socket_dispatch(__NR_bind, listener_fd,
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd, 1, 0, 0, 0, 0) == 0);
    listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->backlog == 1 && listener->pending_limit == 2);
    int one_clients[3];
    for (unsigned i = 0; i < 3; ++i) {
        one_clients[i] = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
            SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
        assert(one_clients[i] >= 0);
    }
    for (unsigned i = 0; i < 2; ++i) {
        assert(syscall_socket_dispatch(__NR_connect, one_clients[i],
            (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    }
    listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->pending_count == 2);
    assert(syscall_socket_dispatch(__NR_connect, one_clients[2],
        (uintptr_t)&address, address_length, 0, 0, 0) == -RELIEFOS_EAGAIN);
    for (unsigned i = 0; i < 2; ++i) {
        accepted = (int)syscall_socket_dispatch(__NR_accept, listener_fd,
            0, 0, 0, 0, 0);
        assert(accepted >= 0);
        task_discard_file_fd(task, accepted);
    }
    for (unsigned i = 0; i < 3; ++i) task_discard_file_fd(task, one_clients[i]);
    task_discard_file_fd(task, listener_fd);

    const char negative_name[] = "net-backlog-negative";
    address = (struct sockaddr_un){.sun_family = AF_UNIX};
    __builtin_memcpy(address.sun_path + 1, negative_name, sizeof(negative_name) - 1);
    address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 1 + sizeof(negative_name) - 1);
    listener_fd = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM, 0, 0, 0, 0);
    assert(listener_fd >= 0);
    assert(syscall_socket_dispatch(__NR_bind, listener_fd,
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd,
        (uint64_t)(int64_t)-1, 0, 0, 0, 0) == 0);
    listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->backlog == 4096 && listener->pending_limit == 4097);
    assert(unix_pending_reserve(listener, 4097) == 0);
    assert(listener->pending_capacity == 4097);
    listener->pending_count = 4096;
    int last_client = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    int overflow_client = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM | SOCK_NONBLOCK, 0, 0, 0, 0);
    assert(last_client >= 0 && overflow_client >= 0);
    assert(syscall_socket_dispatch(__NR_connect, last_client,
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    assert(listener->pending_count == 4097);
    uint32_t last_server = listener->pending[4096];
    assert(syscall_socket_dispatch(__NR_connect, overflow_client,
        (uintptr_t)&address, address_length, 0, 0, 0) == -RELIEFOS_EAGAIN);
    listener->pending[0] = last_server;
    listener->pending_count = 1;
    accepted = (int)syscall_socket_dispatch(__NR_accept, listener_fd,
        0, 0, 0, 0, 0);
    assert(accepted >= 0);
    task_discard_file_fd(task, accepted);
    task_discard_file_fd(task, last_client);
    task_discard_file_fd(task, overflow_client);
    task_discard_file_fd(task, listener_fd);

    assert(!unix_sockets);
    descriptor_socket_task = NULL;
    sched_task_file_release(task);
    free(task);
    puts("PASS AF_UNIX listen backlog zero, one, and negative Linux capacity boundaries");
}

/** @brief Verify listen accepts a Linux-sized backlog beyond the old eight entries. */
static void test_unix_listener_backlog_growth(void)
{
    enum { CLIENTS = 12 };
    struct task *task = descriptor_audio_task(1800);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    const char name[] = "net-backlog";
    socklen_t address_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                           1 + sizeof(name) - 1);
    int client_fds[CLIENTS];
    __builtin_memcpy(address.sun_path + 1, name, sizeof(name) - 1);
    descriptor_socket_task = task;
    int listener_fd = (int)syscall_socket_dispatch(__NR_socket, AF_UNIX,
        SOCK_STREAM, 0, 0, 0, 0);
    assert(listener_fd >= 0);
    assert(syscall_socket_dispatch(__NR_bind, listener_fd,
        (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd, 20, 0, 0, 0, 0) == 0);
    assert(unix_from_file(task_file_for_fd(task, listener_fd))->backlog == 20);
    assert(unix_from_file(task_file_for_fd(task, listener_fd))->pending_limit == 21);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd, 5000, 0, 0, 0, 0) == 0);
    assert(unix_from_file(task_file_for_fd(task, listener_fd))->backlog == 4096);
    assert(unix_from_file(task_file_for_fd(task, listener_fd))->pending_limit == 4097);
    assert(syscall_socket_dispatch(__NR_listen, listener_fd, 20, 0, 0, 0, 0) == 0);
    for (unsigned i = 0; i < CLIENTS; ++i) {
        client_fds[i] = (int)syscall_socket_dispatch(__NR_socket,
            AF_UNIX, SOCK_STREAM, 0, 0, 0, 0);
        assert(client_fds[i] >= 0);
        assert(syscall_socket_dispatch(__NR_connect, client_fds[i],
            (uintptr_t)&address, address_length, 0, 0, 0) == 0);
    }
    struct unix_socket *listener = unix_from_file(task_file_for_fd(task, listener_fd));
    assert(listener->pending_count == CLIENTS);
    for (unsigned i = 0; i < CLIENTS; ++i) {
        int accepted = (int)syscall_socket_dispatch(__NR_accept, listener_fd,
            0, 0, 0, 0, 0);
        assert(accepted >= 0);
        task_discard_file_fd(task, accepted);
        task_discard_file_fd(task, client_fds[i]);
    }
    task_discard_file_fd(task, listener_fd);
    assert(!unix_sockets);
    descriptor_socket_task = NULL;
    sched_task_file_release(task);
    free(task);
    puts("PASS AF_UNIX listen backlog grows beyond eight, clamps at Linux somaxconn, and accepts twelve queued clients");
}

static int descriptor_audio_open(struct task *task, uint32_t card, int control)
{
    struct task_file *file;
    int fd = task_allocate_fd(task, 0, &file);
    assert(fd == 3);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_RDWR;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE | (control ? STORAGE_NODE_FLAG_AUDIO_CONTROL : STORAGE_NODE_FLAG_AUDIO_PCM),
        .first_cluster = STORAGE_DEV_KIND_AUDIO,
        .volume_id = control ? card : AUDIO_DEVICE_VOLUME_ID(card, 0, AUDIO_PLAYBACK)};
    assert((control ? audio_control_open(task, file) : audio_device_open(task, file)) == 0);
    if (!control) audio_test_configure_file(file);
    return fd;
}

/* Missing final close must leave the observer locked out; premature close
 * must break the child/pinned read. These test actual syscall OFD helpers. */
static void test_audio_control_descriptions(void)
{
    struct audio_test_card card = {0};
    struct audio_card_ops ops = audio_test_ops;
    ops.control_count = descriptor_control_count;
    ops.control_info = descriptor_control_info;
    ops.control_read = descriptor_control_read;
    ops.control_write = descriptor_control_write;
    struct audio_card_identity identity = {.id = "ofd", .name = "ofd-test"};
    assert(audio_register_card_owned(&identity, &ops, &card, 0x7a, &card.id) == 0);
    struct task *parent = descriptor_audio_task(1000), *child = descriptor_audio_task(1001);
    struct task *observer = descriptor_audio_task(1002);
    int fd = descriptor_audio_open(parent, card.id, 1);
    int observer_fd = descriptor_audio_open(observer, card.id, 1);
    struct task_file *source = task_file_for_fd(parent, fd);
    struct task_file *independent = task_file_for_fd(observer, observer_fd);
    struct snd_ctl_elem_id id = {.numid = 1, .iface = SNDRV_CTL_ELEM_IFACE_MIXER};
    struct snd_ctl_elem_info info = {.id = id};
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == -1 && !(info.access & (SNDRV_CTL_ELEM_ACCESS_LOCK | SNDRV_CTL_ELEM_ACCESS_OWNER)));
    assert(audio_control_ioctl(parent, source, SNDRV_CTL_IOCTL_ELEM_LOCK, (uintptr_t)&id) == 0);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == 1000 && (info.access & SNDRV_CTL_ELEM_ACCESS_LOCK));
    assert(!(info.access & SNDRV_CTL_ELEM_ACCESS_OWNER));
    int subscribe = 1;
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) == 0);
    struct snd_ctl_elem_value value = {.id = id, .value.integer.value = {25}};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
    fail_allocation = 1;
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == -RELIEFOS_ENOMEM);
    assert(!source->description && !task_file_for_fd(parent, 4));
    fail_allocation = 0;
    int duplicate = task_duplicate_file_fd(parent, fd, 0, 0);
    assert(duplicate == 4);
    source = task_file_for_fd(parent, fd);
    assert(source == task_file_for_fd(parent, duplicate) && source->references == 2);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(source == task_file_for_fd(child, fd) && source->references == 4);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(source->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == 1000 && (info.access & SNDRV_CTL_ELEM_ACCESS_OWNER));
    assert(audio_alsa_control_ioctl(task_file_for_fd(child, duplicate)->audio_control_file,
                                    SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
    struct snd_ctl_event event;
    assert(audio_control_read_file(task_file_for_fd(child, fd)->audio_control_file, &event, sizeof(event)) == sizeof(event));
    assert(event.data.elem.id.numid == 1 && event.data.elem.mask == 1);
    assert(audio_control_read_file(source->audio_control_file, &event, sizeof(event)) == -EAGAIN);
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(parent->syscall_file == task_file_description(source));
    assert(source->references == 5);
    clear_task_files(parent);
    clear_task_files(child);
    assert(source->references == 1);
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
    audio_control_changed(card.id, 0);
    assert(audio_control_read_file(parent->syscall_file->audio_control_file, &event, sizeof(event)) == sizeof(event));
    task_release_syscall_file(parent);
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
    info = (struct snd_ctl_elem_info){.id = id};
    assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_INFO, &info) == 0);
    assert(info.owner == -1 && !(info.access & (SNDRV_CTL_ELEM_ACCESS_LOCK | SNDRV_CTL_ELEM_ACCESS_OWNER)));
    clear_task_files(observer);
    sched_task_file_release(parent); sched_task_file_release(child); sched_task_file_release(observer);
    free(parent); free(child); free(observer);
    assert(audio_unregister_card(card.id, 0) == 0);
    puts("PASS actual control OFD: dup/fork shared queue/owner, ENOMEM rollback, I/O pin and final unlock");
}

static void test_audio_pcm_descriptions(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *parent = descriptor_audio_task(1100), *child = descriptor_audio_task(1101);
    int fd = descriptor_audio_open(parent, card.id, 0);
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == 4);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    struct task_file *source = task_file_for_fd(parent, fd);
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(parent->syscall_file == task_file_description(source));
    assert(card.open_count == 1 && card.close_count == 0);
    clear_task_files(parent); clear_task_files(child);
    assert(!syscall_ioctl_resolve_fd(parent, fd));
    assert(task_file_for_io(parent, fd) == parent->syscall_file);
    struct task_file *replacement;
    assert(task_allocate_fd(parent, 0, &replacement) == fd);
    replacement->used = 1;
    replacement->flags = TASK_FILE_FLAG_DEV_NULL;
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    assert(task_file_for_io(parent, fd) != task_file_for_fd(parent, fd));
    assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    uint8_t samples[4] = {0};
    assert(audio_device_write(NULL, parent->syscall_file, samples, sizeof(samples)) == 4);
    task_release_syscall_file(parent);
    clear_task_files(parent);
    assert(card.close_count == 1 && audio_unregister_card(card.id, 0) == 0);
    sched_task_file_release(parent); sched_task_file_release(child);
    free(parent); free(child);
    puts("PASS actual PCM OFD: dup/fork/exit retain stream until last in-flight I/O reference");
}

static void descriptor_socket_pair(struct task *sender, struct task *receiver,
                                    int *send_fd, int *receive_fd)
{
    struct unix_socket *left = unix_alloc(sender->pid, SOCK_DGRAM);
    struct unix_socket *right = unix_alloc(receiver->pid, SOCK_DGRAM);
    assert(left && right);
    left->state = right->state = UNIX_SOCKET_CONNECTED;
    left->peer_handle = right->handle;
    right->peer_handle = left->handle;
    *send_fd = unix_alloc_fd(sender, left);
    *receive_fd = unix_alloc_fd(receiver, right);
    assert(*send_fd == 4 && *receive_fd == 3);
    unix_release_handle(left->handle);
    unix_release_handle(right->handle);
}

static void test_audio_oss_descriptions(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *parent = descriptor_audio_task(1200), *child = descriptor_audio_task(1201);
    struct task_file *file;
    int fd = task_allocate_fd(parent, 0, &file);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_WRONLY;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_AUDIO};
    assert(!audio_oss_open(parent, file));
    assert(card.open_count == 1 && audio_unregister_card(card.id, 0) == -EBUSY);
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == 4);
    child->fd_table = parent->fd_table;
    assert(!syscall_clone_task_files(parent, child));
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd));
    struct task_file *held = parent->syscall_file;
    assert(held == task_file_for_fd(child, 4));
    assert(!audio_oss_ioctl(child, task_file_for_fd(child, 4), SNDCTL_DSP_NONBLOCK, 0));
    assert(held->flags & RELIEFOS_O_NONBLOCK);
    clear_task_files(parent); clear_task_files(child);
    assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    uint8_t data[4] = {0};
    assert(audio_oss_write(NULL, held, data, sizeof(data)) == sizeof(data));
    task_release_syscall_file(parent);
    assert(card.close_count == 1 && !audio_unregister_card(card.id, 0));
    sched_task_file_release(parent); sched_task_file_release(child);
    free(parent); free(child);
    puts("PASS actual OSS OFD: open lease, shared NONBLOCK, dup/fork/exit and final ioctl pin release");
}

static void test_audio_write_batch(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task *task = descriptor_audio_task(1300);
    struct task_file *file;
    assert(task_allocate_fd(task, 0, &file) >= 0);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_WRONLY | RELIEFOS_O_NONBLOCK;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_AUDIO};
    assert(!audio_oss_open(task, file));
    int fragments = (8 << 16) | 13; /* A 64 KiB ring on the bounded fake card. */
    assert(!audio_oss_ioctl(task, file, SNDCTL_DSP_SETFRAGMENT, (uintptr_t)&fragments));
    uint8_t data[98304]; /* Entire large partial-write request stays readable. */
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 13u);
    /* One real Doom block must reach the OSS core in one bounded syscall
     * request, rather than five forced 4 KiB returns/scheduling boundaries. */
    uint32_t length = task_device_write_length(file, 5120u * 4u);
    assert(length == 5120u * 4u);
    assert(audio_oss_write(NULL, file, data, length) == (int)length);
    assert(card.prepared_dma.bytes == 65536u);
    assert(!memcmp(card.prepared_dma.kernel, data, length));
    assert(task_device_write_length(file, UINT64_MAX) == 32768u);
    assert(task_device_write_length(file, 0) == 0);
    assert(task_device_write_length(file, 3) == 3);
    assert(audio_oss_write(NULL, file, data + length, 32768u) == 32768);
    uint32_t remaining = 65536u - length - 32768u;
    assert(audio_oss_write(NULL, file, data + length + 32768u, 32768u) == (int)remaining);
    assert(!memcmp(card.prepared_dma.kernel, data, 65536u));
    assert(audio_oss_write(NULL, file, data, 4) == -EAGAIN);
    struct task_file other = {.flags = TASK_FILE_FLAG_DEV_NODE,
        .node = {.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE, .first_cluster = STORAGE_DEV_KIND_ZERO}};
    assert(task_device_write_length(&other, UINT64_MAX) == RELIEFOS_FS_IO_SLICE_BYTES);
    clear_task_files(task);
    assert(!audio_unregister_card(card.id, 0));
    sched_task_file_release(task);
    free(task);
    puts("PASS actual OSS syscall batching: whole Doom block, bounded large request, exact samples, short/EAGAIN");
}

static void test_nonblocking_drm_read_dispatch(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = task->tgid = 1400;
    sched_task_limits(task)->nofile.rlim_cur = 64;
    struct task_file *file;
    int fd = task_allocate_fd(task, 3, &file);
    assert(fd == 3);
    file->flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_RDONLY |
                  RELIEFOS_O_NONBLOCK;
    file->node = (struct storage_node){.type = RELIEFOS_FS_TYPE_DEVICE,
        .flags = STORAGE_NODE_FLAG_DEV_NODE,
        .first_cluster = STORAGE_DEV_KIND_DRM_CARD0};
    file->drm_file = (struct drm_file *)(uintptr_t)1;
    descriptor_dispatch_task = task;
    descriptor_dispatch_sleep_count = 0;
    descriptor_drm_read_count = 0;
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_READ, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_READV, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV, fd));
    assert(syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV2, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PWRITE64, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PWRITEV, fd));
    file->flags &= ~RELIEFOS_O_NONBLOCK;
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREADV, fd));
    file->flags |= RELIEFOS_O_NONBLOCK;
    file->drm_file = NULL;
    assert(!syscall_eagain_is_nonblocking_device(task, LINUX_SYS_PREAD64, fd));
    file->drm_file = (struct drm_file *)(uintptr_t)1;
    char byte;
    struct trap_frame frame = {.rax = LINUX_SYS_READ, .rdi = (uint64_t)fd,
        .rsi = (uintptr_t)&byte, .rdx = 1, .rip = 0x4002, .rsp = 0x8000};

    syscall_dispatch_frame(&frame);

    assert(descriptor_drm_read_count == 1);
    assert((int64_t)frame.rax == -RELIEFOS_EAGAIN);
    assert(frame.rip == 0x4002);
    assert(descriptor_dispatch_sleep_count == 0);
    assert(task->syscall_file == NULL);

    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    frame = (struct trap_frame){.rax = LINUX_SYS_READV, .rdi = (uint64_t)fd,
        .rsi = (uintptr_t)&vector, .rdx = 1, .rip = 0x5002, .rsp = 0x8000};
    syscall_dispatch_frame(&frame);
    assert(descriptor_drm_read_count == 2);
    assert((int64_t)frame.rax == -RELIEFOS_EAGAIN);
    assert(frame.rip == 0x5002);
    assert(descriptor_dispatch_sleep_count == 0);
    assert(task->syscall_file == NULL);

    assert(syscall_drm_read(task, file, (uintptr_t)&byte, 1) == -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 3);
    assert(syscall_drm_pread(task, file, (uintptr_t)&byte, 1, 4096) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 4);
    assert(file->offset == 0);
    assert(syscall_drm_pread(task, file, (uintptr_t)&byte, 1, UINT64_MAX) ==
           -RELIEFOS_EINVAL);
    assert(descriptor_drm_read_count == 4);
    assert(syscall_drm_readv(task, file, &vector, 1, false, 0, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 5);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 6);
    assert(file->offset == 0);
    assert(syscall_drm_readv(task, file, &vector, 1, false, UINT64_MAX, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 7);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 1) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 8);
    assert(syscall_drm_readv(task, file, &vector, 1, true, 4096, 2) ==
           -RELIEFOS_EOPNOTSUPP);
    assert(syscall_drm_readv(task, file, &vector, 0, true, UINT64_MAX, 0) ==
           -RELIEFOS_EINVAL);
    assert(syscall_drm_readv(task, file, &vector, 0, false, UINT64_MAX, 1) == 0);
    assert(file->offset == 0);
    assert(descriptor_drm_read_count == 8);

    file->offset = 37;
    for (uint64_t whence = 0; whence <= 4; ++whence)
        assert(syscall_drm_llseek(file, whence) == 37);
    assert(syscall_drm_llseek(file, 5) == -RELIEFOS_EINVAL);
    assert(syscall_drm_llseek(file, UINT32_MAX) == -RELIEFOS_EINVAL);
    assert(file->offset == 37);
    task_discard_file_fd(task, fd);
    descriptor_dispatch_task = NULL;
    free(task);
    puts("PASS production syscall frame returns nonblocking DRM read/readv EAGAIN without retry sleep; positional DRM EAGAIN classification is scoped by syscall number");
}

/* mode 0 imports then dup/closes; modes 1/2 truncate by control capacity/fd
 * limit; mode 3 exits with queued rights. Each checks the actual last release. */
static void test_audio_scm_rights(int control, unsigned mode)
{
    struct audio_test_card card = {0};
    if (control) {
        struct audio_card_ops ops = audio_test_ops;
        ops.control_count = descriptor_control_count;
        ops.control_info = descriptor_control_info;
        ops.control_read = descriptor_control_read;
        ops.control_write = descriptor_control_write;
        struct audio_card_identity identity = {.id = "scm", .name = "scm-test"};
        assert(audio_register_card_owned(&identity, &ops, &card, 0x7b, &card.id) == 0);
    } else audio_test_card_init(&card);
    struct task *sender = descriptor_audio_task(1200), *receiver = descriptor_audio_task(1201);
    struct task *observer = control ? descriptor_audio_task(1202) : NULL;
    int audio_fd = descriptor_audio_open(sender, card.id, control);
    struct task_file *independent = NULL;
    struct snd_ctl_elem_id id = {.numid = 1, .iface = SNDRV_CTL_ELEM_IFACE_MIXER};
    struct snd_ctl_elem_value value = {.id = id, .value.integer.value = {30}};
    if (control) {
        independent = task_file_for_fd(observer, descriptor_audio_open(observer, card.id, 1));
        assert(audio_control_ioctl(sender, task_file_for_fd(sender, audio_fd),
                                    SNDRV_CTL_IOCTL_ELEM_LOCK, (uintptr_t)&id) == 0);
        int subscribe = 1;
        assert(audio_alsa_control_ioctl(task_file_for_fd(sender, audio_fd)->audio_control_file,
            SNDRV_CTL_IOCTL_SUBSCRIBE_EVENTS, &subscribe) == 0);
    }
    int send_fd, receive_fd;
    descriptor_socket_pair(sender, receiver, &send_fd, &receive_fd);
    union { struct cmsghdr alignment; uint8_t bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct cmsghdr *header = (struct cmsghdr *)ancillary.bytes;
    *header = (struct cmsghdr){.cmsg_len = CMSG_LEN(sizeof(int)),
        .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS};
    memcpy(ancillary.bytes + CMSG_LEN(0), &audio_fd, sizeof(audio_fd));
    uint8_t byte = 0x65;
    struct iovec vector = {.iov_base = &byte, .iov_len = 1};
    struct msghdr message = {.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    struct socket_message_result result = {0};
    descriptor_socket_task = sender;
    fail_allocation = 1;
    assert(task_socket_message(sender, task_file_for_fd(sender, send_fd), (uintptr_t)&message,
        MSG_DONTWAIT | MSG_NOSIGNAL, false, false, &result) == -ENOMEM);
    fail_allocation = 0;
    assert(!task_file_for_fd(sender, audio_fd)->description);
    assert(task_socket_message(sender, task_file_for_fd(sender, send_fd), (uintptr_t)&message,
        MSG_DONTWAIT | MSG_NOSIGNAL, false, false, &result) == 1);
    struct task_file *held = task_file_for_fd(sender, audio_fd);
    assert(held->references == 2 && held->scm_references == 1);
    clear_task_file(task_descriptor_for_fd(sender, audio_fd));
    assert(held->references == 1 && held->scm_references == 1);
    if (control) {
        assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
        audio_control_changed(card.id, 0);
    } else assert(card.close_count == 0 && audio_unregister_card(card.id, 0) == -EBUSY);
    descriptor_socket_task = receiver;
    if (mode == 3) {
        clear_task_files(receiver);
    } else {
        memset(&ancillary, 0, sizeof(ancillary));
        byte = 0;
        message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1,
            .msg_control = ancillary.bytes, .msg_controllen = mode == 1 ? 0 : sizeof(ancillary.bytes)};
        if (mode == 2) receiver->limits.nofile.rlim_cur = 4;
        assert(task_socket_message(receiver, task_file_for_fd(receiver, receive_fd), (uintptr_t)&message,
            MSG_DONTWAIT | MSG_CMSG_CLOEXEC, true, false, &result) == 1);
        assert(byte == 0x65);
        if (mode) {
            assert(message.msg_flags & MSG_CTRUNC);
            assert(message.msg_controllen == 0 && !task_file_for_fd(receiver, 4));
        } else {
            assert(!(message.msg_flags & MSG_CTRUNC));
            assert(header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
            int received;
            memcpy(&received, ancillary.bytes + CMSG_LEN(0), sizeof(received));
            assert(received == 4 && task_file_for_fd(receiver, received) == held);
            assert(held->references == 1 && !held->scm_references);
            assert(task_fd_descriptor_flags(receiver, received) == RELIEFOS_FD_CLOEXEC);
            assert(task_duplicate_file_fd(receiver, received, 0, 0) == 5);
            if (control) {
                struct snd_ctl_event event;
                assert(audio_control_read_file(held->audio_control_file, &event, sizeof(event)) == sizeof(event));
                assert(event.data.elem.id.numid == 1 && event.data.elem.mask == 1);
                assert(audio_alsa_control_ioctl(held->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
            } else {
                uint8_t samples[4] = {0};
                assert(audio_device_write(NULL, held, samples, sizeof(samples)) == sizeof(samples));
            }
            clear_task_file(task_descriptor_for_fd(receiver, received));
            assert(held->references == 1);
            if (control)
                assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == -EPERM);
            else assert(card.close_count == 0);
            clear_task_file(task_descriptor_for_fd(receiver, 5));
        }
    }
    if (control) {
        assert(audio_alsa_control_ioctl(independent->audio_control_file, SNDRV_CTL_IOCTL_ELEM_WRITE, &value) == 0);
        clear_task_files(observer); sched_task_file_release(observer); free(observer);
    } else assert(card.close_count == 1);
    clear_task_files(sender); clear_task_files(receiver);
    sched_task_file_release(sender); sched_task_file_release(receiver);
    free(sender); free(receiver);
    descriptor_socket_task = NULL;
    assert(audio_unregister_card(card.id, 0) == 0);
    assert(!unix_sockets);
    printf("PASS actual SCM_RIGHTS %s: %s\n", control ? "control" : "PCM",
        mode == 0 ? "queue survives sender close, CLOEXEC import, shared state, final dup close" :
        mode == 1 ? "missing control buffer discards queued reference" :
        mode == 2 ? "RLIMIT import rollback discards queued reference" : "receiver exit drains queued reference");
}

static void test_flock_promotion(void)
{
    for (unsigned type = RELIEFOS_FLOCK_SH; type <= RELIEFOS_FLOCK_EX; ++type) {
        struct task_file source = {.used = 1, .flock_type = type};
        struct task_file duplicate = {0};
        struct reliefos_flock_entry *entry = &reliefos_flocks[0];
        *entry = (struct reliefos_flock_entry){.used = 1, .type = type};
        kernel_wait_queue_init(&entry->waiters);
        if (type == RELIEFOS_FLOCK_EX) entry->owner = &source;
        else { entry->shared_count = 1; entry->shared_owners[0] = &source; }
        fail_allocation = 1;
        assert(task_file_reference(&duplicate, &source) == -RELIEFOS_ENOMEM);
        assert(!source.description && !duplicate.used);
        assert((type == RELIEFOS_FLOCK_EX ? entry->owner : entry->shared_owners[0]) == &source);
        fail_allocation = 0;
        assert(task_file_reference(&duplicate, &source) == 0);
        assert((type == RELIEFOS_FLOCK_EX ? entry->owner : entry->shared_owners[0]) == source.description);
        clear_task_file(&source);
        assert(entry->used);
        clear_task_file(&duplicate);
        assert(!entry->used);
    }
    puts("PASS flock owner promotion: exclusive/shared, ENOMEM rollback and final close");
}

static void test_pty_descriptions(void)
{
    struct task *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
    assert(parent && child);
    parent->limits.nofile.rlim_cur = child->limits.nofile.rlim_cur = 1024;
    fail_allocation = 1;
    assert(task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR) == -RELIEFOS_ENOMEM);
    assert(!pty_references && !task_pty_fd_for_fd(parent, 3));
    fail_allocation = 0;
    int fd = task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR | RELIEFOS_O_CLOEXEC);
    assert(fd == 3 && pty_references == 1);
    struct task_pty_fd *source = task_pty_fd_for_fd(parent, fd);
    int duplicate = task_pty_duplicate_fd(parent, fd, 0, 0);
    assert(duplicate == 4 && source->description->references == 2 && pty_references == 1);
    source->description->status_flags |= RELIEFOS_O_NONBLOCK;
    assert(task_pty_status(task_pty_fd_for_fd(parent, duplicate)) & RELIEFOS_O_NONBLOCK);
    assert(task_fd_descriptor_flags(parent, fd) == RELIEFOS_FD_CLOEXEC &&
           task_fd_descriptor_flags(parent, duplicate) == 0);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(source->description->references == 4);
    struct task_pty_fd queued;
    assert(task_pty_export_fd(parent, fd, &queued) == 0 && source->description->references == 5);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(parent, duplicate));
    task_pty_release_entry(task_pty_fd_for_fd(child, fd));
    task_pty_release_entry(task_pty_fd_for_fd(child, duplicate));
    sched_task_pty_release(parent);
    sched_task_pty_release(child);
    assert(queued.description->references == 1 && pty_references == 1);
    fd = task_pty_import_fd(parent, &queued, RELIEFOS_FD_CLOEXEC);
    assert(fd == 3 && queued.description->references == 2);
    task_pty_release_entry(&queued);
    source = task_pty_fd_for_fd(parent, fd);
    parent->syscall_pty = *source;
    ++source->description->references;
    parent->syscall_fd = fd;
    assert(task_pty_dup2_fd(parent, fd, 1) == 1 && parent->pty_id == 0);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(parent, 1));
    assert(parent->syscall_pty.description->references == 1 && pty_references == 1);
    assert(task_pty_for_io(parent, fd) == &parent->syscall_pty);
    task_release_syscall_file(parent);
    assert(!pty_references && !parent->syscall_pty.used);
    fd = task_pty_endpoint_fd(parent, 7, TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR);
    assert(fd >= 0);
    struct task_fd_table_state *shared = kernel_malloc(sizeof(*shared));
    assert(shared);
    clear_task_files(child);
    sched_task_pty_release(child);
    *shared = parent->fd_table;
    shared->references = 2;
    parent->shared_files = child->shared_files = shared;
    fail_allocation = 1;
    assert(syscall_unshare_task_files(parent) == -RELIEFOS_ENOMEM);
    assert(parent->shared_files == shared && shared->references == 2);
    fail_allocation = 0;
    assert(syscall_unshare_task_files(parent) == 0);
    source = task_pty_fd_for_fd(parent, fd);
    assert(parent->shared_files != shared && shared->references == 1);
    assert(source != task_pty_fd_for_fd(child, fd) && source->description->references == 2);
    source->description->status_flags |= RELIEFOS_O_APPEND;
    assert(task_pty_status(task_pty_fd_for_fd(child, fd)) & RELIEFOS_O_APPEND);
    task_pty_release_entry(source);
    task_pty_release_entry(task_pty_fd_for_fd(child, fd));
    assert(!pty_references);
    clear_task_files(parent);
    clear_task_files(child);
    sched_task_pty_release(parent);
    sched_task_pty_release(child);
    kernel_free(parent->shared_files);
    kernel_free(shared);
    free(parent);
    free(child);
}

static void test_pty_endpoint_path_full_id_range(void)
{
    uint32_t pty_id = 0;
    assert(task_pty_endpoint_path("/dev/pts/65536", &pty_id) == 1 &&
           pty_id == 65536u);
    assert(task_pty_endpoint_path("/dev/pts/2147483647", &pty_id) == 1 &&
           pty_id == 0x7fffffffu);
    assert(task_pty_endpoint_path("/dev/pts/65537", &pty_id) == 0);
    assert(task_pty_endpoint_path("/dev/pts/2147483648", &pty_id) == 0);
    assert(task_pty_endpoint_path("/dev/pts/9999999999999999999999999",
                                  &pty_id) == 0);
    assert(task_pty_endpoint_path("/dev/pts/65536x", &pty_id) == 0);
    puts("PASS PTY endpoint path parser accepts active IDs through INT32_MAX safely");
}

static void test_pty_alias_capacity(void)
{
    struct task *owner = calloc(1, sizeof(*owner));
    struct task *child = calloc(1, sizeof(*child));
    struct task *receiver = calloc(1, sizeof(*receiver));
    assert(owner && child && receiver);
    owner->limits.nofile.rlim_cur = receiver->limits.nofile.rlim_cur = 128;
    child->limits.nofile.rlim_cur = 128;

    int descriptors[20];
    for (unsigned i = 0; i < 16; ++i) {
        descriptors[i] = task_pty_endpoint_fd(owner, 7,
            TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR);
        assert(descriptors[i] == (int)(i + 3));
        assert(task_pty_fd_for_fd(owner, descriptors[i]));
    }
    fail_allocation = 1;
    assert(task_pty_endpoint_fd(owner, 7, TASK_PTY_ENDPOINT_SLAVE,
                                RELIEFOS_O_RDWR) == -RELIEFOS_ENOMEM);
    assert(sched_task_pty_capacity(owner) == 16 && pty_references == 16);
    fail_allocation = 0;

    /* dup2 forces alias-vector growth while its source pointer is borrowed. */
    assert(task_pty_dup2_fd(owner, descriptors[0], 64) == 64);
    assert(task_pty_fd_for_fd(owner, descriptors[0])->description ==
           task_pty_fd_for_fd(owner, 64)->description);
    int plain_dup = task_pty_duplicate_fd(owner, descriptors[0], 65, 0);
    assert(plain_dup == 65);
    assert(task_pty_fd_for_fd(owner, plain_dup)->description ==
           task_pty_fd_for_fd(owner, descriptors[0])->description);
    for (unsigned i = 16; i < 20; ++i) {
        descriptors[i] = task_pty_endpoint_fd(owner, 7,
            TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR);
        assert(descriptors[i] == (int)(i + 3));
    }
    assert(sched_task_pty_capacity(owner) >= 21);

    /* The real close syscall releases a high alias and a later endpoint open
     * reuses both its descriptor number and its empty backing-vector slot. */
    descriptor_dispatch_task = owner;
    unsigned references_before_close = pty_references;
    int closed_fd = descriptors[5];
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, (uint64_t)closed_fd,
                                      0, 0, 0, 0, 0) == 0);
    assert(!task_pty_fd_for_fd(owner, closed_fd));
    assert(pty_references + 1 == references_before_close);
    descriptors[5] = task_pty_endpoint_fd(owner, 7,
        TASK_PTY_ENDPOINT_SLAVE, RELIEFOS_O_RDWR);
    assert(descriptors[5] == closed_fd && pty_references == references_before_close);
    descriptor_dispatch_task = NULL;

    /* Materialize inherited streams before forcing the child's first vector
     * allocation to fail. No parent reference may be lost or duplicated. */
    owner->pty_id = 7;
    assert(task_pty_materialize_stdio(owner) == 0);
    unsigned references_before_clone = pty_references;
    child->fd_table = owner->fd_table;
    fail_allocation = 1;
    assert(syscall_clone_task_files(owner, child) == -RELIEFOS_ENOMEM);
    fail_allocation = 0;
    assert(pty_references == references_before_clone);
    assert(child->fd_table.pty_fds == NULL && child->fd_table.pty_fd_count == 0);
    sched_task_pty_release(child);

    /* A forked descriptor table has independent vector storage and references. */
    child->fd_table = owner->fd_table;
    assert(syscall_clone_task_files(owner, child) == 0);
    assert(sched_task_pty_capacity(child) >= 21);
    assert(task_pty_fd_for_fd(child, descriptors[0])->description ==
           task_pty_fd_for_fd(owner, descriptors[0])->description);

    /* Export/import exercises the SCM_RIGHTS lifetime path while both task
     * tables grow independently. */
    struct task_pty_fd transfers[20] = {{0}};
    for (unsigned i = 0; i < 20; ++i)
        assert(task_pty_export_fd(owner, descriptors[i], &transfers[i]) == 0);
    for (unsigned i = 0; i < 20; ++i)
        assert(task_pty_import_fd(receiver, &transfers[i], 0) == (int)(i + 3));
    assert(sched_task_pty_capacity(receiver) >= 20);
    /* New aliases still obey RLIMIT_NOFILE even after the backing vector has
     * grown beyond its initial storage. */
    owner->limits.nofile.rlim_cur = 65;
    assert(task_pty_duplicate_fd(owner, descriptors[0], 65, 0) ==
           -RELIEFOS_EINVAL);
    assert(task_pty_dup2_fd(owner, descriptors[0], 65) == -RELIEFOS_EBADF);
    owner->limits.nofile.rlim_cur = 128;

    clear_task_files(owner);
    clear_task_files(child);
    clear_task_files(receiver);
    sched_task_pty_release(owner);
    sched_task_pty_release(child);
    sched_task_pty_release(receiver);
    for (unsigned i = 0; i < 20; ++i) task_pty_release_entry(&transfers[i]);
    assert(!pty_references);
    assert(!console_references);
    free(owner); free(child); free(receiver);
    puts("PASS PTY alias vector growth, dup2, SCM_RIGHTS import/export and RLIMIT_NOFILE");
}

static void test_epoll_open_file_description_lifetime(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *target;
    struct epoll_event event;
    struct epoll_event ready[4] = {{0}};
    int64_t result;
    assert(task);
    task->limits.nofile.rlim_cur = 128;
    descriptor_dispatch_task = task;

    int original = task_allocate_fd(task, 3, &target);
    assert(original == 3);
    target->node.type = RELIEFOS_FS_TYPE_FILE;
    target->flags = RELIEFOS_O_RDWR;
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 4, &target);
    assert(epfd == 4);
    target->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    target->aux = (uint64_t)(uintptr_t)epoll;
    int duplicate = task_duplicate_file_fd(task, original, 5, 0);
    assert(duplicate == 5 && task_file_for_fd(task, duplicate) == task_file_for_fd(task, original));

    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x11)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, original,
                             (uint64_t)(uintptr_t)&event) == 0);
    event.data = UINT64_C(0x22);
    /* Linux keys registrations by descriptor number and open-file description. */
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, duplicate,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 2);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, original, 0, 0, 0, 0, 0) == 0);
    /* The first registration must keep probing the retained OFD through dup. */
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    if (result != 2) {
        fprintf(stderr, "FAIL: epoll lost OFD watch after closing original fd: %lld events\n",
                (long long)result);
        abort();
    }

    int reused = task_allocate_fd(task, original, &target);
    assert(reused == original && target);
    target->node.type = RELIEFOS_FS_TYPE_FILE;
    target->flags = RELIEFOS_O_RDWR;
    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x33)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, reused,
                             (uint64_t)(uintptr_t)&event) == 0);
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    assert(result == 3);
    uint32_t payloads = 0;
    for (int64_t i = 0; i < result; ++i) {
        if (ready[i].data == UINT64_C(0x11)) payloads |= 1u;
        if (ready[i].data == UINT64_C(0x22)) payloads |= 2u;
        if (ready[i].data == UINT64_C(0x33)) payloads |= 4u;
    }
    assert(payloads == 7u);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, duplicate, 0, 0, 0, 0, 0) == 0);
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    assert(result == 1 && ready[0].data == UINT64_C(0x33));
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_DEL, reused, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, reused, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll OFD lifetime across dup, close, final release, and fd reuse");
}

static void test_epoll_eventfd_dup_readiness(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *file;
    struct epoll_event event;
    struct epoll_event ready[4] = {{0}};
    uint64_t increment = 1;
    assert(task);
    task->limits.nofile.rlim_cur = 128;
    descriptor_dispatch_task = task;

    int original = task_allocate_fd(task, 0, &file);
    assert(original == 3);
    file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    int duplicate = task_duplicate_file_fd(task, original, 0, 0);
    assert(duplicate == 4);
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 5, &file);
    assert(epfd == 5);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;

    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x11)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, original,
                             (uint64_t)(uintptr_t)&event) == 0);
    event.data = UINT64_C(0x22);
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, duplicate,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, original, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    int reused = task_allocate_fd(task, original, &file);
    assert(reused == original);
    file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x33)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, reused,
                             (uint64_t)(uintptr_t)&event) == 0);

    assert(syscall_io_write(duplicate, (uint64_t)(uintptr_t)&increment,
                            sizeof(increment)) == (int64_t)sizeof(increment));
    int64_t count = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    if (count != 2) {
        fprintf(stderr, "FAIL: readiness on surviving dup returned %lld epoll events\n",
                (long long)count);
        abort();
    }
    uint32_t payloads = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (ready[i].data == UINT64_C(0x11)) payloads |= 1u;
        if (ready[i].data == UINT64_C(0x22)) payloads |= 2u;
    }
    assert(payloads == 3u);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, duplicate, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    assert(syscall_io_write(reused, (uint64_t)(uintptr_t)&increment,
                            sizeof(increment)) == (int64_t)sizeof(increment));
    count = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    assert(count == 1 && ready[0].data == UINT64_C(0x33));
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, reused, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll readiness follows a dup OFD across original close and fd reuse");
}

static void test_epoll_clone_description_lifetime(void)
{
    struct task *parent = descriptor_audio_task(1500);
    struct task *child = descriptor_audio_task(1501);
    struct task_file *file;
    struct epoll_event event = {.events = EPOLLIN, .data = UINT64_C(0x41)};
    struct epoll_event ready[2] = {{0}};
    uint64_t increment = 1;
    descriptor_dispatch_task = parent;

    int watched = task_allocate_fd(parent, 0, &file);
    assert(watched == 3);
    file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(parent, 4, &file);
    assert(epfd == 4);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, watched,
                             (uint64_t)(uintptr_t)&event) == 0);

    /* Match clone.c's private-table copy before it retains descriptor refs. */
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(task_file_for_fd(parent, watched)->description ==
           task_file_for_fd(child, watched)->description);
    assert(task_file_for_fd(parent, epfd)->description ==
           task_file_for_fd(child, epfd)->description);

    descriptor_dispatch_task = parent;
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, watched, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = child;
    assert(syscall_io_write(watched, (uint64_t)(uintptr_t)&increment,
                            sizeof(increment)) == (int64_t)sizeof(increment));
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 1);
    assert(ready[0].data == UINT64_C(0x41));

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, watched, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);
    descriptor_dispatch_task = parent;
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = child;
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);

    descriptor_dispatch_task = NULL;
    syscall_release_task_files(parent);
    syscall_release_task_files(child);
    free(parent);
    free(child);
    puts("PASS epoll weak OFD watch survives private fork-table references");
}

static void test_epoll_pty_description_lifetime(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *file;
    struct epoll_event event, ready[4] = {{0}};
    assert(task);
    task->limits.nofile.rlim_cur = 128;
    descriptor_dispatch_task = task;
    descriptor_pty_input_available = 0;

    int original = task_pty_endpoint_fd(task, 7, TASK_PTY_ENDPOINT_SLAVE,
                                        RELIEFOS_O_RDWR);
    assert(original == 3);
    int duplicate = task_pty_duplicate_fd(task, original, 0, 0);
    assert(duplicate == 4);
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 5, &file);
    assert(epfd == 5);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;

    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x81)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, original,
                             (uint64_t)(uintptr_t)&event) == 0);
    event.data = UINT64_C(0x82);
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, duplicate,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);

    descriptor_pty_input_available = 1;
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, original, 0, 0, 0, 0, 0) == 0);
    int64_t count = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    if (count != 2) {
        fprintf(stderr, "FAIL: epoll lost PTY OFD watches after closing original alias: %lld events\n",
                (long long)count);
        abort();
    }
    uint32_t payloads = 0;
    for (int64_t i = 0; i < count; ++i) {
        if (ready[i].data == UINT64_C(0x81)) payloads |= 1u;
        if (ready[i].data == UINT64_C(0x82)) payloads |= 2u;
    }
    assert(payloads == 3u);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, duplicate, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    int reused = task_pty_endpoint_fd(task, 7, TASK_PTY_ENDPOINT_SLAVE,
                                      RELIEFOS_O_RDWR);
    assert(reused == original);
    event.data = UINT64_C(0x83);
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, reused,
                             (uint64_t)(uintptr_t)&event) == 0);
    count = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0);
    assert(count == 1 && ready[0].data == UINT64_C(0x83));
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, reused, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 4, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_pty_input_available = 0;
    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll PTY OFD watch survives dup close and detaches on final release/reuse");
}

static void test_epoll_zero_mask_hup_and_oneshot_rearm(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *file;
    struct epoll_event event = {.data = UINT64_C(0x91)};
    struct epoll_event ready[2] = {{0}};
    int sockets[2] = {-1, -1};
    assert(task);
    task->limits.nofile.rlim_cur = 128;
    descriptor_dispatch_task = task;
    descriptor_socket_task = task;
    int64_t result = syscall_socket_dispatch(__NR_socketpair, AF_UNIX,
        SOCK_STREAM, 0, (uint64_t)(uintptr_t)sockets, 0, 0);
    assert(result == 0 && sockets[0] == 3 && sockets[1] == 4);

    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 5, &file);
    assert(epfd == 5);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, sockets[0],
                             (uint64_t)(uintptr_t)&event) == 0);

    /* Closing the peer publishes unconditional HUP through production poll. */
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, sockets[1], 0, 0, 0, 0, 0) == 0);
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0);
    if (result != 1 || ready[0].events != EPOLLHUP ||
        ready[0].data != UINT64_C(0x91)) {
        fprintf(stderr,
                "FAIL: zero-mask epoll watch missed peer-close HUP (events=%lld mask=0x%x)\n",
                (long long)result, result > 0 ? ready[0].events : 0);
        abort();
    }
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_DEL, sockets[0], 0) == 0);

    event.events = EPOLLONESHOT;
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, sockets[0],
                             (uint64_t)(uintptr_t)&event) == 0);
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0);
    assert(result == 1 && ready[0].events == EPOLLHUP &&
           ready[0].data == UINT64_C(0x91));
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);

    /* MOD with no readiness bits is still a rearm for unconditional HUP. */
    event.events = 0;
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_MOD, sockets[0],
                             (uint64_t)(uintptr_t)&event) == 0);
    result = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0);
    assert(result == 1 && ready[0].events == EPOLLHUP &&
           ready[0].data == UINT64_C(0x91));

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, sockets[0], 0, 0, 0, 0, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_socket_task = NULL;
    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll zero-mask HUP and oneshot MOD rearm");
}

static void test_epoll_scm_rights_watch_lifetime(void)
{
    struct task *sender = descriptor_audio_task(1400);
    struct task *receiver = descriptor_audio_task(1401);
    struct task_file *event_file, *epoll_file;
    struct epoll_event event, ready[2] = {{0}};
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    struct socket_message_result result = {0};
    union { struct cmsghdr alignment; uint8_t bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct cmsghdr *header = (struct cmsghdr *)ancillary.bytes;
    uint8_t byte = 0x7a;
    uint64_t increment = 1;
    struct iovec vector = {.iov_base = &byte, .iov_len = sizeof(byte)};
    struct msghdr message;
    int send_fd, receive_fd;
    assert(sender && receiver && epoll);
    descriptor_dispatch_task = sender;

    int watched = task_allocate_fd(sender, 0, &event_file);
    assert(watched == 3);
    event_file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    descriptor_socket_pair(sender, receiver, &send_fd, &receive_fd);
    int epfd = task_allocate_fd(sender, 5, &epoll_file);
    assert(send_fd == 4 && receive_fd == 3 && epfd == 5);
    epoll_file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    epoll_file->aux = (uint64_t)(uintptr_t)epoll;

    event = (struct epoll_event){.events = EPOLLIN, .data = UINT64_C(0x51)};
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, watched,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);

    *header = (struct cmsghdr){.cmsg_len = CMSG_LEN(sizeof(int)),
        .cmsg_level = SOL_SOCKET, .cmsg_type = SCM_RIGHTS};
    memcpy(ancillary.bytes + CMSG_LEN(0), &watched, sizeof(watched));
    message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    descriptor_dispatch_task = sender;
    descriptor_socket_task = sender;
    assert(task_socket_message(sender, task_file_for_fd(sender, send_fd),
        (uintptr_t)&message, MSG_DONTWAIT | MSG_NOSIGNAL, false, false, &result) == 1);
    struct task_file *canonical = task_file_for_fd(sender, watched);
    assert(canonical && canonical->scm_references == 1);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, watched, 0, 0, 0, 0, 0) == 0);
    assert(!task_file_for_fd(sender, watched));

    memset(&ancillary, 0, sizeof(ancillary));
    byte = 0;
    message = (struct msghdr){.msg_iov = &vector, .msg_iovlen = 1,
        .msg_control = ancillary.bytes, .msg_controllen = sizeof(ancillary.bytes)};
    descriptor_dispatch_task = receiver;
    descriptor_socket_task = receiver;
    assert(task_socket_message(receiver, task_file_for_fd(receiver, receive_fd),
        (uintptr_t)&message, MSG_DONTWAIT, true, false, &result) == 1);
    int received = -1;
    memcpy(&received, ancillary.bytes + CMSG_LEN(0), sizeof(received));
    assert(received == 4 && task_file_for_fd(receiver, received) == canonical);
    assert(syscall_io_write(received, (uint64_t)(uintptr_t)&increment,
                            sizeof(increment)) == (int64_t)sizeof(increment));

    descriptor_dispatch_task = sender;
    int64_t count = syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0);
    if (count != 1 || ready[0].data != UINT64_C(0x51)) {
        fprintf(stderr, "FAIL: epoll watch did not survive queued SCM_RIGHTS transfer (events=%lld)\n",
                (long long)count);
        abort();
    }
    descriptor_dispatch_task = receiver;
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, received, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = sender;
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);

    descriptor_dispatch_task = NULL;
    descriptor_socket_task = NULL;
    clear_task_files(sender);
    clear_task_files(receiver);
    sched_task_file_release(sender);
    sched_task_file_release(receiver);
    free(sender);
    free(receiver);
    assert(!unix_sockets);
    puts("PASS epoll weak OFD watch follows queued and imported SCM_RIGHTS lifetime");
}

static void test_epoll_fd_poll_readiness(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *file;
    struct epoll_event event = {
        .events = EPOLLIN | EPOLLET | EPOLLONESHOT,
        .data = UINT64_C(0x7a),
    };
    struct epoll_event ready[2] = {{0}};
    struct pollfd descriptor = {.events = POLLIN};
    uint64_t increment = 1;
    assert(task);
    task->limits.nofile.rlim_cur = 64;
    descriptor_dispatch_task = task;

    int watched = task_allocate_fd(task, 0, &file);
    assert(watched == 3);
    file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 4, &file);
    assert(epfd == 4);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;

    descriptor.fd = epfd;
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 0);
    assert(descriptor.revents == 0);
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, watched,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 0);
    assert(descriptor.revents == 0);

    assert(syscall_io_write(watched, (uint64_t)(uintptr_t)&increment,
                            sizeof(increment)) == (int64_t)sizeof(increment));
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 1);
    assert(descriptor.revents & POLLIN);
    /* Polling an epoll fd is a peek: it cannot consume ET or ONESHOT state. */
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 1);
    assert(descriptor.revents & POLLIN);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 1);
    assert(ready[0].data == UINT64_C(0x7a));
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);

    /* MOD rearms the still-readable target; a poll peek preserves that event. */
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_MOD, watched,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(syscall_poll((uint64_t)(uintptr_t)&descriptor, 1, 0) == 1);
    assert(descriptor.revents & POLLIN);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 1);

    struct task_epoll *outer = calloc(1, sizeof(*outer));
    assert(outer);
    int outerfd = task_allocate_fd(task, 5, &file);
    assert(outerfd == 5);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)outer;
    event.events = EPOLLIN;
    assert(syscall_epoll_ctl(outerfd, EPOLL_CTL_ADD, epfd,
                             (uint64_t)(uintptr_t)&event) == -RELIEFOS_EPERM);
    assert(outer->entries == 0 && !task_file_for_fd(task, epfd)->epoll_watches);

    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, watched, 0, 0, 0, 0, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, outerfd, 0, 0, 0, 0, 0) == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, epfd, 0, 0, 0, 0, 0) == 0);
    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll fd poll peeks armed readiness and rejects nested epoll targets");
}

static void test_epoll_inflight_and_nested_rejection(void)
{
    struct task *task = calloc(1, sizeof(*task));
    struct task_file *file;
    struct epoll_event event = {.events = EPOLLIN, .data = UINT64_C(0x61)};
    struct epoll_event ready[2] = {{0}};
    assert(task);
    task->limits.nofile.rlim_cur = 64;
    descriptor_dispatch_task = task;

    int watched = task_allocate_fd(task, 0, &file);
    assert(watched == 3);
    file->flags = TASK_FILE_FLAG_EVENTFD | RELIEFOS_O_RDWR;
    file->aux = 1;
    struct task_epoll *epoll = calloc(1, sizeof(*epoll));
    assert(epoll);
    int epfd = task_allocate_fd(task, 4, &file);
    assert(epfd == 4);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)epoll;
    assert(syscall_epoll_ctl(epfd, EPOLL_CTL_ADD, watched,
                             (uint64_t)(uintptr_t)&event) == 0);
    assert(task_begin_syscall_io(task, LINUX_SYS_READ, watched) == 0);
    assert(task->syscall_file == task_file_for_fd(task, watched));
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, watched, 0, 0, 0, 0, 0) == 0);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 1);
    task_release_syscall_file(task);
    assert(syscall_epoll_wait(epfd, (uint64_t)(uintptr_t)ready, 2, 0) == 0);

    int first = task_allocate_fd(task, 3, &file);
    assert(first == 3);
    struct task_epoll *first_state = calloc(1, sizeof(*first_state));
    assert(first_state);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)first_state;
    int second = task_allocate_fd(task, 5, &file);
    assert(second == 5);
    struct task_epoll *second_state = calloc(1, sizeof(*second_state));
    assert(second_state);
    file->flags = TASK_FILE_FLAG_EPOLL | RELIEFOS_O_RDWR;
    file->aux = (uint64_t)(uintptr_t)second_state;
    event.data = UINT64_C(0x71);
    assert(syscall_epoll_ctl(first, EPOLL_CTL_ADD, second,
                             (uint64_t)(uintptr_t)&event) == -RELIEFOS_EPERM);
    event.data = UINT64_C(0x72);
    assert(syscall_epoll_ctl(second, EPOLL_CTL_ADD, first,
                             (uint64_t)(uintptr_t)&event) == -RELIEFOS_EPERM);
    struct task_file *first_description = task_file_for_fd(task, first);
    struct task_file *second_description = task_file_for_fd(task, second);
    assert(first_description && second_description);
    assert(!first_description->epoll_watches && !second_description->epoll_watches);
    assert(first_state->entries == 0 && second_state->entries == 0);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, first, 0, 0, 0, 0, 0) == 0);
    assert(second_state->entries == 0 && !second_description->epoll_watches);
    assert(syscall_legacy_descriptors(LINUX_SYS_CLOSE, second, 0, 0, 0, 0, 0) == 0);

    descriptor_dispatch_task = NULL;
    syscall_release_task_files(task);
    free(task);
    puts("PASS epoll watches retain in-flight OFDs and reject nested registrations cleanly");
}

static void test_console_descriptions(void)
{
    struct task *parent = calloc(1, sizeof(*parent)), *child = calloc(1, sizeof(*child));
    assert(parent && child);
    parent->limits.nofile.rlim_cur = child->limits.nofile.rlim_cur = 64;
    assert(task_pty_materialize_stdio(parent) == 0);
    assert(console_references == 1);
    assert(task_file_for_fd(parent, 0) && task_file_for_fd(parent, 1) && task_file_for_fd(parent, 2));
    assert(task_device_is(task_file_for_fd(parent, 0), STORAGE_DEV_KIND_CONSOLE));
    assert(task_file_for_fd(parent, 0) == task_file_for_fd(parent, 1));
    assert(task_file_for_fd(parent, 1) == task_file_for_fd(parent, 2));
    assert((task_file_for_fd(parent, 1)->flags & RELIEFOS_O_ACCMODE) == RELIEFOS_O_RDWR);
    assert(task_file_for_fd(parent, 1)->aux == 1);
    assert(!strcmp(task_file_for_fd(parent, 1)->path, "/dev/console"));
    assert(!parent->controlling_pty_id && !parent->pty_id);
    descriptor_active_vt = 2;
    assert(task_pty_materialize_stdio(parent) == 0);
    assert(task_file_for_fd(parent, 1)->aux == 1);
    assert(task_console_materialize_stdio(child) == 0);
    assert(task_file_for_fd(child, 0)->aux == 2);
    clear_task_files(child);
    descriptor_active_vt = 1;
    assert(task_fd_set_descriptor_flags(parent, 2, RELIEFOS_FD_CLOEXEC) == 0);
    child->fd_table = parent->fd_table;
    assert(syscall_clone_task_files(parent, child) == 0);
    assert(task_file_for_fd(parent, 1) == task_file_for_fd(child, 1));
    int copy = task_duplicate_file_fd(child, 1, 0, 0);
    assert(copy == 3);
    task_file_for_fd(child, copy)->flags |= RELIEFOS_O_NONBLOCK;
    assert(task_file_for_fd(parent, 1)->flags & RELIEFOS_O_NONBLOCK);
    assert(task_file_for_fd(parent, 0)->flags & RELIEFOS_O_NONBLOCK);
    assert(task_file_for_fd(parent, 2)->flags & RELIEFOS_O_NONBLOCK);
    assert(task_fd_descriptor_flags(child, 2) == RELIEFOS_FD_CLOEXEC);
    task_discard_file_fd(child, 1);
    assert(task_pty_materialize_stdio(child) == 0 && !task_file_for_fd(child, 1));
    assert(task_file_for_fd(parent, 1));
    clear_task_files(child);
    clear_task_files(parent);
    assert(console_references == 0);
    fail_allocation = 1;
    assert(task_console_materialize_stdio(parent) == -RELIEFOS_ENOMEM);
    assert(!task_file_for_fd(parent, 0) && !task_file_for_fd(parent, 2));
    fail_allocation = 0;
    parent->closed_stdio_mask = 1u;
    parent->stdio_files[1] = (struct task_file){.used = 1,
        .flags = TASK_FILE_FLAG_DEV_NODE | RELIEFOS_O_WRONLY,
        .node = {.type = RELIEFOS_FS_TYPE_DEVICE, .flags = STORAGE_NODE_FLAG_DEV_NODE,
                 .first_cluster = STORAGE_DEV_KIND_NULL}};
    assert(task_console_materialize_stdio(parent) == 0);
    assert(!task_file_for_fd(parent, 0));
    assert(task_device_is(task_file_for_fd(parent, 1), STORAGE_DEV_KIND_NULL));
    assert(task_device_is(task_file_for_fd(parent, 2), STORAGE_DEV_KIND_CONSOLE));
    clear_task_files(parent);
    free(child);
    free(parent);
    puts("PASS boot console descriptors share one terminal, preserve redirection and survive VT switches");
}

int main(void)
{
    test_nonblocking_drm_read_dispatch();
    test_audio_write_batch();
    kernel_objects_init();
    test_unix_socket_registry_growth();
    test_unix_listener_backlog_boundaries();
    test_unix_listener_backlog_growth();
    struct audio_test_card stat_cards[2];
    audio_test_card_init(&stat_cards[0]);
    audio_test_card_init(&stat_cards[1]);
    struct storage_node sound_node = {.flags = STORAGE_NODE_FLAG_AUDIO_PCM,
        .volume_id = AUDIO_DEVICE_VOLUME_ID(stat_cards[1].id, 7, AUDIO_CAPTURE)};
    assert(linux_audio_rdev(&sound_node) == ((116u << 8) | 63u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL;
    sound_node.volume_id = stat_cards[1].id;
    assert(linux_audio_rdev(&sound_node) == ((116u << 8) | 32u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_MIXER;
    assert(linux_audio_rdev(&sound_node) == ((14u << 8) | 16u));
    sound_node.flags = STORAGE_NODE_FLAG_DEV_NODE;
    sound_node.first_cluster = STORAGE_DEV_KIND_AUDIO;
    assert(linux_audio_rdev(&sound_node) == ((14u << 8) | 3u));
    assert(!audio_unregister_card(stat_cards[0].id, 0));
    assert(!audio_unregister_card(stat_cards[1].id, 0));
    struct task_file pipe = {.flags = TASK_FILE_FLAG_PIPE};
    struct linux_stat_abi pipe_stat = {.st_mode = 0020660, .st_rdev = 1};
    linux_stat_fd_type(&pipe_stat, &pipe);
    assert(pipe_stat.st_mode == (LINUX_S_IFIFO | 0600) && pipe_stat.st_rdev == 0);
    pipe.flags |= TASK_FILE_FLAG_PIPE_WRITE;
    pipe_stat.st_mode = 0100644;
    linux_stat_fd_type(&pipe_stat, &pipe);
    assert(pipe_stat.st_mode == (LINUX_S_IFIFO | 0600));
    struct task_file device = {.flags = TASK_FILE_FLAG_DEV_NULL};
    struct linux_stat_abi device_stat = {.st_mode = 0020660, .st_rdev = 1};
    linux_stat_fd_type(&device_stat, &device);
    linux_stat_fd_type(&device_stat, NULL);
    assert(device_stat.st_mode == 0020660 && device_stat.st_rdev == 1);
    puts("PASS stat pipe endpoints report FIFO; character devices remain unchanged");
    test_flock_promotion();
    test_epoll_open_file_description_lifetime();
    test_epoll_eventfd_dup_readiness();
    test_epoll_clone_description_lifetime();
    test_epoll_pty_description_lifetime();
    test_epoll_zero_mask_hup_and_oneshot_rearm();
    test_epoll_scm_rights_watch_lifetime();
    test_epoll_fd_poll_readiness();
    test_epoll_inflight_and_nested_rejection();
    test_pty_descriptions();
    test_pty_endpoint_path_full_id_range();
    test_pty_alias_capacity();
    test_console_descriptions();
    test_audio_control_descriptions();
    test_audio_pcm_descriptions();
    test_audio_oss_descriptions();
    for (unsigned mode = 0; mode < 4; ++mode) {
        test_audio_scm_rights(1, mode);
        test_audio_scm_rights(0, mode);
    }
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    sched_task_limits(task)->nofile.rlim_cur = 1024;
    struct task_file *slot;
    assert(task_allocate_fd(task, 0, &slot) == 3);
    slot->flags = TASK_FILE_FLAG_PIPE;
    struct task_file *resolved;
    char path[RELIEFOS_FS_PATH_LEN];
    assert(resolve_kernel_path_at_flags(task, 3, "", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == 0);
    assert(resolved == slot && !path[0]);
    assert(resolve_kernel_path_at_flags(task, 3, "entry", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_ENOTDIR);
    assert(resolve_kernel_path_at_flags(task, 3, "", false, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_ENOENT);
    assert(resolve_kernel_path_at_flags(task, -1, "", true, path, &resolved, false, FS_LOOKUP_FOLLOW) == -RELIEFOS_EBADF);
    slot->flags = 0;
    puts("PASS AT_EMPTY_PATH anonymous descriptor resolution and invalid-path errors");
    slot->offset = 123;
    slot->kind = TASK_FILE_KIND_SIGNALFD;
    slot->aux = 1ULL << 35;
    assert(task_duplicate_file_fd(task, 3, 100, 1) == 100);
    assert(task_dup2_fd(task, 100, 700) == 700);
    assert(task_file_for_fd(task, 100) == task_file_for_fd(task, 700));
    assert(task_file_for_fd(task, 700)->offset == 123);
    assert(task_file_for_fd(task, 700)->kind == TASK_FILE_KIND_SIGNALFD);
    task_file_for_fd(task, 100)->aux = 1ULL << 34;
    assert(task_file_for_fd(task, 700)->aux == (1ULL << 34));
    assert(task_descriptor_for_fd(task, 100)->fd_flags == 1);
    assert(task_descriptor_for_fd(task, 700)->fd_flags == 0);
    assert(task_file_for_fd(task, 3)->references == 3);
    /* Reject failed expansion without consuming an OFD reference or fd. */
    fail_allocation = 1;
    assert(task_dup2_fd(task, 100, 900) == -RELIEFOS_ENOMEM);
    assert(task_file_for_fd(task, 900) == NULL);
    assert(task_file_for_fd(task, 3)->references == 3);
    fail_allocation = 0;
    assert(task_dup2_fd(task, 100, 900) == 900);
    assert(task_dup2_fd(task, 999, 900) == -RELIEFOS_EBADF);
    assert(task_file_for_fd(task, 3)->references == 4);
    struct task waiter = {.pid = 777, .state = TASK_BLOCKED};
    reliefos_flocks[0] = (struct reliefos_flock_entry){.used = 1,
        .type = RELIEFOS_FLOCK_EX, .owner = task_file_for_fd(task, 3)};
    kernel_wait_queue_init(&reliefos_flocks[0].waiters);
    assert(kernel_wait_queue_add(&reliefos_flocks[0].waiters, &waiter) == 0);
    sched_task_limits(task)->nofile.rlim_cur = 4;
    assert(task_allocate_fd(task, 0, &slot) == -RELIEFOS_EMFILE);
    assert(task_dup2_fd(task, 900, 900) == 900);
    task_discard_file_fd(task, 0);
    assert(task_allocate_fd(task, 0, &slot) == 0);
    task_discard_file_fd(task, 0);
    assert(task_allocate_fd(task, 0, &slot) == 0);
    task_discard_file_fd(task, 0);
    for (int fd = 3; fd < 1024; ++fd) {
        task_discard_file_fd(task, fd);
        if (fd < 900) assert(reliefos_flocks[0].used && waiter.waiting_queue);
    }
    assert(!reliefos_flocks[0].used && !waiter.waiting_queue && !reliefos_flocks[0].waiters.count);
    assert(task_allocate_fd(task, 0, &slot) == 0 && !slot->kind);
    task_discard_file_fd(task, 0);
    task->signalfd_vectors = kernel_malloc(9 * sizeof(struct iovec));
    assert(task->signalfd_vectors);
    task->signalfd_waiting = true;
    task->signalfd_wait_mask = UINT64_MAX;
    task->socket_io_deadline = 1234567;
    task->socket_io_timed = true;
    task_release_syscall_file(task);
    assert(!task->signalfd_vectors && !task->signalfd_waiting && !task->signalfd_wait_mask);
    assert(!task->socket_io_deadline && !task->socket_io_timed);
    puts("PASS syscall-file cleanup clears a completed socket timeout deadline");
    sched_task_file_release(task);
    free(task);
    puts("PASS actual fd table: growth, shared OFD, failure rollback, stdio reuse, limit lowering");
}
