#include "drm_internal.h"

#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/futex.h>
#include <reliefnt/storage.h>
#include <reliefnt/time.h>

static bool drm_task_admin(const struct task *task)
{
    return task && (task->cap_effective & (1ULL << CAP_SYS_ADMIN));
}

static void drm_master_release_device(struct drm_file *file)
{
    struct drm_device *device = file->device;
    struct drm_master *master = file->master;
    if (!device || !master) return;
    if (device->master == master && file->is_master) {
        device->master = NULL;
        drm_master_put(master);
    }
}

static void drm_master_remove_file_magics(struct drm_file *file)
{
    struct drm_magic_entry **link;
    if (!file->master) return;
    link = &file->master->magics;
    while (*link) {
        if ((*link)->file == file ||
            (file->magic && (*link)->magic == file->magic)) {
            struct drm_magic_entry *entry = *link;
            *link = entry->next;
            kernel_free(entry);
        } else {
            link = &(*link)->next;
        }
    }
}

int drm_file_open(struct task *task, struct task_file *task_file)
{
    struct drm_device *device = drm_device_get();
    struct drm_file *file;
    bool is_render;
    if (!task || !task_file || !device) return -LINUX_ENODEV;
    is_render = task_file->node.first_cluster ==
                STORAGE_DEV_KIND_DRM_RENDERD128;
    if (is_render && !drm_device_render_node_ready()) return -LINUX_ENODEV;
    file = kernel_malloc(sizeof(*file));
    if (!file) return -LINUX_ENOMEM;
    *file = (struct drm_file){
        .device = device,
        .owner_tgid = sched_task_tgid(task),
        .authenticated = !is_render && drm_task_admin(task),
        .is_render = is_render,
    };
    kernel_wait_queue_init(&file->event_wait);
    if (is_render) {
        /* Render OFDs have their own client class and never participate in
         * primary DRM master ownership, even when they open first. */
    } else if (!device->master) {
        struct drm_master *master = drm_master_create();
        if (!master) {
            kernel_free(file);
            return -LINUX_ENOMEM;
        }
        /* The master is referenced once by the open file and once by device. */
        drm_master_get(master);
        file->master = master;
        file->is_master = true;
        file->was_master = true;
        file->authenticated = true;
        device->master = master;
    } else {
        file->master = device->master;
        drm_master_get(file->master);
    }
    task_file->drm_file = file;
    return 0;
}

void drm_file_close(struct task_file *task_file)
{
    struct drm_file *file;
    if (!task_file || !task_file->drm_file) return;
    file = task_file->drm_file;
    task_file->drm_file = NULL;
    /* Handles and SYNCCPU grabs belong to the DRM open-file description.
     * VMA-held task_file references defer this final close until the last
     * mapping is gone. */
    drm_kms_file_release(file);
    drm_vmwgfx_execbuf_file_release(file);
    drm_bo_file_release(file);
    /* Resource close only drops file-local metadata and queues retirement;
     * backend callbacks run later from the bounded syscall-context reaper. */
    drm_vmwgfx_resource_file_release(file);
    drm_master_remove_file_magics(file);
    drm_master_release_device(file);
    drm_master_put(file->master);
    kernel_free(file);
}

int drm_file_set_master(struct task *task, struct drm_file *file)
{
    struct drm_device *device;
    struct drm_master *master;
    bool was_owner;
    if (!task || !file || !(device = file->device)) return -LINUX_EINVAL;
    if (file->is_render) return -LINUX_EACCES;
    was_owner = file->was_master && file->owner_tgid == sched_task_tgid(task);
    if (!was_owner && !drm_task_admin(task)) return -LINUX_EACCES;
    if (file->is_master && device->master == file->master) return 0;
    if (device->master) return -LINUX_EBUSY;
    if (!file->master) return -LINUX_EINVAL;
    if (!file->is_master) {
        master = drm_master_create();
        if (!master) return -LINUX_ENOMEM;
        drm_master_get(master);
        /* Magic entries are owned by the old master. Drop them before the
         * per-open file switches its master reference. */
        drm_master_remove_file_magics(file);
        file->magic = 0;
        drm_master_put(file->master);
        file->master = master;
    } else {
        master = file->master;
        drm_master_get(master);
    }
    device->master = master;
    file->is_master = true;
    file->was_master = true;
    file->authenticated = true;
    file->owner_tgid = sched_task_tgid(task);
    return 0;
}

int drm_file_drop_master(struct task *task, struct drm_file *file)
{
    struct drm_device *device;
    if (!task || !file || !(device = file->device)) return -LINUX_EINVAL;
    if (file->is_render) return -LINUX_EACCES;
    if (!(file->was_master && file->owner_tgid == sched_task_tgid(task)) &&
        !drm_task_admin(task))
        return -LINUX_EACCES;
    if (!file->is_master || device->master != file->master || !device->master)
        return -LINUX_EINVAL;
    drm_kms_drop_master(file);
    device->master = NULL;
    drm_master_put(file->master);
    return 0;
}

int drm_file_read(struct task *task, struct task_file *task_file,
                  void *buffer, uint32_t length)
{
    (void)task;
    (void)buffer;
    struct drm_file *file = task_file ? task_file->drm_file : NULL;
    if (!file) return -LINUX_EBADF;
    if (!length) return 0;
    if (task_file->flags & RELIEFOS_O_NONBLOCK) return -LINUX_EAGAIN;
    if (kernel_wait_queue_add(&file->event_wait, task) == 0)
        sched_block_current();
    else
        /* A saturated generic queue must still preserve blocking read
         * semantics. Retry after one tick so a freed slot or pending signal
         * is observed without turning this syscall into a busy loop. */
        sched_sleep_current_until(time_ticks() + 1u);
    return KERNEL_SYSCALL_BLOCKED;
}

uint32_t drm_file_poll(struct task_file *task_file, uint32_t events)
{
    (void)events;
    (void)task_file;
    return 0;
}
