#include "../../drivers/video/drm/memory.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "../../kernel/params/internal.h"

#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/wait.h>
#include <reliefnt/drm.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <linux/fcntl.h>
#include <linux/mman.h>
#include <linux/poll.h>
#include <reliefnt/console.h>
#include <reliefnt/mm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/paging.h>
#include <drm/drm_mode.h>
#include "../../drivers/video/drm/drm_device.c"
#include "../../drivers/video/drm/drm_faildiag.c"
#include "../../drivers/video/drm/drm_file.c"
#include "../../kernel/device/character.c"
#include "../../drivers/video/drm/event_io.c"
#include "../../drivers/video/drm/file_ops.c"
#include "../../drivers/video/drm/mapping.c"
#include "../../drivers/video/drm/drm_ioctl.c"
#include "../../drivers/video/drm/drm_kms.c"
#include "../../drivers/video/drm/drm_bo.c"
#include "../../drivers/video/drm/drm_prime.c"
#include "../../drivers/video/drm/virtio/internal.h"
#ifdef RELIEFNT_DRM_NATIVE_OWNER_TEST
#include "../../kernel/module/internal.h"
#include "../../drivers/video/drm/memory.h"
struct module drm_test_native_owner;
void module_memory_free(struct module_memory *memory){assert(!memory);}
#endif

extern const struct module_parameter __start___param[];
extern const struct module_parameter __stop___param[];

static unsigned allocations;
static unsigned drm_test_permanent_allocations;
static char drm_test_diag_log_buffer[16384];
static unsigned drm_test_diag_log_calls;
static int fail_kernel_malloc_after = -1;
static unsigned wait_queue_add_calls;
static unsigned block_current_calls;
static unsigned sleep_current_calls;
static uint64_t mm_pages_allocated;
static uint64_t mm_pages_freed;
static bool mm_allocation_fails;
static uint64_t fail_copyout_address;

void console_log(enum console_log_level level, const char *format, ...)
{
    va_list arguments;
    size_t used = strlen(drm_test_diag_log_buffer);
    (void)level;
    va_start(arguments, format);
    if (used < sizeof(drm_test_diag_log_buffer))
        (void)vsnprintf(drm_test_diag_log_buffer + used,
                        sizeof(drm_test_diag_log_buffer) - used,
                        format, arguments);
    va_end(arguments);
    ++drm_test_diag_log_calls;
}

static void drm_test_diag_log_reset(void)
{
    drm_test_diag_log_buffer[0] = 0;
    drm_test_diag_log_calls = 0;
}

void *paging_kernel_direct_map(uint64_t physical)
{
    return (void *)(uintptr_t)physical;
}

void *kernel_malloc(size_t size)
{
    if (fail_kernel_malloc_after == 0) return NULL;
    if (fail_kernel_malloc_after > 0) --fail_kernel_malloc_after;
    void *value = malloc(size);
    if (value) ++allocations;
    return value;
}

void kernel_free(void *value)
{
    if (value) {
        assert(allocations > 0);
        --allocations;
        free(value);
    }
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{
    if (flags) *flags = 0;
    while (__atomic_exchange_n(&lock->state, 1u, __ATOMIC_ACQUIRE)) { }
}

void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{
    (void)flags;
    __atomic_store_n(&lock->state, 0u, __ATOMIC_RELEASE);
}

void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{
    memset(queue, 0, sizeof(*queue));
}

void kernel_wait_queue_block_current(struct kernel_wait_queue *queue)
{
    (void)queue;
}

int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{
    (void)queue;
    (void)task;
    ++wait_queue_add_calls;
    return 0;
}

void sched_block_current(void) { ++block_current_calls; }
void sched_sleep_current_until(uint64_t wake_tick) { (void)wake_tick; ++sleep_current_calls; }

uint64_t time_ticks(void)
{
    return 0;
}

uint64_t mm_alloc_pages(uint32_t page_count)
{
    if (!page_count || mm_allocation_fails) return 0;
    size_t bytes = (size_t)page_count * 4096u;
    void *memory = aligned_alloc(4096u, bytes);
    if (!memory) return 0;
    memset(memory, 0, bytes);
    mm_pages_allocated += page_count;
    return (uint64_t)(uintptr_t)memory;
}

void mm_free_pages(uint64_t physical, uint32_t page_count)
{
    assert(physical && page_count);
    assert(!(physical & 4095u));
    mm_pages_freed += page_count;
    free((void *)(uintptr_t)physical);
}

bool user_range_ok(uint64_t address, uint64_t length)
{
    return length == 0 || (address >= 4096 && address <= UINTPTR_MAX - length);
}

bool user_range_writable(uint64_t address, uint64_t length)
{
    return user_range_ok(address, length);
}

int user_copy_to_task(struct task *task, uint64_t address,
                      const void *source, uint64_t length)
{
    (void)task;
    if (address == fail_copyout_address) return -LINUX_EFAULT;
    if (!user_range_writable(address, length)) return -LINUX_EFAULT;
    if (length) memcpy((void *)(uintptr_t)address, source, (size_t)length);
    return 0;
}

int user_copy_from_task(struct task *task, void *destination, uint64_t address,
                        uint64_t length)
{
    (void)task;
    if (!user_range_ok(address, length)) return -LINUX_EFAULT;
    if (length) memcpy(destination, (const void *)(uintptr_t)address, (size_t)length);
    return 0;
}

static struct task make_task(uint32_t tgid, bool admin)
{
    return (struct task){
        .pid = tgid,
        .tgid = tgid,
        .cap_effective = admin ? (1ULL << CAP_SYS_ADMIN) : 0,
    };
}

#define DRM_TEST_FD_GROUPS 16u
#define DRM_TEST_FD_SLOTS 16u
#define DRM_TEST_OFD_SLOTS 64u
static struct task_file drm_test_fd_slots[DRM_TEST_FD_GROUPS][DRM_TEST_FD_SLOTS];
static struct task_file drm_test_ofd_slots[DRM_TEST_OFD_SLOTS];

static uint32_t drm_test_fd_group(const struct task *task)
{
    return task ? task->pid % DRM_TEST_FD_GROUPS : 0;
}

struct task_file *task_file_for_fd(struct task *task, int fd)
{
    if (!task || fd < 0 || fd >= (int)DRM_TEST_FD_SLOTS) return NULL;
    struct task_file *slot = &drm_test_fd_slots[drm_test_fd_group(task)][fd];
    return slot->used ? task_file_description(slot) : NULL;
}

struct task_file *task_file_for_io(struct task *task, int fd)
{
    return task_file_for_fd(task, fd);
}

int task_allocate_fd(struct task *task, int minimum, struct task_file **out)
{
    if (!task || !out || minimum < 0) return -LINUX_EINVAL;
    uint32_t group = drm_test_fd_group(task);
    for (int fd = minimum; fd < (int)DRM_TEST_FD_SLOTS; ++fd) {
        struct task_file *slot = &drm_test_fd_slots[group][fd];
        if (slot->used) continue;
        *slot = (struct task_file){.used = 1};
        *out = slot;
        return fd;
    }
    return -LINUX_EMFILE;
}

static struct task_file *drm_test_promote_file(struct task_file *source)
{
    if (!source || !source->used) return NULL;
    if (source->description) return source->description;
    if (source->references) return source;
    for (uint32_t i = 0; i < DRM_TEST_OFD_SLOTS; ++i) {
        struct task_file *description = &drm_test_ofd_slots[i];
        if (description->used) continue;
        *description = *source;
        description->used = 1;
        description->references = 1;
        description->fd_flags = 0;
        source->description = description;
        return description;
    }
    return NULL;
}

int task_file_reference(struct task_file *destination, struct task_file *source)
{
    if (!destination) return -LINUX_EINVAL;
    struct task_file *description = drm_test_promote_file(source);
    if (!description) return -LINUX_EBADF;
    *destination = (struct task_file){
        .used = 1, .description = description,
    };
    ++description->references;
    return 0;
}

struct task_file *task_file_get(struct task_file *source)
{
    struct task_file *description = drm_test_promote_file(source);
    if (description) ++description->references;
    return description;
}

static void drm_test_ofd_release(struct task_file *description)
{
    struct character_file *handle=description->character_file;description->character_file=NULL;
    character_file_close(handle,description);
    *description = (struct task_file){0};
}

void task_file_put(struct task_file *description)
{
    if (description && description->references &&
        --description->references == 0)
        drm_test_ofd_release(description);
}

void task_discard_file_fd(struct task *task, int fd)
{
    if (!task || fd < 0 || fd >= (int)DRM_TEST_FD_SLOTS) return;
    struct task_file *slot = &drm_test_fd_slots[drm_test_fd_group(task)][fd];
    if (!slot->used) return;
    if (slot->description) {
        struct task_file *description = slot->description;
        *slot = (struct task_file){0};
        if (description->references && --description->references == 0)
            drm_test_ofd_release(description);
    } else {
        struct character_file *handle=slot->character_file;slot->character_file=NULL;
        character_file_close(handle,slot);
        *slot = (struct task_file){0};
    }
}

int file_can_read(const struct task_file *file)
{
    return file && (file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_WRONLY;
}

int file_can_write(const struct task_file *file)
{
    return file && (file->flags & RELIEFOS_O_ACCMODE) != RELIEFOS_O_RDONLY;
}

uint64_t mm_free_memory_kib(void)
{
    return 12345u;
}

static struct pci_device fixture_device(void)
{
    return (struct pci_device){
        .bus = 0,
        .slot = 15,
        .function = 0,
        .vendor_id = 0x1234,
        .device_id = 0x1111,
    };
}

static int ioctl_call(struct task *task, struct task_file *file,
                      uint32_t command, void *argument)
{
    return (int)drm_file_ioctl(task, file, command,
                               (uint64_t)(uintptr_t)argument);
}

static void test_registration_and_open_lifetime(void)
{
    struct task unprivileged = make_task(100, false);
    struct task stranger = make_task(201, false);
    struct task admin = make_task(200, true);
    struct task_file first = {0}, second = {0}, third = {0};
    struct pci_device identity;
    assert(drm_device_register(NULL) == -LINUX_EINVAL);
    assert(!drm_device_is_registered());
    assert(drm_file_open(&unprivileged, &first) == -LINUX_ENODEV);
    assert(!first.drm_file && allocations == drm_test_permanent_allocations);

    struct pci_device pci = fixture_device();
    assert(drm_device_register(&pci) == 0);
    assert(drm_device_register(&pci) == 0);
    struct pci_device other = pci;
    ++other.slot;
    assert(drm_device_register(&other) == -LINUX_EBUSY);
    assert(!drm_device_render_node_ready());
    assert(drm_device_pci_identity(&identity) == 0);
    assert(identity.bus == 0 && identity.slot == 15 && identity.function == 0);
    assert(drm_file_open(&unprivileged, &first) == 0);
    assert(first.drm_file && first.drm_file->is_master);
    assert(first.drm_file->authenticated);
    assert(drm_file_open(&admin, &second) == 0);
    assert(second.drm_file && !second.drm_file->is_master);
    struct drm_auth second_magic = {0};
    assert(ioctl_call(&admin, &second, DRM_IOCTL_GET_MAGIC, &second_magic) == 0);
    assert(second_magic.magic && first.drm_file->master->magics);
    assert(drm_file_set_master(&unprivileged, second.drm_file) == -LINUX_EACCES);
    assert(drm_file_set_master(&admin, second.drm_file) == -LINUX_EBUSY);
    assert(drm_file_drop_master(&stranger, first.drm_file) == -LINUX_EACCES);
    assert(drm_file_drop_master(&unprivileged, first.drm_file) == 0);
    second.drm_file->authenticated = false;
    assert(drm_file_set_master(&admin, second.drm_file) == 0);
    assert(second.drm_file->authenticated);
    assert(!first.drm_file->master->magics);
    drm_file_close(&first);
    assert(drm_device_get()->master == second.drm_file->master);
    drm_file_close(&second);
    assert(!drm_device_get()->master);
    assert(drm_file_open(&admin, &third) == 0 && third.drm_file->is_master);
    drm_file_close(&third);
    assert(allocations == drm_test_permanent_allocations);
}

static void test_core_abi_and_validation(void)
{
    struct task master_task = make_task(300, true);
    struct task client_task = make_task(301, false);
    struct task_file master = {0}, client = {0};
    char name[4] = {0}, date[1] = {0}, desc[8] = {0};
    struct drm_version version = {
        .name_len = sizeof(name), .name = name,
        .date_len = sizeof(date), .date = date,
        .desc_len = sizeof(desc), .desc = desc,
    };
    struct drm_get_cap cap = {.capability = DRM_CAP_TIMESTAMP_MONOTONIC};
    struct drm_set_client_cap client_cap = {
        .capability = DRM_CLIENT_CAP_UNIVERSAL_PLANES, .value = 1,
    };
    struct drm_client client_info = {0};
    struct drm_stats stats = {0};
    struct drm_set_version set_version = {
        .drm_di_major = 1, .drm_di_minor = 4,
        .drm_dd_major = 0, .drm_dd_minor = 1,
    };

    assert(drm_file_open(&master_task, &master) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, &version) == 0);
    assert(version.version_major == 0 && version.version_minor == 1 &&
           version.version_patchlevel == 0);
    assert(version.name_len == strlen("reliefnt") && !memcmp(name, "reli", 4));
    assert(version.date_len == 1 && date[0] == '0');
    assert(version.desc_len == strlen("ReliefNT DRM core"));

    assert(ioctl_call(&master_task, &master, DRM_IOCTL_SET_VERSION,
                      &set_version) == 0);
    assert(set_version.drm_di_major == 1 && set_version.drm_di_minor == 4);
    assert(set_version.drm_dd_major == 0 && set_version.drm_dd_minor == 1);
    struct drm_unique unique = {.unique_len = 0, .unique = NULL};
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    static const char expected_busid[] = "pci:0000:00:0f.0";
    assert(unique.unique_len == sizeof(expected_busid) - 1);
    char busid[sizeof(expected_busid)] = {0};
    unique.unique = busid;
    unique.unique_len = sizeof(busid) - 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    assert(!memcmp(busid, expected_busid, sizeof(expected_busid)));
    char short_busid[4] = {'x', 'x', 'x', 'x'};
    unique.unique = short_busid;
    unique.unique_len = sizeof(short_busid) - 1;
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_GET_UNIQUE, &unique) == 0);
    assert(unique.unique_len == sizeof(expected_busid) - 1);
    assert(!memcmp(short_busid, "xxxx", sizeof(short_busid)));

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 1);
    cap.capability = DRM_CAP_PRIME;
    cap.value = UINT64_MAX;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT));
    cap.capability = DRM_CAP_DUMB_BUFFER;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CAP, &cap) == 0 &&
           cap.value == 1);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_CLIENT_CAP,
                      &client_cap) == -LINUX_EOPNOTSUPP);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_VERSION,
                      (void *)(uintptr_t)1) == -LINUX_EFAULT);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_SET_VERSION,
                      &set_version) == -LINUX_EACCES);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_CLIENT,
                      &client_info) == -LINUX_EOPNOTSUPP);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_STATS,
                      &stats) == -LINUX_EOPNOTSUPP);

    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, NULL) == -LINUX_EFAULT);
    version.name_len = 1;
    version.name = (char *)(uintptr_t)1;
    version.date_len = 0;
    version.desc_len = 0;
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION, &version) == -LINUX_EFAULT);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_VERSION ^ 0x100u,
                      &version) == -LINUX_ENOTTY);
    assert(ioctl_call(&client_task, &client, _IO('d', 0xff), &version) == -LINUX_EINVAL);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_SET_UNIQUE,
                      &(struct drm_unique){0}) == -LINUX_EINVAL);

    drm_file_close(&client);
    drm_file_close(&master);
    assert(!drm_device_get()->master && allocations == drm_test_permanent_allocations);
}

static void test_legacy_magic_and_empty_readiness(void)
{
    struct task master_task = make_task(400, true);
    struct task client_task = make_task(401, false);
    struct task_file master = {0}, client = {0};
    struct drm_auth auth = {0};
    char byte;

    assert(drm_file_open(&master_task, &master) == 0);
    assert(drm_file_open(&client_task, &client) == 0);
    assert(ioctl_call(&client_task, &client, DRM_IOCTL_GET_MAGIC, &auth) == 0);
    assert(auth.magic != 0 && !client.drm_file->authenticated);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_AUTH_MAGIC, &auth) == 0);
    assert(client.drm_file->authenticated);
    assert(ioctl_call(&master_task, &master, DRM_IOCTL_AUTH_MAGIC, &auth) == -LINUX_EINVAL);
    assert(drm_file_read(&client_task, &client, &byte, 0) == 0);
    assert(drm_file_poll(&client, 0xffffu) == 0);
    client.flags |= RELIEFOS_O_NONBLOCK;
    assert(drm_file_read(&client_task, &client, &byte, 1) == -LINUX_EAGAIN);
    client.flags &= ~RELIEFOS_O_NONBLOCK;
    assert(drm_file_read(&client_task, &client, &byte, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(wait_queue_add_calls == 1 && block_current_calls == 1);
    assert(drm_file_read(&client_task, &client, &byte, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(wait_queue_add_calls == 2 && block_current_calls == 2);
    assert(sleep_current_calls == 0);

    drm_file_close(&client);
    assert(master.drm_file->master->magics == NULL);
    drm_file_close(&master);
    assert(allocations == drm_test_permanent_allocations);
}

static void test_dumb_bo_contract(void)
{
    struct task owner_task = make_task(500, false);
    struct task foreign_task = make_task(501, false);
    struct task_file owner = {0}, foreign = {0};
    struct drm_get_cap cap = {.capability = DRM_CAP_DUMB_BUFFER};
    struct drm_mode_create_dumb create = {
        .height = 100, .width = 17, .bpp = 32, .flags = 0,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    struct drm_mode_map_dumb map = {0};
    struct drm_gem_close gem_close = {0};
    assert(drm_file_open(&owner_task, &owner) == 0);
    assert(drm_file_open(&foreign_task, &foreign) == 0);
    assert(owner.drm_file->authenticated);
    assert(!foreign.drm_file->authenticated);

    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 1);
    cap.capability = DRM_CAP_DUMB_PREFERRED_DEPTH;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 32);
    cap.capability = DRM_CAP_DUMB_PREFER_SHADOW;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_GET_CAP, &cap) == 0);
    assert(cap.value == 0);

    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &create) == 0);
    assert(create.handle && create.pitch == create.width * 4u);
    assert(create.size >= (uint64_t)create.pitch * create.height);
    assert((create.size & 4095u) == 0);

    map.handle = create.handle;
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_MODE_MAP_DUMB,
                      &map) == -LINUX_EINVAL);
    assert(ioctl_call(&foreign_task, &foreign, DRM_IOCTL_MODE_DESTROY_DUMB,
                      &(struct drm_mode_destroy_dumb){.handle = create.handle}) ==
           -LINUX_EINVAL);
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_MAP_DUMB, &map) == 0);
    assert(map.offset && !(map.offset & 4095u));

    struct drm_mode_create_dumb invalid = {
        .height = 1, .width = 1, .bpp = 24, .flags = 0,
        .handle = UINT32_MAX, .pitch = UINT32_MAX, .size = UINT64_MAX,
    };
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &invalid) == -LINUX_EINVAL);
    assert(!invalid.handle && !invalid.pitch && !invalid.size);

    struct drm_bo_handle *handles_before = owner.drm_file->bo_handles;
    const uint64_t pages_before = mm_pages_allocated;
    const uint64_t pages_freed_before = mm_pages_freed;
    struct drm_mode_create_dumb copyout_failure = {
        .height = 2, .width = 1024, .bpp = 32,
    };
    fail_copyout_address = (uint64_t)(uintptr_t)&copyout_failure;
    assert(ioctl_call(&owner_task, &owner, DRM_IOCTL_MODE_CREATE_DUMB,
                      &copyout_failure) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(owner.drm_file->bo_handles == handles_before);
    assert(mm_pages_allocated - pages_before ==
           mm_pages_freed - pages_freed_before);

    struct drm_bo *mapped_bo = NULL, *other_bo = NULL;
    uint64_t bo_offset = UINT64_MAX;
    assert(!drm_file_mmap_bo(&owner, map.offset, 8192, &mapped_bo, &bo_offset));
    assert(mapped_bo && bo_offset == 0 && drm_bo_size(mapped_bo) == 8192);
    for (uint64_t offset = 0; offset < 8192; offset += 4096) {
        const uint8_t *page = (void *)(uintptr_t)drm_bo_page_physical(mapped_bo, offset);
        for (unsigned i = 0; i < 4096; ++i) assert(!page[i]);
    }
    assert(drm_file_mmap_bo(&foreign, map.offset, 4096, &other_bo, &bo_offset) == -LINUX_EINVAL);
    assert(!drm_file_mmap_bo(&owner, map.offset + 4096, 4096, &other_bo, &bo_offset));
    assert(other_bo == mapped_bo && bo_offset == 4096);
    drm_bo_put(other_bo);
    assert(drm_file_mmap_bo(&owner, UINT64_MAX - 4095, 4096, &other_bo, &bo_offset) == -LINUX_EINVAL);

    gem_close.handle = create.handle;
    assert(!ioctl_call(&owner_task, &owner, DRM_IOCTL_GEM_CLOSE, &gem_close));
    assert(drm_file_mmap_bo(&owner, map.offset, 4096, &other_bo, &bo_offset) == -LINUX_EINVAL);
    assert(drm_bo_page_physical(mapped_bo, 0));
    drm_bo_put(mapped_bo);
    drm_file_close(&foreign);
    drm_file_close(&owner);
    assert(allocations == drm_test_permanent_allocations);
}

static void test_prime_bo_lifetime(void)
{
    struct task producer = make_task(550, false), consumer = make_task(551, false);
    struct task_file card = {0}, importer = {0};
    assert(!drm_file_open(&producer, &card));
    assert(!drm_file_open(&consumer, &importer));
    struct drm_mode_create_dumb dumb = {.width = 64, .height = 32, .bpp = 32};
    assert(!ioctl_call(&producer, &card, DRM_IOCTL_MODE_CREATE_DUMB, &dumb));

    struct drm_prime_handle export = {.handle = dumb.handle, .flags = LINUX_O_CLOEXEC | LINUX_O_RDWR};
    assert(ioctl_call(&producer, &card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export) == -LINUX_ENOENT);
    assert(!task_file_for_fd(&producer, 0));

    /* Model a VirtIO-backed BO without starting the transport. The core only
     * checks the presence of the resource when admitting a PRIME export. */
    struct vg_resource resource = {.id = 1, .width = dumb.width, .height = dumb.height};
    struct drm_bo *backed = drm_bo_lookup_handle_get(card.drm_file, dumb.handle);
    assert(backed);
    drm_bo_set_virtio(backed, &resource);
    drm_bo_put(backed);

    fail_copyout_address = (uintptr_t)&export;
    assert(ioctl_call(&producer, &card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(!task_file_for_fd(&producer, 0));
    assert(!ioctl_call(&producer, &card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &export));
    struct task_file *description = task_file_for_fd(&producer, export.fd);
    assert(description && description->drm_prime_file);
    struct drm_prime_handle duplicate = {.handle = dumb.handle, .flags = LINUX_O_RDWR};
    assert(!ioctl_call(&producer, &card, DRM_IOCTL_PRIME_HANDLE_TO_FD, &duplicate));
    assert(duplicate.fd != export.fd && task_file_for_fd(&producer, duplicate.fd) == description);

    struct task_file *slot = NULL;
    int fd = task_allocate_fd(&consumer, 0, &slot);
    assert(fd >= 0 && !task_file_reference(slot, description));
    struct drm_prime_handle import = {.fd = fd};
    fail_copyout_address = (uintptr_t)&import;
    assert(ioctl_call(&consumer, &importer, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import) == -LINUX_EFAULT);
    fail_copyout_address = 0;
    assert(!importer.drm_file->bo_handles);
    assert(!ioctl_call(&consumer, &importer, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import));
    uint32_t handle = import.handle;
    assert(handle && !ioctl_call(&consumer, &importer, DRM_IOCTL_PRIME_FD_TO_HANDLE, &import));
    assert(import.handle == handle);
    struct drm_bo *bo = NULL;
    assert(!drm_prime_mmap_bo(description, 4096, 4096, &bo));
    assert(bo == importer.drm_file->bo_handles->bo && drm_bo_size(bo) == 8192);

    drm_file_close(&card);
    task_discard_file_fd(&producer, export.fd);
    task_discard_file_fd(&producer, duplicate.fd);
    task_discard_file_fd(&consumer, fd);
    assert(!drm_bo_prime_description(bo) && drm_bo_page_physical(bo, 4096));
    drm_file_close(&importer);
    drm_bo_put(bo);
    assert(allocations == drm_test_permanent_allocations);
}

static int drm_test_fail_alloc_bo(struct task *task, struct task_file *task_file)
{
    struct drm_mode_create_dumb argument = {.width = 32, .height = 32, .bpp = 32};
    return ioctl_call(task, task_file, DRM_IOCTL_MODE_CREATE_DUMB, &argument);
}

static void test_faildiag_real_bo_failures(void)
{
    struct task task = make_task(314, false);
    struct drm_file drm_file = {0};
    struct task_file task_file = {.drm_file = &drm_file};
    const uint64_t original_bo_bytes = drm_bo_bytes;
    const unsigned original_allocations = allocations;
    const uint64_t original_pages_allocated = mm_pages_allocated;
    const uint64_t original_pages_freed = mm_pages_freed;
    assert(original_bo_bytes == 0u);

    drm_test_diag_log_reset();
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_test_diag_log_calls == 0u);
    drm_bo_bytes = original_bo_bytes;

    const char *near_matches[] = {
        "xdrm.faildiag=1",
        "drm.faildiag=10",
        "drm.faildiag=1x",
        "drm.faildiag=0",
    };
    for (unsigned i = 0; i < sizeof(near_matches) / sizeof(near_matches[0]); ++i) {
        drm_faildiag_configure(near_matches[i]);
        drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
        assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
        assert(drm_test_diag_log_calls == 0u);
        drm_bo_bytes = original_bo_bytes;
    }
    drm_faildiag_configure(NULL);
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_test_diag_log_calls == 0u);
    drm_bo_bytes = original_bo_bytes;

    drm_faildiag_configure("quiet drm.faildiag=1 console=ttyS0");
    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    assert(drm_bo_bytes == DRM_BO_GLOBAL_BYTES);
    drm_bo_bytes = original_bo_bytes;
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "[drm:faildiag]"));
    assert(strstr(drm_test_diag_log_buffer, "pid=314"));
    assert(strstr(drm_test_diag_log_buffer, "handler=11/bo"));
    assert(strstr(drm_test_diag_log_buffer, "stage=bo.global_bytes"));
    assert(strstr(drm_test_diag_log_buffer, "errno=-12"));
    assert(strstr(drm_test_diag_log_buffer, "requested=4096"));
    assert(strstr(drm_test_diag_log_buffer, "aligned=4096"));
    assert(strstr(drm_test_diag_log_buffer, "retained=268435456"));
    assert(strstr(drm_test_diag_log_buffer, "pages=1"));
    assert(strstr(drm_test_diag_log_buffer, "free_kib=12345"));

    drm_test_diag_log_reset();
    drm_faildiag_configure("drm.faildiag=1");
    mm_allocation_fails = true;
    assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    mm_allocation_fails = false;
    assert(drm_bo_bytes == original_bo_bytes);
    assert(allocations == original_allocations);
    assert(mm_pages_allocated == original_pages_allocated);
    assert(mm_pages_freed == original_pages_freed);
    assert(drm_test_diag_log_calls == 1u);
    assert(strstr(drm_test_diag_log_buffer, "stage=bo.physical_pages"));
    assert(strstr(drm_test_diag_log_buffer, "retained=0"));

    drm_bo_bytes = DRM_BO_GLOBAL_BYTES;
    for (unsigned i = 0; i < 20u; ++i)
        assert(drm_test_fail_alloc_bo(&task, &task_file) == -LINUX_ENOMEM);
    drm_bo_bytes = original_bo_bytes;
    assert(drm_test_diag_log_calls == 16u);
    assert(allocations == original_allocations);
    assert(mm_pages_allocated == original_pages_allocated);
    assert(mm_pages_freed == original_pages_freed);
    assert(drm_bo_bytes == original_bo_bytes);
    drm_faildiag_configure(NULL);
    drm_test_diag_log_reset();
}

static void test_faildiag_parameter_bootstrapping(void)
{
    const struct module_parameter *parameter = NULL;
    for (const struct module_parameter *p = __start___param; p < __stop___param; ++p)
        if (!strcmp(p->name, "drm.faildiag")) parameter = p;
    assert(parameter && parameter->type == MODULE_PARAMETER_bool);
    assert(parameter->permissions == 0400 && !*(bool *)parameter->value);
    struct parameter_values values = {.records = parameter, .count = 1};
    assert(!parameter_arguments(&values, "drm.faildiag=1", false));
    assert(*(bool *)parameter->value);
    assert(parameter_arguments(&values, "drm.faildiag=invalid", false) == -LINUX_EINVAL);
    assert(*(bool *)parameter->value);
    assert(!parameter_arguments(&values, "drm.faildiag=0", false));
    assert(!*(bool *)parameter->value);
    assert(!parameter_arguments(&values, "drm.faildiag=Y", false));
}

int main(void)
{
#ifdef RELIEFNT_DRM_NATIVE_OWNER_TEST
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    memset(runtime,0,sizeof(*runtime));
    strcpy(drm_test_native_owner.name,"drm");strcpy(runtime->name,"drm");
    drm_test_native_owner.runtime=runtime;runtime->owner=&drm_test_native_owner;
    runtime->state=MODULE_LIVE;runtime->init_succeeded=true;module_list=runtime;
    unsigned heap_before=allocations;
    runtime->state=MODULE_GOING;assert(!drm_async_alloc(32) && allocations==heap_before);
    runtime->state=MODULE_LIVE;
    void *owned=drm_async_alloc(32);assert(owned && runtime->users==1);
    assert(module_unload("drm",0)==-LINUX_EWOULDBLOCK && runtime->state==MODULE_LIVE);
    drm_async_free(owned);assert(!runtime->users && allocations==heap_before);
#endif
    test_faildiag_parameter_bootstrapping();
    assert(!drm_character_init());
    assert(drm_faildiag_is_enabled());
    drm_faildiag_configure(NULL);
    drm_test_permanent_allocations=allocations;
    test_registration_and_open_lifetime();
    test_core_abi_and_validation();
    test_legacy_magic_and_empty_readiness();
    test_dumb_bo_contract();
    test_prime_bo_lifetime();
    test_faildiag_real_bo_failures();
    assert(mm_pages_freed == mm_pages_allocated);
    drm_character_exit();
#ifdef RELIEFNT_DRM_NATIVE_OWNER_TEST
    assert(!runtime->users && !runtime->executing);
    assert(!module_unload("drm",0) && !module_list);
    puts("PASS actual DRM core: native async-owner rejection, busy unload stays LIVE, full real state machines and final ledger/code teardown");
#endif
    assert(!allocations);
    puts("drm-core: PASS");
    return 0;
}
