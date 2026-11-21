#include "drm_internal.h"
#include <reliefnt/console.h>

#include <drm/drm_mode.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/text_stream.h>
#include <reliefnt/usercopy.h>
#ifdef CONFIG_SWAP
#include <reliefnt/swap.h>
#include <reliefnt/paging.h>

/** @brief Prepare an actual DRM operand at the outer execution boundary.
 * @param task Current user owner. @param address First user byte.
 * @param length Actual byte count. @param writing True for a destination.
 * @return Zero, EFAULT, or prerequisite wait; each preparation is at most 32 KiB.
 */
int drm_user_prepare(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    if (!length) return 0;
    if (address<RELIEFNT_USER_BASE || address>=RELIEFNT_USER_TOP ||
        length>RELIEFNT_USER_TOP-address) return -LINUX_EFAULT;
    while(length) {
        uint64_t take=length<32768u ? length : 32768u;
        int ret=swap_prepare_user_range(task,address,take,writing);
        if(ret==SWAP_STORAGE_CONTENDED)return KERNEL_SYSCALL_SWAP_CONTENDED;
        if(ret<0)return -LINUX_EFAULT;
        address+=take;length-=take;
    }
    return 0;
}

/** @brief Import a DRM input into kernel storage using the retry-owned snapshot.
 * @param task Current owner. @param destination Kernel buffer. @param address
 * User source. @param length Actual input size. @return Zero, errno or wait.
 */
int drm_user_import(struct task *task,void *destination,uint64_t address,uint32_t length)
{
    if(!length)return 0;
    struct task_user_text snapshot;
    int ret=task_lookup_user_bytes(task,address,length,&snapshot);
    if(ret)return ret;
    __builtin_memcpy(destination,snapshot.text,length);kernel_free(snapshot.allocation);
    return 0;
}
#endif

#define DRM_IOCTL_REQUIRED_IN  (_IOC_WRITE << _IOC_DIRSHIFT)
#define DRM_IOCTL_REQUIRED_OUT (_IOC_READ << _IOC_DIRSHIFT)
#define DRM_IOCTL_INTERFACE_MAJOR 1
#define DRM_IOCTL_INTERFACE_MINOR 4

enum drm_ioctl_handler {
    DRM_HANDLER_UNSUPPORTED,
    DRM_HANDLER_VERSION,
    DRM_HANDLER_GET_UNIQUE,
    DRM_HANDLER_GET_MAGIC,
    DRM_HANDLER_GET_CAP,
    DRM_HANDLER_SET_CLIENT_CAP,
    DRM_HANDLER_SET_VERSION,
    DRM_HANDLER_SET_UNIQUE,
    DRM_HANDLER_AUTH_MAGIC,
    DRM_HANDLER_SET_MASTER,
    DRM_HANDLER_DROP_MASTER,
    DRM_HANDLER_BO,
    DRM_HANDLER_PRIME,
    DRM_HANDLER_KMS,
    DRM_HANDLER_DRIVER,
};

struct drm_ioctl_desc {
    uint32_t command;
    enum drm_ioctl_handler handler;
    uint32_t permission;
};

static const char *drm_ioctl_handler_name(enum drm_ioctl_handler handler)
{
    switch (handler) {
    case DRM_HANDLER_BO: return "bo";
    case DRM_HANDLER_PRIME: return "prime";
    case DRM_HANDLER_KMS: return "kms";
    case DRM_HANDLER_DRIVER: return "driver";
    case DRM_HANDLER_VERSION: return "version";
    case DRM_HANDLER_GET_UNIQUE: return "get_unique";
    case DRM_HANDLER_GET_MAGIC: return "get_magic";
    case DRM_HANDLER_GET_CAP: return "get_cap";
    case DRM_HANDLER_SET_CLIENT_CAP: return "set_client_cap";
    case DRM_HANDLER_SET_VERSION: return "set_version";
    case DRM_HANDLER_SET_UNIQUE: return "set_unique";
    case DRM_HANDLER_AUTH_MAGIC: return "auth_magic";
    case DRM_HANDLER_SET_MASTER: return "set_master";
    case DRM_HANDLER_DROP_MASTER: return "drop_master";
    case DRM_HANDLER_UNSUPPORTED: return "unsupported";
    }
    return "unknown";
}

#define DRM_IOCTL_ENTRY(command_, handler_) \
    { .command = (command_), .handler = (handler_) }
#define DRM_IOCTL_DENY_ENTRY(command_, permissions_) \
    { .command = (command_), .handler = DRM_HANDLER_UNSUPPORTED, \
      .permission = (permissions_) }

/* Match the Linux 6.12 core table's command shapes and access flags. The
 * supported query and ownership entries are handled below; every other known
 * command is rejected with EOPNOTSUPP after Linux-compatible argument copy. */
static const struct drm_ioctl_desc drm_ioctl_table[] = {
    { .command = DRM_IOCTL_VERSION, .handler = DRM_HANDLER_VERSION,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    DRM_IOCTL_ENTRY(DRM_IOCTL_GET_UNIQUE, DRM_HANDLER_GET_UNIQUE),
    DRM_IOCTL_ENTRY(DRM_IOCTL_GET_MAGIC, DRM_HANDLER_GET_MAGIC),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_GET_CLIENT, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_GET_STATS, 0),
    { .command = DRM_IOCTL_GET_CAP, .handler = DRM_HANDLER_GET_CAP,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    DRM_IOCTL_ENTRY(DRM_IOCTL_SET_CLIENT_CAP, DRM_HANDLER_SET_CLIENT_CAP),
    { .command = DRM_IOCTL_SET_VERSION, .handler = DRM_HANDLER_SET_VERSION,
      .permission = DRM_PERMISSION_MASTER },
    { .command = DRM_IOCTL_SET_UNIQUE, .handler = DRM_HANDLER_SET_UNIQUE,
      .permission = DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                    DRM_PERMISSION_ADMIN },
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_BLOCK,
                         DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                         DRM_PERMISSION_ADMIN),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_UNBLOCK,
                         DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                         DRM_PERMISSION_ADMIN),
    { .command = DRM_IOCTL_AUTH_MAGIC, .handler = DRM_HANDLER_AUTH_MAGIC,
      .permission = DRM_PERMISSION_MASTER },
    DRM_IOCTL_ENTRY(DRM_IOCTL_SET_MASTER, DRM_HANDLER_SET_MASTER),
    DRM_IOCTL_ENTRY(DRM_IOCTL_DROP_MASTER, DRM_HANDLER_DROP_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_ADD_DRAW,
                         DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                         DRM_PERMISSION_ADMIN),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_RM_DRAW,
                         DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                         DRM_PERMISSION_ADMIN),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_FINISH, DRM_PERMISSION_AUTH),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_WAIT_VBLANK, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_UPDATE_DRAW,
                         DRM_PERMISSION_AUTH | DRM_PERMISSION_MASTER |
                         DRM_PERMISSION_ADMIN),
    { .command = DRM_IOCTL_GEM_CLOSE, .handler = DRM_HANDLER_BO,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_GEM_FLINK, DRM_PERMISSION_AUTH),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_GEM_OPEN, DRM_PERMISSION_AUTH),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETRESOURCES, DRM_HANDLER_KMS),
    { .command = DRM_IOCTL_PRIME_HANDLE_TO_FD, .handler = DRM_HANDLER_PRIME,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOCTL_PRIME_FD_TO_HANDLE, .handler = DRM_HANDLER_PRIME,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GETPLANERESOURCES, 0),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETCRTC, DRM_HANDLER_KMS),
    { .command = DRM_IOCTL_MODE_SETCRTC, .handler = DRM_HANDLER_KMS,
      .permission = DRM_PERMISSION_MASTER },
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GETPLANE, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_SETPLANE, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_CURSOR, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GETGAMMA, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_SETGAMMA, DRM_PERMISSION_MASTER),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETENCODER, DRM_HANDLER_KMS),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETCONNECTOR, DRM_HANDLER_KMS),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_ATTACHMODE, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_DETACHMODE, DRM_PERMISSION_MASTER),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETPROPERTY, DRM_HANDLER_KMS),
    { .command = DRM_IOCTL_MODE_SETPROPERTY, .handler = DRM_HANDLER_KMS,
      .permission = DRM_PERMISSION_MASTER },
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_GETPROPBLOB, DRM_HANDLER_KMS),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GETFB, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GETFB2, 0),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_ADDFB, DRM_HANDLER_KMS),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_ADDFB2, DRM_HANDLER_KMS),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_RMFB, DRM_HANDLER_KMS),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_CLOSEFB, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_PAGE_FLIP, DRM_PERMISSION_MASTER),
    { .command = DRM_IOCTL_MODE_DIRTYFB, .handler = DRM_HANDLER_KMS,
      .permission = DRM_PERMISSION_MASTER },
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_CREATE_DUMB, DRM_HANDLER_BO),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_MAP_DUMB, DRM_HANDLER_BO),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_DESTROY_DUMB, DRM_HANDLER_BO),
    DRM_IOCTL_ENTRY(DRM_IOCTL_MODE_OBJ_GETPROPERTIES, DRM_HANDLER_KMS),
    { .command = DRM_IOCTL_MODE_OBJ_SETPROPERTY, .handler = DRM_HANDLER_KMS,
      .permission = DRM_PERMISSION_MASTER },
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_CURSOR2, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_ATOMIC, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_CREATEPROPBLOB, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_DESTROYPROPBLOB, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_CREATE, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_DESTROY, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_TRANSFER, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_WAIT, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_EVENTFD, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_RESET, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_SIGNAL, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_SYNCOBJ_QUERY, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_CRTC_GET_SEQUENCE, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_CRTC_QUEUE_SEQUENCE, 0),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_CREATE_LEASE, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_LIST_LESSEES, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_GET_LEASE, DRM_PERMISSION_MASTER),
    DRM_IOCTL_DENY_ENTRY(DRM_IOCTL_MODE_REVOKE_LEASE, DRM_PERMISSION_MASTER),
};

static const struct drm_ioctl_desc *drm_ioctl_find(const struct drm_file *file,
                                                   uint32_t command,
                                                   struct drm_ioctl_desc *private)
{
    uint32_t type = _IOC_TYPE(command);
    uint32_t number = _IOC_NR(command);
    if (type != DRM_IOCTL_BASE) return NULL;
    if (number >= DRM_COMMAND_BASE && number < DRM_COMMAND_END) {
        const struct drm_driver *driver = file->device->driver;
        for (unsigned i = 0; i < driver->ioctl_count; ++i) {
            const struct drm_driver_ioctl *entry = &driver->ioctls[i];
            if (_IOC_NR(entry->command) == number) {
                *private = (struct drm_ioctl_desc){entry->command, DRM_HANDLER_DRIVER,
                                                   entry->permission};
                return private;
            }
        }
        return NULL;
    }
    for (uint32_t i = 0; i < sizeof(drm_ioctl_table) / sizeof(drm_ioctl_table[0]); ++i) {
        if (_IOC_TYPE(drm_ioctl_table[i].command) == type &&
            _IOC_NR(drm_ioctl_table[i].command) == number)
            return &drm_ioctl_table[i];
    }
    return NULL;
}

static bool drm_ioctl_is_admin(const struct task *task)
{
    return task && (task->cap_effective & (1ULL << CAP_SYS_ADMIN));
}

static bool drm_file_is_current_master(const struct drm_file *file)
{
    return file && file->is_master && file->master && file->device &&
           file->device->master == file->master;
}

static int drm_ioctl_check_permission(const struct task *task,
                                      const struct drm_file *file,
                                      uint32_t permission)
{
    if ((permission & DRM_PERMISSION_ADMIN) && !drm_ioctl_is_admin(task))
        return -LINUX_EACCES;
    if ((permission & DRM_PERMISSION_AUTH) && !file->is_render &&
        !file->authenticated)
        return -LINUX_EACCES;
    if ((permission & DRM_PERMISSION_MASTER) && !drm_file_is_current_master(file))
        return -LINUX_EACCES;
    if (file->is_render && !(permission & DRM_PERMISSION_RENDER_ALLOW))
        return -LINUX_EACCES;
    return 0;
}

static int drm_copy_field(struct task *task, uint64_t address, size_t *length,
                          const char *value)
{
    size_t copy_length;
    size_t actual_length = __builtin_strlen(value);
    if (!length) return -LINUX_EFAULT;
    copy_length = *length < actual_length ? *length : actual_length;
    *length = actual_length;
    /* Linux drm_copy_field deliberately treats a NULL sub-buffer as omitted. */
    if (copy_length && address &&
        user_copy_to_task(task, address, value, copy_length) < 0)
        return -LINUX_EFAULT;
    return 0;
}

static int drm_ioctl_version(struct task *task, const struct drm_driver *driver,
                             struct drm_version *version)
{
    const char *name = driver->name, *description = driver->description;
#ifdef CONFIG_SWAP
    /* Prepare every actually used output before the first field is copied. */
    const uint64_t addresses[]={(uint64_t)(uintptr_t)version->name,
        (uint64_t)(uintptr_t)version->date,(uint64_t)(uintptr_t)version->desc};
    const size_t lengths[]={version->name_len,version->date_len,version->desc_len};
    const size_t actual[]={__builtin_strlen(name),1,__builtin_strlen(description)};
    for(unsigned i=0;i<3;++i) {
        size_t n=lengths[i]<actual[i] ? lengths[i] : actual[i];
        if(!addresses[i] || !n)continue;
        int prep=drm_user_prepare(task,addresses[i],n,true);
        if(prep==KERNEL_SYSCALL_SWAP_CONTENDED)return prep;
        if(prep<0 || !user_range_writable(addresses[i],n))break;
    }
#endif
    int ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->name,
                             &version->name_len, name);
    if (!ret)
        ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->date,
                             &version->date_len, "0");
    if (!ret)
        ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->desc,
                             &version->desc_len,
                             description);
    version->version_major = driver->major;
    version->version_minor = driver->minor;
    version->version_patchlevel = driver->patchlevel;
    return ret;
}

static int drm_ioctl_get_unique(struct task *task, struct drm_file *file,
                               struct drm_unique *unique)
{
    struct drm_master *master = file->master;
    if (!master) return -LINUX_EINVAL;
#ifdef CONFIG_SWAP
    if (unique->unique_len >= master->unique_length && master->unique_length &&
        drm_user_prepare(task,(uint64_t)(uintptr_t)unique->unique,master->unique_length,true)==
            KERNEL_SYSCALL_SWAP_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
#endif
    if (unique->unique_len >= master->unique_length && master->unique_length &&
        user_copy_to_task(task, (uint64_t)(uintptr_t)unique->unique,
                          master->unique, master->unique_length) < 0)
        return -LINUX_EFAULT;
    unique->unique_len = master->unique_length;
    return 0;
}

static int drm_ioctl_get_magic(struct drm_file *file, struct drm_auth *auth)
{
    struct drm_master *master = file->master;
    struct drm_magic_entry *entry;
    if (!master) return -LINUX_EINVAL;
    if (!file->magic) {
        if (!master->next_magic || master->next_magic > 0x7fffffffu)
            return -LINUX_ENOSPC;
        entry = kernel_malloc(sizeof(*entry));
        if (!entry) return -LINUX_ENOMEM;
        file->magic = master->next_magic++;
        *entry = (struct drm_magic_entry){
            .next = master->magics,
            .file = file,
            .magic = file->magic,
        };
        master->magics = entry;
    }
    auth->magic = file->magic;
    return 0;
}

static int drm_ioctl_auth_magic(struct drm_file *file, const struct drm_auth *auth)
{
    struct drm_master *master = file->master;
    if (!master) return -LINUX_EINVAL;
    for (struct drm_magic_entry *entry = master->magics; entry; entry = entry->next) {
        if (entry->magic == auth->magic && entry->file) {
            entry->file->authenticated = true;
            entry->file = NULL;
            return 0;
        }
    }
    return -LINUX_EINVAL;
}

static int drm_ioctl_set_version(struct drm_file *file,
                                 struct drm_set_version *version)
{
    const struct drm_driver *driver = file->device->driver;
    int ret = 0;
    if (version->drm_di_major != -1) {
        if (version->drm_di_major != DRM_IOCTL_INTERFACE_MAJOR ||
            version->drm_di_minor < 0 ||
            version->drm_di_minor > DRM_IOCTL_INTERFACE_MINOR) {
            ret = -LINUX_EINVAL;
            goto out;
        }
        if (version->drm_di_minor >= 1) {
            struct drm_master *master = file->master;
            if (!master) {
                ret = -LINUX_EINVAL;
                goto out;
            }
            struct pci_device *pci = &file->device->pci;
            struct text_stream busid = {
                .buffer = master->unique,
                .capacity = sizeof(master->unique) - 1,
            };
            text_string(&busid, "pci:");
            text_hex(&busid, 0, 4);
            text_string(&busid, ":");
            text_hex(&busid, pci->bus, 2);
            text_string(&busid, ":");
            text_hex(&busid, pci->slot, 2);
            text_string(&busid, ".");
            text_unsigned(&busid, pci->function);
            master->unique[busid.written] = 0;
            master->unique_length = busid.written;
        }
    }
    if (version->drm_dd_major != -1 &&
        (version->drm_dd_major != driver->major ||
         version->drm_dd_minor < 0 ||
         version->drm_dd_minor > driver->minor))
        ret = -LINUX_EINVAL;
out:
    version->drm_di_major = DRM_IOCTL_INTERFACE_MAJOR;
    version->drm_di_minor = DRM_IOCTL_INTERFACE_MINOR;
    version->drm_dd_major = driver->major;
    version->drm_dd_minor = driver->minor;
    return ret;
}

static int drm_ioctl_get_cap(const struct drm_driver *driver, struct drm_get_cap *cap)
{
    cap->value = 0;
    if (driver->get_cap) {
        int ret = driver->get_cap(cap);
        if (ret != -EOPNOTSUPP)
            return ret;
    }
    if (cap->capability == DRM_CAP_TIMESTAMP_MONOTONIC) {
        cap->value = 1;
        return 0;
    }
    if (cap->capability == DRM_CAP_PRIME) {
        cap->value = DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT;
        return 0;
    }
    if (cap->capability == DRM_CAP_SYNCOBJ ||
        cap->capability == DRM_CAP_SYNCOBJ_TIMELINE)
        return 0;
    if (cap->capability == DRM_CAP_DUMB_BUFFER) {
        cap->value = 1;
        return 0;
    }
    if (cap->capability == DRM_CAP_DUMB_PREFERRED_DEPTH) {
        cap->value = 32;
        return 0;
    }
    if (cap->capability == DRM_CAP_DUMB_PREFER_SHADOW) {
        cap->value = 0;
        return 0;
    }
    /* This stage has no KMS objects, syncobj, or vblank ABI. */
    return -LINUX_EOPNOTSUPP;
}

static int drm_ioctl_dispatch(struct task *task, struct drm_file *file,
                              const struct drm_ioctl_desc *desc,
                              uint32_t command, void *data,
                              uint32_t *created_handle_out,
                              uint32_t *created_fb_out,
                              int *created_fd_out,
                              uint32_t *created_object_out,
                              struct drm_faildiag_context *diagnostics)
{
    const struct drm_driver *driver = file->device->driver;
    switch (desc->handler) {
    case DRM_HANDLER_VERSION:
        return drm_ioctl_version(task, driver, data);
    case DRM_HANDLER_GET_UNIQUE:
        return drm_ioctl_get_unique(task, file, data);
    case DRM_HANDLER_GET_MAGIC:
        return drm_ioctl_get_magic(file, data);
    case DRM_HANDLER_GET_CAP:
        return drm_ioctl_get_cap(driver, data);
    case DRM_HANDLER_SET_CLIENT_CAP: {
        const struct drm_set_client_cap *cap = data;
        if (driver->universal_planes && cap->capability == DRM_CLIENT_CAP_UNIVERSAL_PLANES &&
            cap->value <= 1) {
            file->universal_planes = cap->value;
            return 0;
        }
        if (driver->atomic_modeset && cap->capability == DRM_CLIENT_CAP_ATOMIC) {
            if (cap->value > 1)
                return -LINUX_EINVAL;
            file->atomic_modeset = file->universal_planes = cap->value;
            if (!cap->value)
                file->cursor_hotspot = false;
            return 0;
        }
        if (driver->cursor_hotspot && cap->capability == DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT) {
            if (!file->atomic_modeset || cap->value > 1)
                return -LINUX_EINVAL;
            file->cursor_hotspot = cap->value;
            return 0;
        }
        return -LINUX_EOPNOTSUPP;
    }
    case DRM_HANDLER_SET_VERSION:
        return drm_ioctl_set_version(file, data);
    case DRM_HANDLER_SET_UNIQUE:
        return -LINUX_EINVAL;
    case DRM_HANDLER_AUTH_MAGIC:
        return drm_ioctl_auth_magic(file, data);
    case DRM_HANDLER_SET_MASTER:
        return drm_file_set_master(task, file);
    case DRM_HANDLER_DROP_MASTER:
        return drm_file_drop_master(task, file);
    case DRM_HANDLER_BO:
        return drm_bo_ioctl(file, command, data, created_handle_out,
                            diagnostics);
    case DRM_HANDLER_PRIME:
        return drm_prime_ioctl(task, file, command, data, created_fd_out,
                               created_handle_out, created_object_out);
    case DRM_HANDLER_KMS:
        return drm_kms_ioctl(task, file, command, data, created_fb_out);
    case DRM_HANDLER_UNSUPPORTED:
        return drm_kms_ioctl(task, file, desc->command, data, created_fb_out);
    case DRM_HANDLER_DRIVER:
        return driver->ioctl(task, file, desc->command, data, created_handle_out,
                             created_fd_out, created_object_out);
    }
    return -LINUX_EINVAL;
}

int64_t drm_file_ioctl(struct task *task, struct task_file *task_file,
                       uint32_t command, uint64_t argument)
{
    struct drm_file *file = task_file ? task_file->drm_file : NULL;
    const struct drm_ioctl_desc *desc;
    struct drm_ioctl_desc private;
    uint32_t size, input_size, output_size, kernel_size;
    uint8_t local_data[128];
    uint8_t *data = local_data;
    bool allocated = false;
    uint32_t created_handle = 0;
    uint32_t created_fb = 0;
    uint32_t created_object = 0;
    int created_fd = -1;
    struct drm_faildiag_context diagnostics = {
        .pid = task ? task->pid : 0,
        .command = command,
        .handler = DRM_HANDLER_UNSUPPORTED,
        .handler_name = "unknown",
    };
    int ret;

    if (!file) return -LINUX_EBADF;
    if (_IOC_TYPE(command) != DRM_IOCTL_BASE) return -LINUX_ENOTTY;
    desc = drm_ioctl_find(file, command, &private);
    if (!desc) {
        drm_faildiag_record(&diagnostics, -LINUX_EINVAL, "ioctl.unknown",
                            NULL, 0);
        return -LINUX_EINVAL;
    }
    diagnostics.handler = (uint32_t)desc->handler;
    diagnostics.handler_name = drm_ioctl_handler_name(desc->handler);

    size = _IOC_SIZE(command);
    input_size = size;
    output_size = size;
    if (!(command & desc->command & DRM_IOCTL_REQUIRED_IN)) input_size = 0;
    if (!(command & desc->command & DRM_IOCTL_REQUIRED_OUT)) output_size = 0;
    kernel_size = input_size > output_size ? input_size : output_size;
    if (_IOC_SIZE(desc->command) > kernel_size)
        kernel_size = _IOC_SIZE(desc->command);
    if (kernel_size > sizeof(local_data)) {
        data = kernel_malloc(kernel_size);
        if (!data) {
            const struct drm_faildiag_field fields[] = {
                {"argument_bytes", kernel_size},
            };
            drm_faildiag_record(&diagnostics, -LINUX_ENOMEM,
                                "ioctl.argument_buffer", fields, 1);
            return -LINUX_ENOMEM;
        }
        allocated = true;
    }
    if (kernel_size) __builtin_memset(data, 0, kernel_size);
    ret = 0;
    if (input_size) {
#ifdef CONFIG_SWAP
        ret=drm_user_import(task,data,argument,input_size);
        if(ret)goto out;
#else
        if (user_copy_from_task(task, data, argument, input_size) < 0) {
            ret = -LINUX_EFAULT;
            goto out;
        }
#endif
    }
    /* Linux copies the encoded input payload before checking DRM_AUTH,
     * DRM_MASTER, and DRM_ROOT_ONLY permissions. Preserve that errno order. */
    ret = drm_ioctl_check_permission(task, file, desc->permission);
#ifdef CONFIG_SWAP
    if (output_size && drm_user_prepare(task,argument,output_size,true)==KERNEL_SYSCALL_SWAP_CONTENDED) {
        ret=KERNEL_SYSCALL_SWAP_CONTENDED;goto out;
    }
#endif
    if (!ret) ret = drm_ioctl_dispatch(task, file, desc, command, data,
                                       &created_handle, &created_fb,
                                       &created_fd, &created_object, &diagnostics);
#ifdef CONFIG_SWAP
    if (ret == KERNEL_SYSCALL_SWAP_CONTENDED) goto out;
#endif
    if (output_size && user_copy_to_task(task, argument, data, output_size) < 0) {
        const struct drm_driver *driver = file->device->driver;
        if (!ret && (desc->handler == DRM_HANDLER_DRIVER || created_object ||
                     desc->command == DRM_IOCTL_MODE_CREATEPROPBLOB) && driver->ioctl_rollback)
            driver->ioctl_rollback(file, desc->command, data, created_object);
        ret = -LINUX_EFAULT;
        if (created_handle) (void)drm_bo_handle_delete(file, created_handle);
        if (created_fb) drm_kms_remove_fb(file, created_fb);
        if (created_fd >= 0) task_discard_file_fd(task, created_fd);
    }
out:
    if (allocated) kernel_free(data);
    if (ret < 0 && ret != KERNEL_SYSCALL_SWAP_CONTENDED && !diagnostics.attempted)
        drm_faildiag_record(&diagnostics, ret,
            desc->handler == DRM_HANDLER_BO ? "bo.ioctl" : "ioctl.dispatch", NULL, 0);
    return ret;
}

/**
 * @brief Route the native ioctl operation to a DRM card or opaque PRIME OFD.
 * @param task Calling task used to validate user memory and select its fd state.
 * @param file Resolved shared open-file description for the descriptor.
 * @param command 32-bit Linux ioctl command from the x86-64 syscall ABI.
 * @param argument Full 64-bit syscall argument, preserved for usercopy.
 * @return The selected card or PRIME ioctl result, or negative EBADF when the
 *         open-file description is not a supported DRM object.
 */
int64_t drm_fd_ioctl(struct task *task, struct task_file *file,
                     uint32_t command, uint64_t argument)
{
    if (!task || !file) return -LINUX_EBADF;
    if (file->drm_prime_file)
        return drm_prime_file_ioctl(task, file, command, argument);
    if (file->drm_file)
        return drm_file_ioctl(task, file, command, argument);
    return -LINUX_EBADF;
}
