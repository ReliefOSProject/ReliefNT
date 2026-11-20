#include "drm_internal.h"
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#endif

#include <drm/drm_mode.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/sched.h>

#define DRM_BO_PAGE_SIZE 4096ULL
#define DRM_BO_MAX_BYTES (128ULL * 1024ULL * 1024ULL)
#define DRM_BO_GLOBAL_BYTES (256ULL * 1024ULL * 1024ULL)
#define DRM_BO_HANDLE_MIN 1u
#define DRM_BO_HANDLE_MAX 0x00ffffffu
#define DRM_RESOURCE_HANDLE_MIN 0x01000000u

struct drm_bo {
    uint32_t references;
    uint32_t page_count;
    uint32_t gpu_reservations;
    uint64_t size;
    uint64_t physical;
    uint64_t map_offset;
    struct vg_resource *virtio;
    struct task_file *prime_description; /* weak; execution owner serializes close */
};

struct drm_bo_handle {
    struct drm_bo_handle *next;
    struct drm_bo *bo;
    uint32_t handle;
};

static struct kernel_spinlock drm_bo_lock = KERNEL_SPINLOCK_INIT;
static uint64_t drm_bo_bytes;
static uint64_t drm_bo_next_map_offset = DRM_BO_PAGE_SIZE;

static uint64_t drm_bo_align_size(uint64_t size)
{
    if (!size || size > UINT64_MAX - (DRM_BO_PAGE_SIZE - 1ULL)) return 0;
    return (size + DRM_BO_PAGE_SIZE - 1ULL) & ~(DRM_BO_PAGE_SIZE - 1ULL);
}

static struct drm_bo_handle *drm_bo_find_handle_locked(struct drm_file *file,
                                                        uint32_t handle)
{
    for (struct drm_bo_handle *entry = file ? file->bo_handles : NULL;
         entry; entry = entry->next) {
        if (entry->handle == handle) return entry;
    }
    return NULL;
}

void drm_bo_get(struct drm_bo *bo)
{
    if (bo) __atomic_add_fetch(&bo->references, 1u, __ATOMIC_RELAXED);
}

void drm_bo_put(struct drm_bo *bo)
{
    if (!bo || __atomic_sub_fetch(&bo->references, 1u, __ATOMIC_ACQ_REL) != 0)
        return;
#ifdef CONFIG_DRM_VIRTIO_GPU
    /* A protocol timeout never frees pages that the device can still DMA. */
    if (bo->virtio && !vg_resource_destroy(bo->virtio)) return;
#endif
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    if (drm_bo_bytes >= bo->size) drm_bo_bytes -= bo->size;
    else drm_bo_bytes = 0;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    mm_free_pages(bo->physical, bo->page_count);
    kernel_free(bo);
}

uint64_t drm_bo_page_physical(const struct drm_bo *bo, uint64_t offset)
{
    if (!bo || !bo->physical || (offset & (DRM_BO_PAGE_SIZE - 1ULL)) ||
        offset >= bo->size) return 0;
    return bo->physical + offset;
}

uint64_t drm_bo_size(const struct drm_bo *bo)
{
    return bo ? bo->size : 0;
}

/** @brief Borrow metadata only while the caller retains the associated GEM BO.
 * @param bo Retained BO. @return VirtIO resource or NULL; execution-owned.
 */
struct vg_resource *drm_bo_virtio(const struct drm_bo *bo)
{ return bo ? bo->virtio : NULL; }
/** @brief Adopt one resource in an exclusive, unpublished GEM BO.
 * @param bo Exclusive BO. @param resource Owned backend allocation.
 */
void drm_bo_set_virtio(struct drm_bo *bo, struct vg_resource *resource)
{ bo->virtio = resource; }
/** @brief Return a retained BO's immutable mmap byte selector.
 * @param bo Retained BO. @return Selector or zero.
 */
uint64_t drm_bo_offset(const struct drm_bo *bo)
{ return bo ? bo->map_offset : 0; }

/** @brief Borrow a canonical DMA-BUF OFD while execution ownership excludes close.
 * @param bo Retained BO. @return Weak cached description or NULL; not a new ref.
 */
struct task_file *drm_bo_prime_description(struct drm_bo *bo)
{ return bo ? bo->prime_description : NULL; }

/** @brief Associate an unpublished export without creating a reference cycle.
 * @param bo Retained BO. @param description Canonical export OFD, weakly held.
 * @return Zero or EEXIST; task/execution ownership excludes concurrent mutation.
 */
int drm_bo_prime_bind(struct drm_bo *bo,struct task_file *description)
{
    if(bo->prime_description)return -LINUX_EEXIST;
    bo->prime_description=description;return 0;
}

/** @brief Clear a dying export's weak cache before releasing its last BO ref.
 * @param bo Retained BO. @param description Final canonical OFD; execution-owned.
 */
void drm_bo_prime_unbind(struct drm_bo *bo,struct task_file *description)
{ if(bo && bo->prime_description==description)bo->prime_description=NULL; }

int drm_bo_gpu_reserve(struct drm_bo *bo)
{
    if (!bo) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    int ret = 0;
    if (bo->gpu_reservations == UINT32_MAX) ret = -LINUX_EBUSY;
    else ++bo->gpu_reservations;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return ret;
}

void drm_bo_gpu_release(struct drm_bo *bo)
{
    if (!bo) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    if (bo->gpu_reservations) --bo->gpu_reservations;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
}

bool drm_bo_gpu_busy(struct drm_bo *bo)
{
    if (!bo) return false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    bool busy = bo->gpu_reservations != 0;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return busy;
}

struct drm_bo *drm_bo_lookup_handle_get(struct drm_file *file, uint32_t handle)
{
    if (!file || !handle || handle >= DRM_RESOURCE_HANDLE_MIN) return NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    struct drm_bo_handle *entry = drm_bo_find_handle_locked(file, handle);
    struct drm_bo *bo = entry ? entry->bo : NULL;
    drm_bo_get(bo);
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return bo;
}

static int drm_bo_reserve_bytes(uint64_t size, uint64_t *retained_out)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    if (size > DRM_BO_MAX_BYTES || size > DRM_BO_GLOBAL_BYTES - drm_bo_bytes) {
        if (retained_out) *retained_out = drm_bo_bytes;
        kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
        return -LINUX_ENOMEM;
    }
    drm_bo_bytes += size;
    if (retained_out) *retained_out = drm_bo_bytes;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return 0;
}

static uint64_t drm_bo_release_reserved_bytes(uint64_t size)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    if (drm_bo_bytes >= size) drm_bo_bytes -= size;
    else drm_bo_bytes = 0;
    uint64_t retained = drm_bo_bytes;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return retained;
}

static void drm_bo_record_failure(struct drm_faildiag_context *diagnostics,
                                 int error, const char *stage,
                                 uint64_t requested, uint64_t aligned,
                                 uint64_t retained, uint32_t page_count)
{
    if (!drm_faildiag_is_enabled()) return;
    const struct drm_faildiag_field fields[] = {
        {"requested", requested},
        {"aligned", aligned},
        {"retained", retained},
        {"pages", page_count},
        {"free_kib", mm_free_memory_kib()},
    };
    drm_faildiag_record(diagnostics, error, stage, fields,
                        sizeof(fields) / sizeof(fields[0]));
}

static uint32_t drm_bo_allocate_handle_locked(struct drm_file *file)
{
    uint32_t candidate = file->next_bo_handle;
    if (candidate < DRM_BO_HANDLE_MIN || candidate > DRM_BO_HANDLE_MAX)
        candidate = DRM_BO_HANDLE_MIN;
    uint32_t first = candidate;
    do {
        if (!drm_bo_find_handle_locked(file, candidate)) {
            file->next_bo_handle = candidate == DRM_BO_HANDLE_MAX
                ? DRM_BO_HANDLE_MIN : candidate + 1u;
            return candidate;
        }
        candidate = candidate == DRM_BO_HANDLE_MAX
            ? DRM_BO_HANDLE_MIN : candidate + 1u;
    } while (candidate != first);
    return 0;
}

/** @brief Create or find the per-OFD handle for an existing same-device GEM BO.
 * @param file Target OFD. @param bo Retained source. @param handle Output ID.
 * @param created True only for a newly allocated logical handle.
 * @return Zero or -EINVAL/-ENOMEM/-ENOSPC; task context, no allocation under lock.
 */
int drm_bo_handle_import(struct drm_file *file, struct drm_bo *bo, uint32_t *handle, bool *created)
{
    if(!file || !bo || !handle || !created)return -LINUX_EINVAL;
    struct drm_bo_handle *entry=kernel_malloc(sizeof(*entry));
    if(!entry)return -LINUX_ENOMEM;
    uint64_t flags;kernel_spin_lock_irqsave(&drm_bo_lock,&flags);
    for(struct drm_bo_handle *old=file->bo_handles;old;old=old->next) {
        if(old->bo==bo) {
            *handle=old->handle;*created=false;
            kernel_spin_unlock_irqrestore(&drm_bo_lock,flags);kernel_free(entry);return 0;
        }
    }
    uint32_t id=drm_bo_allocate_handle_locked(file);
    if(id) {
        drm_bo_get(bo);*entry=(struct drm_bo_handle){.next=file->bo_handles,.bo=bo,.handle=id};
        file->bo_handles=entry;*handle=id;*created=true;
    }
    kernel_spin_unlock_irqrestore(&drm_bo_lock,flags);
    if(!id)kernel_free(entry);
    return id ? 0 : -LINUX_ENOSPC;
}

/** @brief Allocate a per-file BO, respecting the owning device's address mask.
 * @param file Borrowed file and device. @param diagnostics Optional diagnostics.
 * @param requested_size Nonzero requested bytes. @param handle_out Required handle output.
 * @param map_offset_out Required mapping-offset output.
 * @return Zero with an owned handle, or negative errno with no published BO.
 */
int drm_bo_create(struct drm_file *file,
                  struct drm_faildiag_context *diagnostics,
                  uint64_t requested_size,
                  uint32_t *handle_out, uint64_t *map_offset_out)
{
    uint64_t size = drm_bo_align_size(requested_size);
    if (!file || !handle_out || !map_offset_out || !size ||
        size > DRM_BO_MAX_BYTES) return -LINUX_EINVAL;
    uint32_t page_count = (uint32_t)(size / DRM_BO_PAGE_SIZE);
    uint64_t retained = 0;
    int ret = drm_bo_reserve_bytes(size, &retained);
    if (ret < 0) {
        drm_bo_record_failure(diagnostics, ret, "bo.global_bytes",
                              requested_size, size, retained, page_count);
        return ret;
    }

    struct drm_bo *bo = kernel_malloc(sizeof(*bo));
    struct drm_bo_handle *entry = kernel_malloc(sizeof(*entry));
    if (!bo || !entry) {
        const char *stage = bo ? "bo.metadata_handle" : "bo.metadata_object";
        kernel_free(entry);
        kernel_free(bo);
        retained = drm_bo_release_reserved_bytes(size);
        drm_bo_record_failure(diagnostics, -LINUX_ENOMEM, stage,
                              requested_size, size, retained, page_count);
        return -LINUX_ENOMEM;
    }
    uint64_t physical = mm_alloc_pages(page_count);
    if (!physical) {
        kernel_free(entry);
        kernel_free(bo);
        retained = drm_bo_release_reserved_bytes(size);
        drm_bo_record_failure(diagnostics, -LINUX_ENOMEM,
                              "bo.physical_pages", requested_size, size,
                              retained, page_count);
        return -LINUX_ENOMEM;
    }
    *bo = (struct drm_bo){
        .references = 1,
        .page_count = page_count,
        .size = size,
        .physical = physical,
    };

    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    uint32_t handle = drm_bo_allocate_handle_locked(file);
    if (!handle || drm_bo_next_map_offset > (uint64_t)INT64_MAX - size) {
        if (handle) {
            /* The unused handle was only reserved in next_bo_handle. It has
             * not been linked, so leaving a harmless gap is safe. */
        }
        kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
        mm_free_pages(physical, page_count);
        kernel_free(entry);
        kernel_free(bo);
        kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
        drm_bo_bytes -= size;
        kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
        return -LINUX_ENOSPC;
    }
    bo->map_offset = drm_bo_next_map_offset;
    drm_bo_next_map_offset += size;
    *entry = (struct drm_bo_handle){
        .next = file->bo_handles,
        .bo = bo,
        .handle = handle,
    };
    file->bo_handles = entry;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);

    *handle_out = handle;
    *map_offset_out = bo->map_offset;
    return 0;
}

int drm_bo_handle_delete(struct drm_file *file, uint32_t handle)
{
    if (!file || !handle || handle >= DRM_RESOURCE_HANDLE_MIN)
        return -LINUX_EINVAL;
    struct drm_bo_handle **link;
    struct drm_bo_handle *entry = NULL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    link = &file->bo_handles;
    while (*link && (*link)->handle != handle) link = &(*link)->next;
    if (*link) {
        entry = *link;
        *link = entry->next;
    }
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    if (!entry) return -LINUX_EINVAL;
    drm_bo_put(entry->bo);
    kernel_free(entry);
    return 0;
}

void drm_bo_file_release(struct drm_file *file)
{
    if (!file) return;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    struct drm_bo_handle *handles = file->bo_handles;
    file->bo_handles = NULL;
    for (struct drm_bo_handle *entry = handles; entry; entry = entry->next) {
    }
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    while (handles) {
        struct drm_bo_handle *next = handles->next;
        drm_bo_put(handles->bo);
        kernel_free(handles);
        handles = next;
    }
}

int drm_file_mmap_bo(struct task_file *task_file, uint64_t offset,
                     uint64_t length, struct drm_bo **bo_out,
                     uint64_t *bo_offset_out)
{
    struct drm_file *file = task_file ? task_file->drm_file : NULL;
    if (!file || !bo_out || !bo_offset_out || !length ||
        (offset & (DRM_BO_PAGE_SIZE - 1ULL)) || offset > (uint64_t)INT64_MAX ||
        length > UINT64_MAX - offset) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    for (struct drm_bo_handle *entry = file->bo_handles; entry; entry = entry->next) {
        struct drm_bo *bo = entry->bo;
        if (offset < bo->map_offset) continue;
        uint64_t delta = offset - bo->map_offset;
        if (delta > bo->size || length > bo->size - delta) continue;
        drm_bo_get(bo);
        *bo_out = bo;
        *bo_offset_out = delta;
        kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
        return 0;
    }
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return -LINUX_EINVAL;
}

static int drm_bo_map_offset(struct drm_file *file, uint32_t handle,
                             uint64_t *offset_out)
{
    if (!file || !handle || !offset_out) return -LINUX_EINVAL;
    uint64_t flags;
    kernel_spin_lock_irqsave(&drm_bo_lock, &flags);
    struct drm_bo_handle *entry = drm_bo_find_handle_locked(file, handle);
    if (entry) *offset_out = entry->bo->map_offset;
    kernel_spin_unlock_irqrestore(&drm_bo_lock, flags);
    return entry ? 0 : -LINUX_EINVAL;
}

static int drm_bo_create_dumb(struct drm_file *file,
                              struct drm_mode_create_dumb *arg,
                              struct drm_faildiag_context *diagnostics,
                              uint32_t *created_handle_out)
{
    if (!arg) return -LINUX_EFAULT;
    uint32_t width = arg->width;
    uint32_t height = arg->height;
    uint32_t bpp = arg->bpp;
    uint32_t flags = arg->flags;
    arg->handle = 0;
    arg->pitch = 0;
    arg->size = 0;
    if (!width || !height || !bpp || flags || bpp > UINT32_MAX - 8u)
        return -LINUX_EINVAL;
    uint32_t cpp = (bpp + 7u) / 8u;
    if (cpp != 1 && cpp != 2 && cpp != 4) return -LINUX_EINVAL;
    if (width > UINT32_MAX / cpp) return -LINUX_EINVAL;
    uint32_t pitch = width * cpp;
    if (height > UINT32_MAX / pitch) return -LINUX_EINVAL;
    uint64_t bytes = (uint64_t)pitch * height;
    uint64_t size = drm_bo_align_size(bytes);
    if (!size || size > UINT32_MAX) return -LINUX_EINVAL;

    uint32_t handle;
    uint64_t map_offset;
    int ret = drm_bo_create(file, diagnostics, size, &handle, &map_offset);
    if (ret < 0) return ret;
#ifdef CONFIG_DRM_VIRTIO_GPU
    if(drm_is_virtio()) {
        if(bpp!=32 && bpp!=24){(void)drm_bo_handle_delete(file,handle);return -LINUX_EINVAL;}
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,handle);
        struct drm_virtgpu_resource_create parameters={.width=width,.height=height,.depth=1,
            .array_size=1,.format=VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM,.target=2,.stride=pitch};
        ret=vg_resource_create(bo,&parameters,false);drm_bo_put(bo);
        if(ret){(void)drm_bo_handle_delete(file,handle);return ret;}
    }
#endif
    arg->handle = handle;
    arg->pitch = pitch;
    arg->size = size;
    if (created_handle_out) *created_handle_out = handle;
    (void)map_offset;
    return 0;
}

int drm_bo_ioctl(struct drm_file *file, uint32_t command, void *data,
                 uint32_t *created_handle_out,
                 struct drm_faildiag_context *diagnostics)
{
    if (created_handle_out) *created_handle_out = 0;
    if (!file) return -LINUX_EBADF;
    switch (command) {
    case DRM_IOCTL_MODE_CREATE_DUMB:
        return drm_bo_create_dumb(file, data, diagnostics,
                                  created_handle_out);
    case DRM_IOCTL_MODE_MAP_DUMB: {
        struct drm_mode_map_dumb *arg = data;
        if (!arg) return -LINUX_EFAULT;
        arg->offset = 0;
        int rc=drm_bo_map_offset(file, arg->handle, &arg->offset);
        return drm_is_virtio() && rc==-LINUX_EINVAL ? -LINUX_ENOENT : rc;
    }
    case DRM_IOCTL_MODE_DESTROY_DUMB: {
        struct drm_mode_destroy_dumb *arg = data;
        if (!arg) return -LINUX_EFAULT;
        return drm_bo_handle_delete(file, arg->handle);
    }
    case DRM_IOCTL_GEM_CLOSE: {
        struct drm_gem_close *arg = data;
        if (!arg) return -LINUX_EFAULT;
        return drm_bo_handle_delete(file, arg->handle);
    }
    default:
        return -LINUX_EINVAL;
    }
}
