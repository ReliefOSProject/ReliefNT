#include "block_description_fixture.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sound/asound.h>
#include <linux/soundcard.h>
#include <linux/poll.h>
#include <reliefnt/sched.h>
#include <reliefnt/wait.h>
#include "../../fs/fs_internal.h"
#include <reliefnt/devfs.h>
/** @brief Keep device discovery outside this descriptor-table fixture.
 * @param type Requested native character/block type.
 * @param device Unmodelled Linux device number. @param out Unused output.
 * @return ENXIO. Actual discovery/rdev/open are covered by devfs_rdev and the
 * owned native initramfs probe; this scalar descriptor fixture has no devices.
 */
int devfs_lookup_rdev(uint32_t type,uint64_t device,struct storage_node *out)
{assert(type==LINUX_S_IFCHR || type==LINUX_S_IFBLK);(void)device;(void)out;return -6;}
static struct storage_inode_ref descriptor_inode;
static unsigned descriptor_truncates;
static int descriptor_truncate_error,descriptor_inode_get_error;

static struct task *descriptor_dispatch_task;
static unsigned transfer_pipe_mode,transfer_pipe_calls,transfer_pipe_bytes;
static unsigned char transfer_pipe_data[16];
static struct kernel_wait_queue transfer_pipe_waiters={.lock=KERNEL_SPINLOCK_INIT};
static unsigned descriptor_dispatch_sleep_count;
static unsigned descriptor_module_retry_stage;
static struct task_file *descriptor_module_source;
static unsigned descriptor_mmap_retry_stage;
static struct task_file *descriptor_mmap_source;
static bool descriptor_dirent_io_pending;
static int descriptor_module_prepare_error;
static bool descriptor_module_check_selection;
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

/* Real DRM operation-table dispatch below; hardware/backend state is modelled. */
void drm_file_close(struct task_file *file)
{ if (file) file->drm_file = NULL; }
void drm_prime_file_close(struct task_file *file) { (void)file; }
/* This fixture models only file operations, not device discovery/publication.
 * Real service admission/DRM publication is linked by drm_core_test and covered
 * by the native DRM coordinator; no broker implementation is stubbed here. */
int drm_core_services_start(void) { return 0; }
void drm_core_services_stop(void) {}

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
/* Permission/descriptor preflight is tested with the real module syscalls
 * separately; this frame fixture injects its result before any OFD promotion. */
int syscall_module_file_prepare(uint64_t fd,uint64_t flags)
{
    (void)flags;
    if(descriptor_module_check_selection) {
        struct task_file *file=task_file_for_io(descriptor_dispatch_task,(int)fd);
        return file && file->node.first_cluster==0x5678 ? 0 : -RELIEFOS_EBADF;
    }
    return descriptor_module_prepare_error;
}

static int64_t descriptor_dispatch_regs(struct task *task, uint64_t number,
                                        uint64_t a0, uint64_t a1, uint64_t a2,
                                        uint64_t a3, uint64_t a4, uint64_t a5)
{
    (void)a3; (void)a4; (void)a5;
    if(number==LINUX_SYS_MMAP) {
        assert(task==descriptor_dispatch_task);
        if((uint32_t)a3&LINUX_MAP_ANONYMOUS) {
            assert(!task->syscall_file && !task->syscall_pty.used);return (int64_t)a0;
        }
        struct task_file *source=task_file_for_io(task,(int32_t)a4);
        assert(source==descriptor_mmap_source && task->syscall_file==source);
        assert(source->node.first_cluster==0x1234);
        if(descriptor_mmap_retry_stage++==1) {
            task_discard_file_fd(task,(int32_t)a4);struct task_file *replacement=NULL;
            assert(task_allocate_fd(task,(int32_t)a4,&replacement)==(int32_t)a4);
            replacement->flags=TASK_FILE_FLAG_DEV_NULL;replacement->node.first_cluster=0x5678;
            return -EAGAIN;
        }
        assert(task_file_for_fd(task,(int32_t)a4)->node.first_cluster==0x5678);return (int64_t)a0;
    }
    if (number == LINUX_SYS_DELETE_MODULE) {
        assert(task == descriptor_dispatch_task);
        assert(!strcmp((const char *)(uintptr_t)a0, "held_provider"));
        assert(a1 == LINUX_O_NONBLOCK);
        return -RELIEFOS_EWOULDBLOCK;
    }
    if(number==LINUX_SYS_FINIT_MODULE && descriptor_module_retry_stage) {
        struct task_file *source=task_file_for_io(task,(int)a0);
        assert(source==descriptor_module_source && task->syscall_file==source);
        assert(source->node.first_cluster==0x1234);
        if(descriptor_module_retry_stage++==1) {
            task_discard_file_fd(task,(int)a0);
            struct task_file *replacement;
            assert(task_allocate_fd(task,(int)a0,&replacement)==(int)a0);
            replacement->flags=TASK_FILE_FLAG_DEV_NULL;
            replacement->node.first_cluster=0x5678;
            return MODULE_FILE_IO_RETRY;
        }
        assert(task_file_for_fd(task,(int)a0)->node.first_cluster==0x5678);
        return -RELIEFOS_EAGAIN;
    }
    if (number == LINUX_SYS_INIT_MODULE || number == LINUX_SYS_FINIT_MODULE)
        return -RELIEFOS_EAGAIN; /* The initializer's final Linux errno. */
    assert((number == LINUX_SYS_READ || number == LINUX_SYS_READV) &&
           task == descriptor_dispatch_task);
    struct task_file *file = task_file_for_io(task, (int)a0);
    assert(file && file->drm_file);
    if (number == LINUX_SYS_READV) {
        assert(a2 == 1);
        return syscall_vector_io(task,number,a0,a1,a2,a3,a4,a5);
    }
    return syscall_io_read(a0,a1,a2);
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

void usb_process_work(void) { }
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

struct descriptor_heap_record {struct descriptor_heap_record *next;void *memory;size_t bytes;};
static struct descriptor_heap_record *descriptor_heap_records;
/** @brief Translate only real fixture-owned heap allocations to modeled PFNs.
 * @param address Borrowed allocation alias, including unpublished PCM pages.
 * @return Modeled physical address or zero for unrelated/static/module data.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value=(uintptr_t)address;
    for(struct descriptor_heap_record *r=descriptor_heap_records;r;r=r->next) {
        uintptr_t base=(uintptr_t)r->memory;
        if(value>=base && value-base<r->bytes)return (uint64_t)value;
    }
    return 0;
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
void *kernel_malloc(size_t size) {
    if(fail_allocation)return NULL;
    void *memory=malloc(size);if(!memory)return NULL;
    struct descriptor_heap_record *record=malloc(sizeof(*record));assert(record);
    *record=(struct descriptor_heap_record){descriptor_heap_records,memory,size};descriptor_heap_records=record;return memory;
}
void kernel_free(void *memory) {
    if(!memory)return;
    struct descriptor_heap_record **at=&descriptor_heap_records;
    while(*at && (*at)->memory!=memory)at=&(*at)->next;
    /* Older ownership-transfer cases seed an OFD with libc-allocated memory;
     * those owners can be freed, but are never accepted as DMA backing. */
    if(*at){struct descriptor_heap_record *old=*at;*at=old->next;free(old);}
    free(memory);
}
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
/* These newly linked open/stat branches are forbidden in this regular-file
 * provider fixture. Real DAC is linked; unrelated hardware/PTY/shmem entry
 * points fail loudly instead of standing in for regular namespace behavior. */
uint64_t storage_volume_device(uint32_t volume){assert(!volume);return 1;}
int storage_inode_stat(const struct storage_node *node,struct linux_stat_abi *out){(void)node;(void)out;abort();}
int pty_get_node(uint32_t id,struct storage_node *node){(void)id;(void)node;abort();}
int pty_inode_permissions(const struct storage_node *node,struct reliefos_permissions *value,bool write)
{(void)node;(void)value;(void)write;abort();}
int32_t pty_create(uint32_t owner){(void)owner;abort();}
int pty_destroy(uint32_t owner,uint32_t id){(void)owner;(void)id;abort();}
void pty_open_controlling(uint32_t id,uint32_t caller,int readable){(void)id;(void)caller;(void)readable;abort();}
int storage_disk_block_busy(uint32_t disk,int32_t partition){(void)disk;(void)partition;abort();}
int drm_file_open(struct task *task,struct task_file *file)
{assert(task && file);file->drm_file=(struct drm_file *)(uintptr_t)1;return 0;}
int64_t drm_fd_ioctl(struct task *task,struct task_file *file,uint32_t request,uint64_t argument)
{(void)task;(void)file;(void)request;(void)argument;abort();}
int drm_character_mmap(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                        uint32_t prot,uint64_t flags,struct device_mapping **out)
{(void)task;(void)file;(void)offset;(void)length;(void)prot;(void)flags;(void)out;abort();}
int64_t drm_prime_file_llseek(struct task_file *file,int64_t offset,uint32_t whence)
{(void)file;(void)offset;(void)whence;abort();}

int task_shm_attach(struct task_file *file,uint32_t flags){(void)file;(void)flags;abort();}
int task_shm_reopen(struct task_file *file,const struct vfs_path *path){(void)file;(void)path;abort();}
uint64_t input_evdev_cursor_now(void){abort();}
/* The inode/backend boundary for the regular-OFD writer lifetime regression. */
int storage_inode_get(const struct storage_node *node,struct storage_inode_ref **out)
{
    *out=NULL;
    if(node->flags&STORAGE_NODE_FLAG_TMPFS) {
        if(descriptor_inode_get_error)return descriptor_inode_get_error;
        assert(node->first_cluster==descriptor_inode.node.first_cluster);
        *out=&descriptor_inode;++descriptor_inode.references;
    }
    return 0;
}
int storage_inode_put(struct storage_inode_ref *inode)
{if(inode){assert(inode==&descriptor_inode && inode->references>1);--inode->references;}return 0;}
int storage_truncate_held_node(struct storage_node *node,uint64_t size)
{
    assert(node->first_cluster==descriptor_inode.node.first_cluster);++descriptor_truncates;
    if(descriptor_truncate_error)return descriptor_truncate_error;
    node->size=size;return 0;
}
int storage_disk_block_write(uint32_t disk_id, int32_t partition_index,
                             uint64_t offset, const void *buffer, uint32_t length,
                             uint32_t *out_written)
{ (void)disk_id; (void)partition_index; (void)offset; (void)buffer; (void)length; (void)out_written; abort(); }
int storage_disk_block_read(uint32_t disk_id,int32_t partition_index,uint64_t offset,
                               void *buffer,uint32_t length,uint32_t *read)
{(void)disk_id;(void)partition_index;(void)offset;(void)buffer;(void)length;(void)read;abort();}
int storage_sync_disk(uint32_t disk_id){(void)disk_id;abort();}
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
{
    assert(file && file->aux==0xf001 && transfer_pipe_mode);
    ++transfer_pipe_calls;
    bool wait=transfer_pipe_mode==1 ? transfer_pipe_calls==1 : transfer_pipe_calls==2;
    if(wait) {
        assert(!kernel_wait_queue_add(&transfer_pipe_waiters,descriptor_dispatch_task));
        return transfer_pipe_mode==1 ? KERNEL_SYSCALL_BLOCKED : -EAGAIN;
    }
    kernel_wait_queue_remove(&transfer_pipe_waiters,descriptor_dispatch_task);
    if(transfer_pipe_mode==2 && transfer_pipe_calls==1)length=1;
    assert(transfer_pipe_bytes+length<=sizeof(transfer_pipe_data));
    memcpy(transfer_pipe_data+transfer_pipe_bytes,buffer,length);transfer_pipe_bytes+=length;return (int)length;
}
int64_t drm_prime_file_rw(const struct task_file *file, bool writing)
{ (void)file; (void)writing; abort(); }
int task_shm_write(struct task_file *file, const void *buffer, uint32_t length)
{ (void)file; (void)buffer; (void)length; abort(); }
/* Ordinary-file vectors must never enter unrelated read-device backends. */
int kernel_random_fill(void *buffer,size_t length){(void)buffer;(void)length;abort();}
int64_t pty_read_input(uint32_t id,char *buffer,uint32_t length)
{(void)id;(void)buffer;(void)length;abort();}
int64_t pty_read_output(uint32_t owner,uint32_t id,char *buffer,uint32_t length)
{(void)owner;(void)id;(void)buffer;(void)length;abort();}
int64_t task_signalfd_read(struct task *task,struct task_file *file,uint64_t buffer,uint64_t count)
{(void)task;(void)file;(void)buffer;(void)count;abort();}
int64_t task_signalfd_readv(struct task *task,struct task_file *file,uint64_t vectors,uint64_t count,uint32_t flags)
{(void)task;(void)file;(void)vectors;(void)count;(void)flags;abort();}
int input_evdev_read_vt(uint32_t kind,uint64_t *cursor,void *buffer,uint32_t length,uint64_t grab,uint32_t number)
{(void)kind;(void)cursor;(void)buffer;(void)length;(void)grab;(void)number;abort();}
int task_pipe_read(struct task_file *file,void *buffer,uint32_t length)
{(void)file;(void)buffer;(void)length;abort();}
int task_shm_read(struct task_file *file,void *buffer,uint32_t length)
{(void)file;(void)buffer;(void)length;abort();}
#include "../../kernel/syscall/io/file_io.c"
void task_pipe_release(struct task_file *file) { (void)file; }
void task_shm_release(struct task_file *file) { (void)file; }
/* AF_UNIX audio transfers must not enter storage or IPv4 backends. */
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
int task_shm_truncate(struct task_file *file, uint64_t size)
{ (void)file; (void)size; abort(); }
int64_t task_pipe_capacity(struct task_file *file, bool setting, uint32_t bytes)
{ (void)file; (void)setting; (void)bytes; abort(); }
int64_t task_shm_get_seals(const struct task_file *file) { (void)file; abort(); }
int task_shm_add_seals(struct task_file *file, uint32_t seals)
{ (void)file; (void)seals; abort(); }
int proc_lookup(const char *path, struct storage_node *out)
{ (void)path; (void)out; abort(); }
/* Host userspace-address boundary: the core copy/lookup still owns and checks
 * its operands; these mapped test strings have no guest page tables. */
size_t user_strlen(const char *text,size_t maximum)
{
    size_t length=0;
    while(length<maximum && user_range_ok((uintptr_t)text+length,1) && text[length])++length;
    return length;
}
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

#include "../../sound/core/file.c"
#include "../../drivers/video/drm/event_io.c"
#include "../../drivers/video/drm/file_ops.c"
static int descriptor_character_open(struct task *task,struct task_file *file)
{
    struct linux_stat_abi status={.st_mode=LINUX_S_IFCHR};devfs_stat_identity(&status,&file->node);
    return character_file_open(status.st_rdev,task,file,&file->character_file);
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
    assert(!descriptor_character_open(task,file));
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
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd, 0));
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
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd, 0));
    assert(parent->syscall_file == task_file_description(source));
    assert(card.open_count == 1 && card.close_count == 0);
    clear_task_files(parent); clear_task_files(child);
    assert(!syscall_ioctl_resolve_fd(parent, fd));
    assert(task_file_for_io(parent, fd) == parent->syscall_file);
    struct task_file *replacement;
    assert(task_allocate_fd(parent, 0, &replacement) == fd);
    replacement->used = 1;
    replacement->flags = TASK_FILE_FLAG_DEV_NULL;
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd, 0));
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
    assert(!descriptor_character_open(parent,file));
    assert(card.open_count == 1 && audio_unregister_card(card.id, 0) == -EBUSY);
    assert(task_duplicate_file_fd(parent, fd, 0, 0) == 4);
    child->fd_table = parent->fd_table;
    assert(!syscall_clone_task_files(parent, child));
    assert(!task_begin_syscall_io(parent, LINUX_SYS_IOCTL, fd, 0));
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
    assert(!descriptor_character_open(task,file));
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

static void test_busy_module_unload_dispatch(void)
{
    struct task *task = calloc(1, sizeof(*task));
    assert(task);
    task->pid = task->tgid = 1399;
    descriptor_dispatch_task = task;
    descriptor_dispatch_sleep_count = 0;
    struct trap_frame frame = {.rax = LINUX_SYS_DELETE_MODULE,
        .rdi = (uintptr_t)"held_provider", .rsi = LINUX_O_NONBLOCK,
        .rip = 0x6002, .rsp = 0x8000};
    syscall_dispatch_frame(&frame);
    assert((int64_t)frame.rax == -RELIEFOS_EWOULDBLOCK);
    assert(frame.rip == 0x6002);
    assert(!descriptor_dispatch_sleep_count && !task->restart_syscall);
    descriptor_dispatch_task = NULL;
    free(task);
    puts("PASS busy delete_module returns EWOULDBLOCK to userspace without storage retry");
}

static void test_module_init_error_dispatch(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1398;descriptor_dispatch_task=task;
    const unsigned calls[]={LINUX_SYS_INIT_MODULE,LINUX_SYS_FINIT_MODULE};
    for(unsigned i=0;i<sizeof(calls)/sizeof(calls[0]);++i) {
        descriptor_dispatch_sleep_count=0;
        struct trap_frame frame={.rax=calls[i],.rdi=3,.rip=0x7002,.rsp=0x8000};
        syscall_dispatch_frame(&frame);
        assert((int64_t)frame.rax==-RELIEFOS_EAGAIN && frame.rip==0x7002);
        assert(!descriptor_dispatch_sleep_count && !task->restart_syscall);
    }
    descriptor_dispatch_task=NULL;free(task);
    puts("PASS init_module/finit_module final EAGAIN is returned without replaying initialization");
}

static void test_finit_module_retry_description(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1397;sched_task_limits(task)->nofile.rlim_cur=64;
    struct task_file *slot;
    int fd=task_allocate_fd(task,3,&slot);assert(fd==3);
    slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x1234;
    descriptor_module_source=task_file_get(slot);assert(descriptor_module_source);
    task_file_put(descriptor_module_source); /* Keep only the descriptor's reference. */
    descriptor_dispatch_task=task;descriptor_dispatch_sleep_count=0;
    descriptor_module_retry_stage=1;
    struct trap_frame frame={.rax=LINUX_SYS_FINIT_MODULE,.rdi=(uint64_t)fd,
        .rip=0x8002,.rsp=0x9000};
    syscall_dispatch_frame(&frame);
    assert(frame.rax==LINUX_SYS_FINIT_MODULE && frame.rip==0x8000 &&
           descriptor_dispatch_sleep_count==1 && task->restart_syscall==LINUX_SYS_FINIT_MODULE+1);
    assert(task->syscall_file==descriptor_module_source && task->syscall_file->node.first_cluster==0x1234);
    frame.rip+=2; /* The resumed SYSCALL instruction advances again. */
    syscall_dispatch_frame(&frame);
    assert((int64_t)frame.rax==-RELIEFOS_EAGAIN && frame.rip==0x8002 &&
           descriptor_dispatch_sleep_count==1 && !task->restart_syscall && !task->syscall_file);
    assert(task_file_for_fd(task,fd)->node.first_cluster==0x5678);
    task_discard_file_fd(task,fd);sched_task_file_release(task);
    descriptor_module_source=NULL;descriptor_module_retry_stage=0;descriptor_dispatch_task=NULL;
    free(task);
    puts("PASS finit_module snapshot retries retain the original OFD across close/reuse and return final init EAGAIN once");
}

static void test_mmap_retry_description(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);task->pid=task->tgid=1393;
    sched_task_limits(task)->nofile.rlim_cur=64;descriptor_dispatch_task=task;
    struct task_file *slot=NULL;int fd=task_allocate_fd(task,7,&slot);assert(fd==7);
    slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x1234;
    descriptor_mmap_source=task_file_get(slot);assert(descriptor_mmap_source);
    task_file_put(descriptor_mmap_source); /* Only the live descriptor owns it. */
    descriptor_mmap_retry_stage=1;
    struct trap_frame frame={.rax=LINUX_SYS_MMAP,.rdi=0x50000000,.rsi=4096,.rdx=LINUX_PROT_READ,
        .r10=LINUX_MAP_PRIVATE,.r8=(1ULL<<32)|(uint32_t)fd,.rip=0x7102,.rsp=0x8000};
    syscall_dispatch_frame(&frame);
    assert(frame.rax==LINUX_SYS_MMAP && frame.rip==0x7100 && task->syscall_fd==fd);
    assert(task->syscall_file==descriptor_mmap_source);
    frame.rip+=2;syscall_dispatch_frame(&frame);
    assert(frame.rax==0x50000000 && !task->syscall_file && !task->restart_syscall);
    /* Anonymous mappings ignore even an open numeric fd, and retire any
     * previous unrelated admission before the mapping callback. */
    assert(!task_begin_syscall_io(task,LINUX_SYS_READ,fd,0) && task->syscall_file);
    frame=(struct trap_frame){.rax=LINUX_SYS_MMAP,.rdi=0x51000000,.rsi=4096,
        .rdx=LINUX_PROT_READ,.r10=LINUX_MAP_PRIVATE|LINUX_MAP_ANONYMOUS,.r8=(uint32_t)fd,.rip=0x7202,.rsp=0x8000};
    syscall_dispatch_frame(&frame);assert(frame.rax==0x51000000 && !task->syscall_file);
    task_discard_file_fd(task,fd);sched_task_file_release(task);free(task);
    descriptor_dispatch_task=NULL;descriptor_mmap_source=NULL;descriptor_mmap_retry_stage=0;
    puts("PASS mmap admits fifth-argument OFD across metadata retry/fd reuse and ignores fd for anonymous mappings");
}

static void test_fd_metadata_retry_description(void)
{
    const uint64_t calls[]={LINUX_SYS_FSTAT,LINUX_SYS_FSTATFS,LINUX_SYS_FCHMOD,
        LINUX_SYS_FCHOWN,LINUX_SYS_FCHDIR,LINUX_SYS_FTRUNCATE,LINUX_SYS_FALLOCATE,
        LINUX_SYS_GETDENTS,LINUX_SYS_GETDENTS64,LINUX_SYS_FSYNC,LINUX_SYS_FDATASYNC,
        LINUX_SYS_SYNCFS,LINUX_SYS_LSEEK,LINUX_SYS_UTIMENSAT,LINUX_SYS_FUTIMESAT};
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1394;sched_task_limits(task)->nofile.rlim_cur=64;
    for(size_t i=0;i<sizeof(calls)/sizeof(calls[0]);++i) {
        struct task_file *slot=NULL;int fd=task_allocate_fd(task,3,&slot);assert(fd==3);
        slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x1234;
        assert(!task_begin_syscall_io(task,calls[i],fd,0));
        assert(task->syscall_file && task->syscall_file->node.first_cluster==0x1234);
        task_discard_file_fd(task,fd);
        assert(task_allocate_fd(task,3,&slot)==fd);
        slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x5678;
        assert(task_file_for_io(task,fd)->node.first_cluster==0x1234);
        task_release_syscall_file(task);
        assert(task_file_for_io(task,fd)->node.first_cluster==0x5678);
        task_discard_file_fd(task,fd);
    }
    sched_task_file_release(task);free(task);
    puts("PASS fd metadata/directory/truncate/sync/seek admission retains exact OFD across fd close/reuse");
}

static void test_standard_character_stat(void)
{
    const struct {uint32_t kind,major,minor;} devices[]={
        {STORAGE_DEV_KIND_NULL,1,3},{STORAGE_DEV_KIND_ZERO,1,5},
        {STORAGE_DEV_KIND_FULL,1,7},{STORAGE_DEV_KIND_RANDOM,1,8},
        {STORAGE_DEV_KIND_URANDOM,1,9},{STORAGE_DEV_KIND_TTY,5,0},
        {STORAGE_DEV_KIND_PTMX,5,2},{STORAGE_DEV_KIND_SERIAL,4,64},
        {STORAGE_DEV_KIND_FB0,29,0}};
    for(size_t i=0;i<sizeof(devices)/sizeof(devices[0]);++i) {
        struct storage_node node={.type=RELIEFOS_FS_TYPE_DEVICE,.flags=STORAGE_NODE_FLAG_DEV_NODE,
            .first_cluster=devices[i].kind};
        struct linux_stat_abi st={.st_mode=LINUX_S_IFCHR};
        linux_stat_device_identity(&st,&node);
        assert(st.st_rdev==((uint64_t)devices[i].major<<8|devices[i].minor));
    }
    for(unsigned type=0;type<2;++type) {
        struct storage_node native={.type=RELIEFOS_FS_TYPE_DEVICE,
            .flags=STORAGE_NODE_FLAG_TMPFS|STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_RDEV,
            .first_cluster=STORAGE_DEV_KIND_NULL};
        struct linux_stat_abi st={.st_mode=type ? LINUX_S_IFBLK|0600 : LINUX_S_IFCHR|0600,
            .st_rdev=type ? (uint64_t)259<<8 | 0x12 | (uint64_t)0xabc00<<12 : 0x501};
        uint64_t expected=st.st_rdev;linux_stat_device_identity(&st,&native);assert(st.st_rdev==expected);
    }
    puts("PASS Linux devfs identities and independent native inode rdev");
}

static void test_materialized_console_permissions(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->cap_effective=UINT64_MAX;
    assert(!console_references && !task_console_materialize_stdio(task));
    struct task_file *file=task_file_for_fd(task,0);assert(file && !file->path_ref.mount);
    struct linux_stat_abi original,changed;
    assert(!metadata_fd_stat(task,0,&original));
    assert(!syscall_permissions(NULL,task,LINUX_SYS_FCHMOD,0,0600,0,0,0));
    assert(!syscall_permissions(NULL,task,LINUX_SYS_FCHOWN,0,123,456,0,0));
    assert(!metadata_fd_stat(task,0,&changed) && (changed.st_mode&07777)==0600 &&
        changed.st_uid==123 && changed.st_gid==456);
    task->cap_effective=0;task->fsuid=789;
    assert(syscall_permissions(NULL,task,LINUX_SYS_FCHMOD,0,0666,0,0,0)==-RELIEFOS_EPERM);
    task->cap_effective=UINT64_MAX;task->fsuid=0;
    file->flags|=TASK_FILE_FLAG_PATH;
    assert(syscall_permissions(NULL,task,LINUX_SYS_FCHMOD,0,0666,0,0,0)==-RELIEFOS_EBADF);
    file->flags&=~TASK_FILE_FLAG_PATH;
    assert(!syscall_permissions(NULL,task,LINUX_SYS_FCHOWN,0,original.st_uid,original.st_gid,0,0));
    assert(!syscall_permissions(NULL,task,LINUX_SYS_FCHMOD,0,original.st_mode&07777,0,0,0));
    assert(!metadata_fd_stat(task,0,&changed) && changed.st_mode==original.st_mode &&
        changed.st_uid==original.st_uid && changed.st_gid==original.st_gid);
    for(int fd=0;fd<3;++fd)task_discard_file_fd(task,fd);
    assert(!console_references);free(task);
    puts("PASS materialized console OFD metadata: pathless builtin identity, DAC, O_PATH, visible updates and exact restoration");
}

static void test_finit_preflight_before_allocation(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1396;sched_task_limits(task)->nofile.rlim_cur=64;
    struct task_file *slot;int fd=task_allocate_fd(task,3,&slot);assert(fd==3);
    slot->flags=TASK_FILE_FLAG_DEV_NULL;descriptor_dispatch_task=task;
    const int errors[]={-RELIEFOS_EPERM,-RELIEFOS_ENOSYS,-RELIEFOS_EBADF,-RELIEFOS_EINVAL};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);++i) {
        descriptor_module_prepare_error=errors[i];fail_allocation=1;
        struct trap_frame frame={.rax=LINUX_SYS_FINIT_MODULE,.rdi=(uint64_t)fd,
            .rip=0x9002,.rsp=0xa000};
        syscall_dispatch_frame(&frame);
        assert((int64_t)frame.rax==errors[i] && frame.rip==0x9002 &&
               !task->syscall_file && !slot->description);
    }
    fail_allocation=0;descriptor_module_prepare_error=0;
    task_discard_file_fd(task,fd);sched_task_file_release(task);
    descriptor_dispatch_task=NULL;free(task);
    puts("PASS finit_module authorization/flags/descriptor errors precede OFD allocation even under OOM");
}

static void test_finit_preflight_retires_other_io(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1395;sched_task_limits(task)->nofile.rlim_cur=64;
    struct task_file *slot;int fd=task_allocate_fd(task,3,&slot);assert(fd==3);
    slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x1234;
    task->syscall_file=task_file_get(slot);assert(task->syscall_file);
    task->syscall_fd=fd;task->syscall_file_number=LINUX_SYS_READ;
    task_discard_file_fd(task,fd);assert(task_allocate_fd(task,3,&slot)==fd);
    slot->flags=TASK_FILE_FLAG_DEV_NULL;slot->node.first_cluster=0x5678;
    descriptor_module_check_selection=true;descriptor_dispatch_task=task;
    struct trap_frame frame={.rax=LINUX_SYS_FINIT_MODULE,.rdi=(uint64_t)fd,
        .rip=0x9102,.rsp=0xa000};
    syscall_dispatch_frame(&frame);
    assert((int64_t)frame.rax==-RELIEFOS_EAGAIN && frame.rip==0x9102 && !task->syscall_file);
    descriptor_module_check_selection=false;descriptor_dispatch_task=NULL;
    task_discard_file_fd(task,fd);sched_task_file_release(task);free(task);
    puts("PASS finit_module retires an unrelated syscall pin before validating the reused descriptor");
}

/* One regular inode provider boundary around the actual namespace, DAC,
 * canonical OFD, open/truncate and stat dispatch implementations. */
static unsigned writer_path_pins;
static int writer_path_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    (void)context;assert(parent->type==RELIEFOS_FS_TYPE_DIR);
    if(strcmp(name,"module.ko"))return -ENOENT;*out=descriptor_inode.node;return 0;
}
static int writer_path_retain(void *context,const struct storage_node *node,void **pin)
{(void)context;assert(node);++writer_path_pins;*pin=&writer_path_pins;return 0;}
static void writer_path_release(void *context,const struct storage_node *node,void *pin)
{(void)context;assert(node && pin==&writer_path_pins && writer_path_pins);--writer_path_pins;}
static bool writer_path_same(void *context,const struct storage_node *one,const struct storage_node *two)
{(void)context;return one->first_cluster==two->first_cluster && one->type==two->type;}
static int writer_path_permissions(void *context,const struct vfs_path *path,struct reliefos_permissions *value,bool write)
{
    (void)context;assert(!write);*value=(struct reliefos_permissions){vfs_path_node(path)->type==RELIEFOS_FS_TYPE_DIR ? 0755 : 0644,0,0};return 0;
}
static int writer_path_truncate(void *context,struct storage_node *node,uint64_t size)
{(void)context;return storage_truncate_held_node(node,size);}
static void writer_namespace_open(struct vfs_namespace *ns,struct task *task,struct vfs_path *root)
{
    static const struct vfs_backend_ops ops={.lookup=writer_path_lookup,.retain=writer_path_retain,
        .release=writer_path_release,.same_node=writer_path_same,.permissions=writer_path_permissions,.truncate=writer_path_truncate};
    struct storage_node node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=1};struct vfs_mount *mount=NULL;
    assert(!writer_path_pins && !vfs_mount_new(&ops,NULL,NULL,&node,&mount));
    assert(!vfs_namespace_init(ns,mount));vfs_mount_put(mount);assert(!vfs_namespace_root(ns,root));
    vfs_path_get(root,&task->fs_paths.root);vfs_path_get(root,&task->fs_paths.cwd);
}
static void writer_namespace_close(struct vfs_namespace *ns,struct task *task,struct vfs_path *root)
{
    vfs_path_put(&task->fs_paths.root);vfs_path_put(&task->fs_paths.cwd);vfs_path_put(root);
    assert(!vfs_namespace_destroy(ns) && !writer_path_pins);
}
static void test_regular_inode_writer_ofd_lifetime(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    task->pid=task->tgid=1394;sched_task_limits(task)->nofile.rlim_cur=64;
    sched_task_fds(task)->closed_stdio_mask=7;
    descriptor_inode=(struct storage_inode_ref){.references=1,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.flags=STORAGE_NODE_FLAG_TMPFS,.first_cluster=501}};
    struct vfs_namespace ns={0};struct vfs_path root={0};writer_namespace_open(&ns,task,&root);
    descriptor_inode_get_error=-ENOMEM;
    assert(alloc_task_fd(task,&descriptor_inode.node,LINUX_O_RDONLY,"/module.ko")==-ENOMEM);
    descriptor_inode_get_error=0;
    int reader=alloc_task_fd(task,&descriptor_inode.node,LINUX_O_RDONLY,"/module.ko");assert(reader==0);
    assert(!storage_inode_deny_write(&descriptor_inode));
    assert(alloc_task_fd(task,&descriptor_inode.node,LINUX_O_RDWR,"/module.ko")==-ETXTBSY);
    assert(descriptor_inode.references==2 && !task_file_for_fd(task,1));
    storage_inode_allow_write(&descriptor_inode);
    assert(!storage_inode_deny_write(&descriptor_inode));descriptor_truncates=0;
    assert(syscall_open_object(&ns,task,LINUX_AT_FDCWD,(uintptr_t)"/module.ko",LINUX_O_RDWR|LINUX_O_TRUNC,0)==-ETXTBSY);
    assert(!descriptor_truncates && descriptor_inode.references==2);
    storage_inode_allow_write(&descriptor_inode);
    int truncated=syscall_open_object(&ns,task,LINUX_AT_FDCWD,(uintptr_t)"/module.ko",LINUX_O_RDWR|LINUX_O_TRUNC|LINUX_O_APPEND,0);
    assert(truncated==1 && descriptor_truncates==1 && !task_file_for_fd(task,truncated)->node.size &&
           !task_file_for_fd(task,truncated)->offset);
    task_discard_file_fd(task,truncated);
    descriptor_truncate_error=-EIO;
    assert(syscall_open_object(&ns,task,LINUX_AT_FDCWD,(uintptr_t)"/module.ko",LINUX_O_RDWR|LINUX_O_TRUNC,0)==-EIO);
    descriptor_truncate_error=0;assert(descriptor_inode.references==2 && !task_file_for_fd(task,1));
    assert(!storage_inode_deny_write(&descriptor_inode));storage_inode_allow_write(&descriptor_inode);
    int writer=alloc_task_fd(task,&descriptor_inode.node,LINUX_O_RDWR,"/module.ko");assert(writer==1);
    assert(storage_inode_deny_write(&descriptor_inode)==-ETXTBSY);
    int alias=task_duplicate_file_fd(task,writer,3,0);assert(alias==3);
    struct task_file *inflight=task_file_get(task_file_for_fd(task,writer));assert(inflight);
    task_discard_file_fd(task,writer);task_discard_file_fd(task,alias);
    assert(storage_inode_deny_write(&descriptor_inode)==-ETXTBSY);
    task_file_put(inflight);
    assert(!storage_inode_deny_write(&descriptor_inode));storage_inode_allow_write(&descriptor_inode);
    task_discard_file_fd(task,reader);assert(descriptor_inode.references==1);
    writer_namespace_close(&ns,task,&root);
    sched_task_file_release(task);free(task);
    puts("PASS regular inode writer lease follows actual OFD dup/final close and in-flight I/O, with denied-open rollback");
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
    assert(!character_file_open(226u<<8,task,file,&file->character_file));
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

    assert(character_file_read(file->character_file,task,file,&byte,1) == -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 3);
    assert(syscall_legacy_io(LINUX_SYS_PREAD64,fd,(uintptr_t)&byte,1,4096,0,0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 4);
    assert(file->offset == 0);
    assert(syscall_legacy_io(LINUX_SYS_PREAD64,fd,(uintptr_t)&byte,1,UINT64_MAX,0,0) ==
           -RELIEFOS_EINVAL);
    assert(descriptor_drm_read_count == 4);
    assert(character_file_readv(file->character_file,task,file, &vector, 1, false, 0, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 5);
    assert(character_file_readv(file->character_file,task,file, &vector, 1, true, 4096, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 6);
    assert(file->offset == 0);
    assert(character_file_readv(file->character_file,task,file, &vector, 1, false, UINT64_MAX, 0) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 7);
    assert(character_file_readv(file->character_file,task,file, &vector, 1, true, 4096, 1) ==
           -RELIEFOS_EAGAIN);
    assert(descriptor_drm_read_count == 8);
    assert(character_file_readv(file->character_file,task,file, &vector, 1, true, 4096, 2) ==
           -RELIEFOS_EOPNOTSUPP);
    assert(character_file_readv(file->character_file,task,file, &vector, 0, true, UINT64_MAX, 0) ==
           -RELIEFOS_EINVAL);
    assert(character_file_readv(file->character_file,task,file, &vector, 0, false, UINT64_MAX, 1) == 0);
    assert(file->offset == 0);
    assert(descriptor_drm_read_count == 8);

    file->offset = 37;
    for (uint64_t whence = 0; whence <= 4; ++whence)
        assert(character_file_seek(file->character_file,file,987,(uint32_t)whence) == 37);
    assert(character_file_seek(file->character_file,file,0,5) == -RELIEFOS_EINVAL);
    assert(character_file_seek(file->character_file,file,0,UINT32_MAX) == -RELIEFOS_EINVAL);
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
    assert(task_begin_syscall_io(task, LINUX_SYS_READ, watched, 0) == 0);
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

static unsigned descriptor_path_pins;
static unsigned descriptor_open_states,descriptor_open_closes;
static int descriptor_path_open(void *context,const struct storage_node *node,uint32_t flags,void **state)
{
    (void)context;(void)flags;assert(node->type==RELIEFOS_FS_TYPE_DIR);
    *state=kernel_malloc(1);if(!*state)return -ENOMEM;++descriptor_open_states;return 0;
}
static int descriptor_path_open_read(void *context,const struct storage_node *node,void *state,
                                        uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{(void)context;(void)node;(void)state;(void)offset;(void)buffer;(void)capacity;(void)mount_flags;*read=0;return 0;}
static void descriptor_path_close(void *context,const struct storage_node *node,void *state)
{
    (void)context;assert(node->type==RELIEFOS_FS_TYPE_DIR && state && descriptor_path_pins && descriptor_open_states);
    --descriptor_open_states;++descriptor_open_closes;kernel_free(state);
}
static int descriptor_path_lookup(void *context,const struct storage_node *parent,
                                  const char *name,struct storage_node *out)
{ (void)context;(void)parent;(void)name;(void)out;return -RELIEFOS_ENOENT; }
static int descriptor_path_retain(void *context,const struct storage_node *node,void **pin)
{ (void)context;assert(node->type==RELIEFOS_FS_TYPE_DIR);++descriptor_path_pins;*pin=&descriptor_path_pins;return 0; }
static void descriptor_path_release(void *context,const struct storage_node *node,void *pin)
{ (void)context;(void)node;assert(pin==&descriptor_path_pins && descriptor_path_pins);--descriptor_path_pins; }
static bool descriptor_path_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ (void)context;return a->first_cluster==b->first_cluster; }
static void test_ofd_owned_mount_path(void)
{
    static const struct vfs_backend_ops ops={.lookup=descriptor_path_lookup,
        .retain=descriptor_path_retain,.release=descriptor_path_release,.same_node=descriptor_path_same,
        .file_open=descriptor_path_open,.file_read=descriptor_path_open_read,.file_close=descriptor_path_close};
    struct storage_node node={.type=RELIEFOS_FS_TYPE_DIR,.first_cluster=7};
    struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};struct vfs_path root={0};
    assert(!vfs_mount_new(&ops,NULL,NULL,&node,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    assert(!vfs_namespace_root(&ns,&root));
    struct task *task=descriptor_audio_task(1100);descriptor_dispatch_task=task;
    struct trap_frame scope_frame={.rax=LINUX_SYS_OPENAT,.rdi=3};
    task_lookup_scope_begin(task,&scope_frame);
    struct task_lookup_path *operand=NULL;assert(!task_lookup_scope_next(task,&operand) && operand);
    operand->name=kernel_malloc(2);assert(operand->name);memcpy(operand->name,".",2);
    vfs_path_get(&root,&operand->root);vfs_path_get(&root,&operand->start);
    task_release_syscall_file(task);
    assert(!task->lookup_scope.armed && !task->lookup_scope.paths[0].name);
    int fd=alloc_task_fd(task,&node,RELIEFOS_O_RDONLY,"/old-name");assert(fd>=0);
    struct task_file *file=task_file_for_fd(task,fd);
    task_file_bind_path(file,&root);vfs_path_put(&root);
    assert(!vfs_file_open(&file->path_ref,LINUX_O_RDONLY,&file->provider_file) && file->provider_file);
    assert(vfs_namespace_destroy(&ns)==-RELIEFOS_EBUSY);
    fail_allocation=1;
    assert(task_duplicate_file_fd(task,fd,0,0)==-RELIEFOS_ENOMEM);
    fail_allocation=0;
    int alias=task_duplicate_file_fd(task,fd,0,0);assert(alias>=0 && alias!=fd);
    assert(descriptor_open_states==1 && !descriptor_open_closes);
    assert(task_file_for_fd(task,alias)->provider_file==task_file_for_fd(task,fd)->provider_file);
    struct task_file *held=task_file_get(task_file_for_fd(task,alias));assert(held);
    assert(vfs_path_node(&held->path_ref)->first_cluster==7);
    task_discard_file_fd(task,fd);task_discard_file_fd(task,alias);
    clear_task_files(task);free(task);
    assert(vfs_namespace_destroy(&ns)==-RELIEFOS_EBUSY);
    task_file_put(held);
    assert(!descriptor_open_states && descriptor_open_closes==1);
    assert(!vfs_namespace_destroy(&ns) && !descriptor_path_pins);
    descriptor_dispatch_task=NULL;
    puts("PASS OFD mount path survives dup, failed promotion, close and last in-flight reference");
}

/* Real canonical OFDs and VFS lifetimes; this provider is only the disk I/O
 * boundary. A retry must not reopen diagnostic names or re-read consumed data. */
static struct {unsigned char bytes[256];size_t size;} transfer_files[4];
static unsigned transfer_provider_pins,transfer_reads,transfer_writes,transfer_drains;
static struct kernel_wait_queue transfer_waiters={.lock=KERNEL_SPINLOCK_INIT};
static bool transfer_wait_write,transfer_wait_read,transfer_wait_sync;
static unsigned vector_wait_read_at,vector_wait_write_at;
static int vector_wait_status=-EAGAIN;
static unsigned vector_wait_partial;
/* Translate only the fake disk's sparse window; all VFS/OFD code is real. */
static uint64_t transfer_origin;
static unsigned transfer_index(const struct storage_node *node)
{assert(node && node->first_cluster>=500 && node->first_cluster<504);return node->first_cluster-500;}
static int transfer_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -ENOENT;}
static int transfer_retain(void *context,const struct storage_node *node,void **pin)
{assert(context==transfer_files && node && pin);++transfer_provider_pins;*pin=NULL;return 0;}
static void transfer_release(void *context,const struct storage_node *node,void *pin)
{assert(context==transfer_files && node && !pin && transfer_provider_pins && !transfer_waiters.count);--transfer_provider_pins;}
static bool transfer_same(void *context,const struct storage_node *a,const struct storage_node *b)
{assert(context==transfer_files);return a->first_cluster==b->first_cluster;}
static int transfer_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{assert(context==transfer_files);*out=(struct linux_stat_abi){.st_size=(int64_t)(transfer_origin+transfer_files[transfer_index(vfs_path_node(path))].size)};return 0;}
static int transfer_read(void *context,const struct storage_node *node,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read,uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context==transfer_files && transfer_provider_pins);unsigned index=transfer_index(node);++transfer_reads;*read=0;
    assert(offset>=transfer_origin);offset-=transfer_origin;
    if(vector_wait_read_at==transfer_reads) {
        vector_wait_read_at=0;
        if(vector_wait_partial){assert(offset<transfer_files[index].size && capacity);memcpy(buffer,transfer_files[index].bytes+offset,1);*read=1;}
        assert(!kernel_wait_queue_add(&transfer_waiters,descriptor_dispatch_task));return vector_wait_status;
    }
    if(transfer_wait_read){transfer_wait_read=false;assert(!kernel_wait_queue_add(&transfer_waiters,descriptor_dispatch_task));return -EAGAIN;}
    if(offset>=transfer_files[index].size)return 0;
    if(capacity>transfer_files[index].size-offset)capacity=(uint32_t)(transfer_files[index].size-offset);
    memcpy(buffer,transfer_files[index].bytes+offset,capacity);*read=capacity;return 0;
}
static int transfer_write(void *context,struct storage_node *node,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written)
{
    assert(context==transfer_files && transfer_provider_pins);unsigned index=transfer_index(node);++transfer_writes;*written=0;
    assert(offset>=transfer_origin);offset-=transfer_origin;
    if(vector_wait_write_at==transfer_writes) {
        vector_wait_write_at=0;
        if(vector_wait_partial){assert(length && offset<sizeof(transfer_files[index].bytes));transfer_files[index].bytes[offset]=*(const unsigned char *)buffer;
            *written=1;if(offset+1>transfer_files[index].size)transfer_files[index].size=offset+1;node->size=transfer_origin+transfer_files[index].size;}
        assert(!kernel_wait_queue_add(&transfer_waiters,descriptor_dispatch_task));return vector_wait_status;
    }
    if(transfer_wait_write){transfer_wait_write=false;return -EAGAIN;}
    if(offset>sizeof(transfer_files[index].bytes) || length>sizeof(transfer_files[index].bytes)-offset)return -ENOSPC;
    memcpy(transfer_files[index].bytes+offset,buffer,length);*written=length;
    if(offset+length>transfer_files[index].size)transfer_files[index].size=offset+length;
    node->size=transfer_origin+transfer_files[index].size;return 0;
}
static int transfer_sync(void *context,const struct storage_node *node,bool filesystem,bool data_only)
{
    assert(context==transfer_files && node && !filesystem && data_only);
    if(transfer_wait_sync){transfer_wait_sync=false;return -EAGAIN;}return 0;
}
static const struct vfs_backend_ops transfer_ops={.lookup=transfer_lookup,.retain=transfer_retain,
    .release=transfer_release,.same_node=transfer_same,.getattr=transfer_stat,.read=transfer_read,.write=transfer_write,.sync=transfer_sync};
static int transfer_open(struct task *task,struct vfs_mount *mount,unsigned index)
{
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=500+index,.size=transfer_origin+transfer_files[index].size};
    struct vfs_path path={0};assert(!vfs_path_anonymous(mount,&node,&path));
    int fd=alloc_task_fd(task,&node,RELIEFOS_O_RDWR,"/replaced/diagnostic-name");assert(fd>=0);
    task_file_bind_path(task_file_for_fd(task,fd),&path);vfs_path_put(&path);return fd;
}
int storage_read_node(const struct storage_node *node,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read)
{(void)node;(void)offset;(void)buffer;(void)capacity;(void)read;fputs("unexpected bare transfer read\n",stderr);abort();}
int storage_write_held_node(struct storage_node *node,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written)
{(void)node;(void)offset;(void)buffer;(void)length;(void)written;abort();}
void storage_drain_task_io(uint32_t pid){assert(descriptor_dispatch_task && pid==descriptor_dispatch_task->pid);++transfer_drains;descriptor_dirent_io_pending=false;}
uint64_t time_uptime_us(void){return 0;}
/* Transfer completion is in this current task; the signal fixture exercises
 * the real destination-MM copy while an unrelated CR3 remains active. */
int user_copy_to_task(struct task *task,uint64_t address,const void *bytes,uint64_t length)
{
    assert(task==descriptor_dispatch_task);
    if(!user_range_writable(address,length))return -EFAULT;
    memcpy((void *)(uintptr_t)address,bytes,(size_t)length);return 0;
}

static void test_transfer_held_providers(void)
{
    struct task *task=descriptor_audio_task(1900);descriptor_dispatch_task=task;
    memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"original payload",16);transfer_files[0].size=16;
    memcpy(transfer_files[2].bytes,"replacement data",16);transfer_files[2].size=16;
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;struct vfs_namespace ns={0};
    assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);mount=ns.root;
    int source=transfer_open(task,mount,0),target=transfer_open(task,mount,1);
    int64_t from=1,to=2;transfer_wait_write=true;transfer_reads=transfer_writes=0;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,target,(uintptr_t)&to,5,0)==-EAGAIN);
    assert(transfer_reads==1 && from==1 && to==2);
    task_discard_file_fd(task,source);task_discard_file_fd(task,target);
    assert(transfer_provider_pins==3); /* Only the pending transfer owns old files. */
    int replacement_source=transfer_open(task,mount,2),replacement_target=transfer_open(task,mount,3);
    assert(replacement_source==source && replacement_target==target && transfer_provider_pins==5);
    memcpy(transfer_files[0].bytes+1,"XXXXX",5);from=77;to=88;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,target,(uintptr_t)&to,5,0)==5);
    assert(from==6 && to==7 && transfer_reads==1 && !memcmp(transfer_files[1].bytes+2,"rigin",5));
    assert(!transfer_files[3].size && transfer_provider_pins==3);
    /* Copied data/ranges belong to the actual same inode, not fd equality. */
    from=0;to=1;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,source,(uintptr_t)&to,3,0)==-EINVAL);
    from=0;to=40;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,source,(uintptr_t)&to,3,0)==3);
    assert(!memcmp(transfer_files[2].bytes+40,"rep",3));
    task_file_for_fd(task,target)->flags|=RELIEFOS_O_APPEND;
    from=0;to=0;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,target,(uintptr_t)&to,1,0)==-EBADF);
    assert(syscall_sendfile(target,source,(uintptr_t)&from,1)==-EINVAL);
    task_file_for_fd(task,target)->flags&=~RELIEFOS_O_APPEND;
    /* Interrupting a completed copy waiting for DSYNC returns accepted bytes
     * and publishes original offset snapshots rather than reporting EINTR. */
    task_file_for_fd(task,target)->flags|=LINUX_O_DSYNC;transfer_wait_sync=true;from=to=0;
    assert(syscall_copy_file_range(source,(uintptr_t)&from,target,(uintptr_t)&to,3,0)==-EAGAIN);
    assert(!from && !to && task_transfer_interrupt(task)==3 && from==3 && to==3);
    task_release_syscall_file(task);task_file_for_fd(task,target)->flags&=~LINUX_O_DSYNC;
    /* Cancellation with closed descriptors releases both provider lifetimes. */
    transfer_wait_read=true;
    assert(syscall_sendfile(target,source,(uintptr_t)&from,3)==-EAGAIN);
    assert(task_transfer_interrupt(task)==0);
    task_discard_file_fd(task,source);task_discard_file_fd(task,target);
    assert(transfer_provider_pins==3);
    task_release_syscall_file(task);assert(transfer_provider_pins==1 && transfer_drains && !transfer_waiters.count && !task->waiting_queue);
    assert(!vfs_namespace_destroy(&ns) && !transfer_provider_pins);
    clear_task_files(task);free(task);descriptor_dispatch_task=NULL;
    puts("PASS transfer provider/OFD pins survive read/write waits, fd close/reuse and input changes; buffered bytes, offsets, overlap, append and cancellation");
}

/* Stream queue/scheduler are the boundary here; the real VFS/OFD ownership
 * and buffer phase must survive both parked statuses, including partial output. */
static void test_transfer_blocked_stream(void)
{
    for(unsigned mode=1;mode<=2;++mode) {
        struct task *task=descriptor_audio_task(1901);descriptor_dispatch_task=task;
        struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR},fifo={.type=RELIEFOS_FS_TYPE_FIFO};
        struct vfs_namespace ns={0};struct vfs_mount *mount=NULL;
        memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"original",8);transfer_files[0].size=8;
        assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));
        assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
        int source=transfer_open(task,ns.root,0);
        int output=alloc_task_fd(task,&fifo,RELIEFOS_O_WRONLY|TASK_FILE_FLAG_PIPE,NULL);assert(output>=0);
        task_file_for_fd(task,output)->aux=0xf001;
        transfer_pipe_mode=mode;transfer_pipe_calls=transfer_pipe_bytes=0;
        int64_t offset=0;
        assert(syscall_sendfile(output,source,(uintptr_t)&offset,3)==(mode==1 ? KERNEL_SYSCALL_BLOCKED : -EAGAIN));
        assert(!offset && transfer_pipe_waiters.count==1);
        task_discard_file_fd(task,source);task_discard_file_fd(task,output);
        assert(transfer_provider_pins==2);
        memcpy(transfer_files[0].bytes,"changed!",8);
        assert(syscall_sendfile(output,source,(uintptr_t)&offset,3)==3 && offset==3);
        assert(transfer_pipe_bytes==3 && !memcmp(transfer_pipe_data,"ori",3));
        assert(!transfer_pipe_waiters.count && !task->waiting_queue && transfer_provider_pins==1);
        assert(!vfs_namespace_destroy(&ns) && !transfer_provider_pins);
        clear_task_files(task);free(task);descriptor_dispatch_task=NULL;
    }
    transfer_pipe_mode=0;
    puts("PASS transfer retains buffered prefix/OFDs across parked stream and partial-output queue waits");
}

struct dirent_catalog {unsigned root,pins;};
static struct dirent_catalog dirent_base={100,0},dirent_mounted={200,0};
static bool dirent_wait_metadata,dirent_bad_name;
static int dirent_metadata_error;
static struct kernel_wait_queue dirent_waiters={.lock=KERNEL_SPINLOCK_INIT};
static struct storage_node dirent_node(unsigned id)
{
    uint32_t type=id==204 ? RELIEFOS_FS_TYPE_SYMLINK : id==205 || id==206 ? RELIEFOS_FS_TYPE_DEVICE :
        id==201 || id==202 ? RELIEFOS_FS_TYPE_FILE : RELIEFOS_FS_TYPE_DIR;
    return (struct storage_node){.type=type,.first_cluster=id};
}
static int dirent_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{
    struct dirent_catalog *catalog=context;
    if(catalog==&dirent_base && parent->first_cluster==100 && !strcmp(name,"point")){*out=dirent_node(101);return 0;}
    static const char *names[]={"file","sock","kid","sym","blk","chr"};
    if(catalog==&dirent_mounted && parent->first_cluster==200)
        for(unsigned i=0;i<6;++i)if(!strcmp(name,names[i])){*out=dirent_node(201+i);return 0;}
    return -ENOENT;
}
static int dirent_retain(void *context,const struct storage_node *node,void **pin)
{struct dirent_catalog *catalog=context;assert(node);++catalog->pins;*pin=NULL;return 0;}
static void dirent_release(void *context,const struct storage_node *node,void *pin)
{struct dirent_catalog *catalog=context;assert(node && !pin && catalog->pins);if(!--catalog->pins)assert(!dirent_waiters.count && !descriptor_dirent_io_pending);}
static bool dirent_same(void *context,const struct storage_node *a,const struct storage_node *b)
{(void)context;return a->first_cluster==b->first_cluster;}
static int dirent_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{
    (void)context;unsigned id=vfs_path_node(path)->first_cluster;
    if(descriptor_dispatch_task)kernel_wait_queue_remove(&dirent_waiters,descriptor_dispatch_task);
    descriptor_dirent_io_pending=false;
    if(id==201 && dirent_metadata_error)return dirent_metadata_error;
    if(id==201 && dirent_wait_metadata) {
        dirent_wait_metadata=false;descriptor_dirent_io_pending=true;
        assert(!kernel_wait_queue_add(&dirent_waiters,descriptor_dispatch_task));return -EAGAIN;
    }
    uint32_t mode=id==201 ? 0100600 : id==202 ? 0140600 : id==204 ? 0120777 :
        id==205 ? 0060600 : id==206 ? 0020600 : 0040755;
    *out=(struct linux_stat_abi){.st_ino=id,.st_mode=mode,.st_size=id==201 ? 16 : 0};return 0;
}
static int dirent_read_directory(void *context,const struct storage_node *node,uint64_t *cursor,struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)mount_flags;
    assert(context==&dirent_mounted && node->first_cluster==200);
    static const char *names[]={".","..","file","sock","kid","sym","blk","chr"};
    if(*cursor>=8)return 0;
    *entry=(struct reliefos_dir_entry){.type=*cursor==0 || *cursor==1 || *cursor==4 ? RELIEFOS_FS_TYPE_DIR :
        *cursor>=6 ? RELIEFOS_FS_TYPE_DEVICE : *cursor==5 ? RELIEFOS_FS_TYPE_SYMLINK : RELIEFOS_FS_TYPE_FILE};
    if(dirent_bad_name)memset(entry->name,'x',sizeof(entry->name));
    else strcpy(entry->name,names[*cursor]);
    ++*cursor;return 1;
}
int storage_readdir_node(const struct storage_node *node,uint64_t *cursor,struct reliefos_dir_entry *entry)
{(void)node;(void)cursor;(void)entry;abort();}
int proc_readdir(const char *name,uint64_t *cursor,struct reliefos_dir_entry *entry)
{(void)name;(void)cursor;(void)entry;abort();}
static void verify_dirent_records(const uint8_t *bytes,uint64_t length,bool wide)
{
    static const char *names[]={".","..","file","sock","kid","sym","blk","chr"};
    static const uint64_t inodes[]={200,200,201,202,203,204,205,206};
    static const uint8_t types[]={4,4,8,12,4,10,6,2};
    uint64_t at=0;
    for(unsigned i=0;i<8;++i) {
        if(i==2 && dirent_metadata_error==-ENOENT)continue;
        assert(length-at>=24);uint64_t inode=0,offset=0;uint16_t record=0;
        memcpy(&inode,bytes+at,8);memcpy(&offset,bytes+at+8,8);memcpy(&record,bytes+at+16,2);
        assert(record>=24 && !(record&7) && record<=length-at);
        assert(inode==inodes[i] && offset==i+1 && !strcmp((const char *)bytes+at+(wide ? 19 : 18),names[i]));
        assert(bytes[at+(wide ? 18 : record-1)]==types[i]);at+=record;
    }
    assert(at==length);
}
static void test_getdents_held_provider(void)
{
    static const struct vfs_backend_ops ops={.lookup=dirent_lookup,.retain=dirent_retain,
        .release=dirent_release,.same_node=dirent_same,.getattr=dirent_stat,.readdir=dirent_read_directory};
    struct vfs_namespace ns={0};struct vfs_mount *mount=NULL;struct storage_node root_node=dirent_node(100);
    assert(!vfs_mount_new(&ops,&dirent_base,NULL,&root_node,&mount));
    assert(!vfs_namespace_init(&ns,mount));vfs_mount_put(mount);
    struct vfs_path root={0},point={0},held={0};assert(!vfs_namespace_root(&ns,&root));
    assert(!vfs_path_lookup(&ns,&root,&root,"point",0,&point));root_node=dirent_node(200);
    assert(!vfs_mount_new(&ops,&dirent_mounted,NULL,&root_node,&mount));
    assert(!vfs_mount_attach(&ns,mount,&point));vfs_mount_put(mount);
    assert(!vfs_path_lookup(&ns,&root,&root,"point",0,&held));
    struct task *task=descriptor_audio_task(1902);descriptor_dispatch_task=task;
    const uint64_t calls[]={LINUX_SYS_GETDENTS,LINUX_SYS_GETDENTS64};
    for(unsigned wide=0;wide<2;++wide) {
        int fd=alloc_task_fd(task,vfs_path_node(&held),LINUX_O_RDONLY,"/obsolete/cover");assert(fd>=0);
        task_file_bind_path(task_file_for_fd(task,fd),&held);
        uint8_t storage[2049],*bytes=storage+1;const uint32_t capacity=2048; /* Linux permits unaligned output. */
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        int64_t length=syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity);
        assert(length>0);verify_dirent_records(bytes,(uint64_t)length,wide);task_release_syscall_file(task);
        /* A proc task may disappear after enumeration but before metadata.
         * Continue with later entries and their real cookies; a persistent
         * stale task must not hide every higher PID from ps/readdir. */
        struct task_file *raced=task_file_for_fd(task,fd);raced->offset=0;
        dirent_metadata_error=-ENOENT;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        length=syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity);
        assert(length>0 && raced->offset==8);
        verify_dirent_records(bytes,(uint64_t)length,wide);task_release_syscall_file(task);
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==0);
        task_release_syscall_file(task);
        /* Hard metadata failures still surface without consuming the entry. */
        raced->offset=2;dirent_metadata_error=-EIO;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==-EIO && raced->offset==2);
        task_release_syscall_file(task);dirent_metadata_error=0;
        assert(!task_begin_syscall_io(task,LINUX_SYS_LSEEK,fd,0));
        assert(syscall_seek_object(task,fd,3,RELIEFOS_SEEK_SET)==3);
        task_release_syscall_file(task);
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,24)==24);
        assert(!strcmp((const char *)bytes+(wide ? 19 : 18),"sock"));
        task_release_syscall_file(task);
        assert(!task_begin_syscall_io(task,LINUX_SYS_LSEEK,fd,0));
        assert(syscall_seek_object(task,fd,0,RELIEFOS_SEEK_CUR)==4);
        assert(syscall_seek_object(task,fd,-5,RELIEFOS_SEEK_CUR)==-EINVAL);
        task_release_syscall_file(task);
        struct task_file *file=task_file_for_fd(task,fd);file->offset=0;file->flags|=TASK_FILE_FLAG_PATH;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==-EBADF);
        task_release_syscall_file(task);file->flags&=~TASK_FILE_FLAG_PATH;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,1)==-EINVAL && !file->offset);
        task_release_syscall_file(task);dirent_bad_name=true;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==-EIO && !file->offset);
        task_release_syscall_file(task);dirent_bad_name=false;
        /* A metadata wait after dot records preserves their buffer prefix and
         * the original OFD through numeric close/reuse. */
        dirent_wait_metadata=true;assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==-EAGAIN);
        assert(task->regular_io.active && task->regular_io.done && file->offset==2);
        task_discard_file_fd(task,fd);struct task_file *replacement=NULL;
        assert(task_allocate_fd(task,fd,&replacement)==fd);replacement->flags=TASK_FILE_FLAG_DEV_NULL;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        length=syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity);
        assert(length>0);verify_dirent_records(bytes,(uint64_t)length,wide);task_release_syscall_file(task);
        assert(!replacement->offset);task_discard_file_fd(task,fd);
        fd=alloc_task_fd(task,vfs_path_node(&held),LINUX_O_RDONLY,"/obsolete/cover");assert(fd>=0);
        task_file_bind_path(task_file_for_fd(task,fd),&held);dirent_wait_metadata=true;
        assert(!task_begin_syscall_io(task,calls[wide],fd,0));
        assert(syscall_getdents_object(task,calls[wide],fd,(uintptr_t)bytes,capacity)==-EAGAIN);
        assert(dirent_waiters.count==1 && descriptor_dirent_io_pending);
        task_discard_file_fd(task,fd);task_release_syscall_file(task);
        assert(!dirent_waiters.count && !task->waiting_queue && !descriptor_dirent_io_pending);
    }
    struct vfs_path regular={0};assert(!vfs_path_lookup(&ns,&root,&root,"point/file",0,&regular));
    int fd=alloc_task_fd(task,vfs_path_node(&regular),LINUX_O_RDONLY,"/obsolete/file");assert(fd>=0);
    task_file_bind_path(task_file_for_fd(task,fd),&regular);vfs_path_put(&regular);
    assert(!task_begin_syscall_io(task,LINUX_SYS_LSEEK,fd,0));dirent_wait_metadata=true;
    struct task_file *original=task->syscall_file;
    assert(syscall_seek_object(task,fd,-1,RELIEFOS_SEEK_END)==-EAGAIN && original->io_owner==task->pid);
    struct task *other=descriptor_audio_task(1903);
    other->syscall_file=task_file_get(original);other->syscall_fd=fd;other->syscall_file_number=LINUX_SYS_LSEEK;
    assert(syscall_seek_object(other,fd,9,RELIEFOS_SEEK_SET)==-EAGAIN && !original->offset);
    task_release_syscall_file(other);clear_task_files(other);free(other);
    task_discard_file_fd(task,fd);struct task_file *replacement=NULL;
    assert(task_allocate_fd(task,fd,&replacement)==fd);replacement->flags=TASK_FILE_FLAG_DEV_NULL;
    assert(!task_begin_syscall_io(task,LINUX_SYS_LSEEK,fd,0));
    assert(syscall_seek_object(task,fd,-1,RELIEFOS_SEEK_END)==15 && original->offset==15 && !replacement->offset);
    assert(syscall_seek_object(task,fd,INT64_MAX,RELIEFOS_SEEK_CUR)==-EINVAL && original->offset==15);
    assert(syscall_seek_object(task,fd,8,3)==8 && syscall_seek_object(task,fd,8,4)==16);
    assert(syscall_seek_object(task,fd,16,3)==-ENXIO && syscall_seek_object(task,fd,16,4)==-ENXIO);
    task_release_syscall_file(task);task_discard_file_fd(task,fd);
    clear_task_files(task);free(task);descriptor_dispatch_task=NULL;
    assert(!vfs_mount_unmount(&ns,&held));vfs_path_put(&held);vfs_path_put(&point);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns) && !dirent_base.pins && !dirent_mounted.pins);
    puts("PASS getdents actual inode/type, mount-root dot-parent, old/new unaligned ABI, opaque seek cookies, retained metadata/positions across fd reuse and cancellation");
}

static void test_vector_held_provider(void)
{
    const struct {uint64_t number;bool writing,positional,current;} cases[]={
        {LINUX_SYS_READV,false,false,false},{LINUX_SYS_WRITEV,true,false,false},
        {LINUX_SYS_PREADV,false,true,false},{LINUX_SYS_PWRITEV,true,true,false},
        {LINUX_SYS_PREADV2,false,true,false},{LINUX_SYS_PWRITEV2,true,true,false},
        {LINUX_SYS_PREADV2,false,true,true},{LINUX_SYS_PWRITEV2,true,true,true}};
    for(unsigned wait=0;wait<2;++wait)for(unsigned which=0;which<sizeof(cases)/sizeof(cases[0]);++which) {
        struct task *task=descriptor_audio_task(1950+which);descriptor_dispatch_task=task;
        memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"abcdef",6);transfer_files[0].size=6;
        transfer_reads=transfer_writes=0;vector_wait_status=wait ? KERNEL_SYSCALL_BLOCKED : -EAGAIN;vector_wait_partial=wait;
        struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
        assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));
        int fd=transfer_open(task,mount,cases[which].writing ? 1 : 0);
        assert(!task_begin_syscall_io(task,cases[which].number,fd,0));
        struct task_file *original=task->syscall_file;
        original->offset=cases[which].positional && !cases[which].current ? 9 : 0;
        char left[4]="abc",right[4]="def";
        if(!cases[which].writing){memset(left,0,sizeof(left));memset(right,0,sizeof(right));}
        struct iovec vectors[2]={{left,3},{right,3}};
        uint64_t low=cases[which].current ? UINT64_MAX : 0,high=UINT64_C(0x9876543210abcdef);
        if(cases[which].writing)vector_wait_write_at=2;else vector_wait_read_at=2;
        assert(syscall_vector_io(task,cases[which].number,fd,(uintptr_t)vectors,2,low,high,0)==vector_wait_status);
        assert(task_vector_progress(task)==3+wait);
        assert(transfer_waiters.count==1 && original->io_owner==task->pid);
        task_discard_file_fd(task,fd);int replacement=transfer_open(task,mount,3);assert(replacement==fd);
        vectors[0]=(struct iovec){(void *)(uintptr_t)1,SIZE_MAX};vectors[1]=vectors[0];
        if(cases[which].writing){memcpy(left,"xxx",3);if(wait)right[0]='Z';}
        else memset(transfer_files[0].bytes,'X',3+wait); /* Accepted prefix must not replay. */
        kernel_wait_queue_remove(&transfer_waiters,task);
        struct trap_frame retry={.rax=cases[which].number,.rdi=(uint32_t)fd,.rsi=(uintptr_t)vectors,.rdx=2,
            .r10=low,.r8=high^UINT64_MAX};
        task_vector_enter(task,&retry); /* x86_64 consumes no pos_h bits. */
        assert(task->vector_io && task->syscall_file==original);
        assert(!task_begin_syscall_io(task,cases[which].number,fd,0));
        assert(syscall_vector_io(task,cases[which].number,fd,(uintptr_t)vectors,2,low,high,0)==6);
        if(cases[which].writing)assert(transfer_files[1].size==6 && !memcmp(transfer_files[1].bytes,"abcdef",6));
        else assert(!memcmp(left,"abc",3) && !memcmp(right,"def",3));
        assert(original->offset==(cases[which].positional && !cases[which].current ? 9 : 6));
        assert(!task_file_for_fd(task,fd)->offset && !transfer_files[3].size);
        task_release_syscall_file(task);task_discard_file_fd(task,fd);clear_task_files(task);free(task);
        descriptor_dispatch_task=NULL;vfs_mount_put(mount);assert(!transfer_provider_pins);
    }
    vector_wait_status=-EAGAIN;vector_wait_partial=0;
    for(unsigned allocation_failure=0;allocation_failure<2;++allocation_failure) {
        struct task *task=descriptor_audio_task(1970+allocation_failure);descriptor_dispatch_task=task;
        memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"abcdef",6);transfer_files[0].size=6;
        transfer_reads=transfer_writes=0;struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
        assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));int fd=transfer_open(task,mount,0);
        assert(!task_begin_syscall_io(task,LINUX_SYS_READV,fd,0));char left[3],right[3];struct iovec vectors[]={{left,3},{right,3}};
        if(allocation_failure) {
            fail_allocation=1;
            assert(syscall_vector_io(task,LINUX_SYS_READV,fd,(uintptr_t)vectors,2,0,0,0)==-ENOMEM);
            fail_allocation=0;assert(!task->vector_io && !transfer_reads && !transfer_waiters.count);
        } else {
            vector_wait_read_at=2;
            assert(syscall_vector_io(task,LINUX_SYS_READV,fd,(uintptr_t)vectors,2,0,0,0)==-EAGAIN);
            task_discard_file_fd(task,fd);assert(transfer_open(task,mount,3)==fd);
            struct trap_frame changed={.rax=LINUX_SYS_READV,.rdi=(uint32_t)fd,.rsi=(uintptr_t)(vectors+1),.rdx=1};
            task_vector_enter(task,&changed);
            assert(!task->vector_io && !task->syscall_file && !task->waiting_queue && !transfer_waiters.count);
            assert(transfer_provider_pins==2); /* Mount and replacement only. */
        }
        task_release_syscall_file(task);clear_task_files(task);free(task);descriptor_dispatch_task=NULL;
        vfs_mount_put(mount);assert(!transfer_provider_pins);
    }
    puts("PASS ordinary vectors retain accepted prefixes/OFD/imported iovecs through EAGAIN/parked partial I/O, fd reuse, positional variants, cancellation and OOM");
}

static void test_vector_native_position(void)
{
    const struct {uint64_t number;bool writing;} cases[]={
        {LINUX_SYS_PREADV,false},{LINUX_SYS_PWRITEV,true},
        {LINUX_SYS_PREADV2,false},{LINUX_SYS_PWRITEV2,true}};
    for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        struct task *task=descriptor_audio_task(1980+i);descriptor_dispatch_task=task;
        transfer_origin=UINT64_C(0x100000007);
        memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"abcdef",6);transfer_files[0].size=6;
        struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
        assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));
        int fd=transfer_open(task,mount,cases[i].writing ? 1 : 0);
        assert(!task_begin_syscall_io(task,cases[i].number,fd,0));
        task->syscall_file->offset=21;char bytes[6]="abcdef";
        if(!cases[i].writing)memset(bytes,0,sizeof(bytes));
        struct iovec vectors[]={{bytes,2},{bytes+2,4}};
        assert(syscall_vector_io(task,cases[i].number,fd,(uintptr_t)vectors,2,
            UINT64_C(0x100000007),UINT64_C(0xfedcba9876543210),UINT64_C(0x100000000))==6);
        assert(task->syscall_file->offset==21 && !memcmp(bytes,"abcdef",6));
        if(cases[i].writing)assert(transfer_files[1].size==6 && !memcmp(transfer_files[1].bytes,"abcdef",6));
        task_release_syscall_file(task);clear_task_files(task);free(task);descriptor_dispatch_task=NULL;
        vfs_mount_put(mount);assert(!transfer_provider_pins);transfer_origin=0;
    }
    /* Same low32 position but a changed high32 is a new operation, even when
     * the number, FD and iov pointer match a parked request. */
    struct task *task=descriptor_audio_task(1985);descriptor_dispatch_task=task;
    memset(transfer_files,0,sizeof(transfer_files));memcpy(transfer_files[0].bytes,"abcdef",6);transfer_files[0].size=6;
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&transfer_ops,transfer_files,NULL,&root,&mount));int fd=transfer_open(task,mount,0);
    assert(!task_begin_syscall_io(task,LINUX_SYS_PREADV,fd,0));char bytes[6];struct iovec vectors[]={{bytes,3},{bytes+3,3}};
    transfer_reads=0;vector_wait_read_at=2;
    assert(syscall_vector_io(task,LINUX_SYS_PREADV,fd,(uintptr_t)vectors,2,0,0,0)==-EAGAIN);
    struct trap_frame changed={.rax=LINUX_SYS_PREADV,.rdi=(uint32_t)fd,.rsi=(uintptr_t)vectors,.rdx=2,.r10=UINT64_C(0x100000000)};
    task_vector_enter(task,&changed);
    assert(!task->vector_io && !task->syscall_file && !task->waiting_queue && !transfer_waiters.count);
    clear_task_files(task);free(task);descriptor_dispatch_task=NULL;vfs_mount_put(mount);assert(!transfer_provider_pins);
    puts("PASS native x86_64 vector full64 position, ignored pos_h/flags high bits and changed-position cancellation");
}

static void test_block_description_lifetime(void)
{
    struct task *task=calloc(1,sizeof(*task));assert(task);
    sched_task_limits(task)->nofile.rlim_cur=64;
    struct storage_node node={.type=RELIEFOS_FS_TYPE_DEVICE,.flags=STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_DEV_BLOCK,
        .first_cluster=STORAGE_DEV_KIND_DISK,.volume_id=STORAGE_BLOCK_VOLUME_ID(0,-1)};
    int fd=alloc_task_fd(task,&node,TASK_FILE_FLAG_DEV_NODE|TASK_FILE_FLAG_DEV_BLOCK,"block-owner");assert(fd>=0);
    struct task_file *file=task_file_for_fd(task,fd);assert(!storage_disk_block_get(0,&file->block_ref));
    struct task_file alias={0};assert(!task_file_reference(&alias,file) && fixture_block_refs==1);
    assert(file->description && !file->block_ref.device && task_file_description(file)->block_ref.device);
    clear_task_file(file);assert(fixture_block_refs==1);
    clear_task_file(&alias);assert(!fixture_block_refs);
    sched_task_file_release(task);free(task);
    puts("PASS block OFD ownership moves on promotion, survives dup, releases once on final close");
}

int main(void)
{
    assert(!audio_character_init());
    assert(!drm_character_init());
    test_block_description_lifetime();
    test_vector_held_provider();
    test_vector_native_position();
    test_getdents_held_provider();
    test_transfer_blocked_stream();
    test_transfer_held_providers();
    test_ofd_owned_mount_path();
    test_busy_module_unload_dispatch();
    test_module_init_error_dispatch();
    test_finit_module_retry_description();
    test_fd_metadata_retry_description();
    test_mmap_retry_description();
    test_standard_character_stat();
    test_materialized_console_permissions();
    test_finit_preflight_before_allocation();
    test_finit_preflight_retires_other_io();
    test_regular_inode_writer_ofd_lifetime();
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
    struct linux_stat_abi sound_stat={.st_mode=LINUX_S_IFCHR};
    devfs_stat_identity(&sound_stat,&sound_node);
    assert(sound_stat.st_rdev == ((116u << 8) | 63u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_CONTROL;
    sound_node.volume_id = stat_cards[1].id;
    devfs_stat_identity(&sound_stat,&sound_node);
    assert(sound_stat.st_rdev == ((116u << 8) | 32u));
    sound_node.flags = STORAGE_NODE_FLAG_AUDIO_MIXER;
    devfs_stat_identity(&sound_stat,&sound_node);
    assert(sound_stat.st_rdev == ((14u << 8) | 16u));
    sound_node.flags = STORAGE_NODE_FLAG_DEV_NODE;
    sound_node.first_cluster = STORAGE_DEV_KIND_AUDIO;
    devfs_stat_identity(&sound_stat,&sound_node);
    assert(sound_stat.st_rdev == ((14u << 8) | 3u));
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
    struct vfs_namespace anonymous_ns={0};struct vfs_path anonymous_root={0};struct linux_stat_abi anonymous_stat;
    writer_namespace_open(&anonymous_ns,task,&anonymous_root);
    assert(!syscall_metadata(&anonymous_ns,task,LINUX_SYS_NEWFSTATAT,3,(uintptr_t)"",(uintptr_t)&anonymous_stat,LINUX_AT_EMPTY_PATH,0));
    assert((anonymous_stat.st_mode&LINUX_S_IFMT)==LINUX_S_IFIFO);
    assert(syscall_metadata(&anonymous_ns,task,LINUX_SYS_NEWFSTATAT,3,(uintptr_t)"entry",(uintptr_t)&anonymous_stat,LINUX_AT_EMPTY_PATH,0)==-RELIEFOS_ENOTDIR);
    assert(syscall_metadata(&anonymous_ns,task,LINUX_SYS_NEWFSTATAT,3,(uintptr_t)"",(uintptr_t)&anonymous_stat,0,0)==-RELIEFOS_ENOENT);
    assert(syscall_metadata(&anonymous_ns,task,LINUX_SYS_NEWFSTATAT,(uint64_t)-1,(uintptr_t)"",(uintptr_t)&anonymous_stat,LINUX_AT_EMPTY_PATH,0)==-RELIEFOS_EBADF);
    writer_namespace_close(&anonymous_ns,task,&anonymous_root);
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
    drm_character_exit();
    audio_character_exit();
    puts("PASS actual fd table: growth, shared OFD, failure rollback, stdio reuse, limit lowering");
}

/* Real counted UNIX address parsing runs in this fixture. Only filesystem
 * endpoint resolution is outside its scope and must abort if ever reached;
 * actual inode/DAC/bind cases live in mount_backend_test and the native probe. */
int syscall_unix_path(struct vfs_namespace *ns,struct task *task,const char *name,
                          bool create,struct vfs_path *out)
{ (void)ns;(void)task;(void)name;(void)create;(void)out;abort(); }
