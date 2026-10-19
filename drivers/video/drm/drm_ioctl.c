#include "drm_internal.h"

#include <drm/drm_mode.h>
#include <drm/vmwgfx_drm.h>
#include <linux/capability.h>
#include <linux/errno.h>
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/text_stream.h>
#include <reliefnt/usercopy.h>

#define DRM_IOCTL_REQUIRED_IN  (_IOC_WRITE << _IOC_DIRSHIFT)
#define DRM_IOCTL_REQUIRED_OUT (_IOC_READ << _IOC_DIRSHIFT)
#define DRM_IOCTL_INTERFACE_MAJOR 1
#define DRM_IOCTL_INTERFACE_MINOR 4
#define DRM_IOCTL_DRIVER_MAJOR 2
#define DRM_IOCTL_DRIVER_MINOR 1

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
    DRM_HANDLER_VMW_CAPS,
    DRM_HANDLER_VMW_RESOURCE,
    DRM_HANDLER_VMW_EXECBUF,
};

enum drm_ioctl_permission {
    DRM_PERMISSION_AUTH = 1u << 0,
    DRM_PERMISSION_MASTER = 1u << 1,
    DRM_PERMISSION_ADMIN = 1u << 2,
    DRM_PERMISSION_RENDER_ALLOW = 1u << 3,
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
    case DRM_HANDLER_VMW_CAPS: return "vmw_caps";
    case DRM_HANDLER_VMW_RESOURCE: return "vmw_resource";
    case DRM_HANDLER_VMW_EXECBUF: return "execbuf";
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

static const char *drm_ioctl_failure_stage(
    const struct drm_ioctl_desc *desc, uint32_t command)
{
    if (!desc) return "ioctl.return";
    if (desc->handler == DRM_HANDLER_VMW_RESOURCE) {
        switch (_IOC_NR(command)) {
        case DRM_COMMAND_BASE + DRM_VMW_CREATE_CONTEXT:
            return "resource.context";
        case DRM_COMMAND_BASE + DRM_VMW_CREATE_SURFACE:
            return "resource.surface";
        default:
            return "resource.ioctl";
        }
    }
    if (desc->handler == DRM_HANDLER_VMW_EXECBUF) {
        switch (_IOC_NR(command)) {
        case DRM_COMMAND_BASE + DRM_VMW_EXECBUF:
            return "execbuf.return";
        case DRM_COMMAND_BASE + DRM_VMW_FENCE_WAIT:
            return "fence.wait";
        case DRM_COMMAND_BASE + DRM_VMW_FENCE_SIGNALED:
            return "fence.signaled";
        default:
            return "execbuf.ioctl";
        }
    }
    if (desc->handler == DRM_HANDLER_BO) return "bo.ioctl";
    return "ioctl.dispatch";
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
    /* These vmwgfx operations carry Linux DRM_RENDER_ALLOW. The render
     * permission is explicit; all object authority remains per-file. */
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_ALLOC_BO,
                          union drm_vmw_alloc_bo_arg),
      .handler = DRM_HANDLER_BO, .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_DMABUF,
                         struct drm_vmw_handle_close_arg),
      .handler = DRM_HANDLER_BO, .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_SYNCCPU,
      struct drm_vmw_synccpu_arg),
      .handler = DRM_HANDLER_VMW_EXECBUF,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_GET_PARAM,
                          struct drm_vmw_getparam_arg),
      .handler = DRM_HANDLER_VMW_CAPS,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOR(DRM_COMMAND_BASE + DRM_VMW_CREATE_CONTEXT,
                         struct drm_vmw_context_arg),
      .handler = DRM_HANDLER_VMW_RESOURCE,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_CONTEXT,
                         struct drm_vmw_context_arg),
      .handler = DRM_HANDLER_VMW_RESOURCE,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_CREATE_SURFACE,
                          union drm_vmw_surface_create_arg),
      .handler = DRM_HANDLER_VMW_RESOURCE,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_UNREF_SURFACE,
                         struct drm_vmw_surface_arg),
      .handler = DRM_HANDLER_VMW_RESOURCE,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_REF_SURFACE,
                          union drm_vmw_surface_reference_arg),
      .handler = DRM_HANDLER_VMW_RESOURCE,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_GET_3D_CAP,
                         struct drm_vmw_get_3d_cap_arg),
      .handler = DRM_HANDLER_VMW_CAPS,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_EXECBUF,
                         struct drm_vmw_execbuf_arg),
      .handler = DRM_HANDLER_VMW_EXECBUF,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_FENCE_WAIT,
                          struct drm_vmw_fence_wait_arg),
      .handler = DRM_HANDLER_VMW_EXECBUF,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOWR(DRM_COMMAND_BASE + DRM_VMW_FENCE_SIGNALED,
                          struct drm_vmw_fence_signaled_arg),
      .handler = DRM_HANDLER_VMW_EXECBUF,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
    { .command = DRM_IOW(DRM_COMMAND_BASE + DRM_VMW_FENCE_UNREF,
                         struct drm_vmw_fence_arg),
      .handler = DRM_HANDLER_VMW_EXECBUF,
      .permission = DRM_PERMISSION_RENDER_ALLOW },
};

static const struct drm_ioctl_desc *drm_ioctl_find(uint32_t command)
{
    uint32_t type = _IOC_TYPE(command);
    uint32_t number = _IOC_NR(command);
    if (type != DRM_IOCTL_BASE) return NULL;
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

static int drm_ioctl_version(struct task *task, struct drm_version *version)
{
    int ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->name,
                             &version->name_len, "vmwgfx");
    if (!ret)
        ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->date,
                             &version->date_len, "0");
    if (!ret)
        ret = drm_copy_field(task, (uint64_t)(uintptr_t)version->desc,
                             &version->desc_len,
                             "Linux drm driver for VMware graphics devices");
    version->version_major = DRM_IOCTL_DRIVER_MAJOR;
    version->version_minor = DRM_IOCTL_DRIVER_MINOR;
    version->version_patchlevel = 0;
    return ret;
}

static int drm_ioctl_get_unique(struct task *task, struct drm_file *file,
                               struct drm_unique *unique)
{
    struct drm_master *master = file->master;
    if (!master) return -LINUX_EINVAL;
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
        (version->drm_dd_major != DRM_IOCTL_DRIVER_MAJOR ||
         version->drm_dd_minor < 0 ||
         version->drm_dd_minor > DRM_IOCTL_DRIVER_MINOR))
        ret = -LINUX_EINVAL;
out:
    version->drm_di_major = DRM_IOCTL_INTERFACE_MAJOR;
    version->drm_di_minor = DRM_IOCTL_INTERFACE_MINOR;
    version->drm_dd_major = DRM_IOCTL_DRIVER_MAJOR;
    version->drm_dd_minor = DRM_IOCTL_DRIVER_MINOR;
    return ret;
}

static int drm_ioctl_get_cap(struct drm_get_cap *cap)
{
    cap->value = 0;
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
                              struct drm_vmwgfx_rollback_receipt *resource_receipt,
                              struct drm_faildiag_context *diagnostics,
                              uint64_t user_argument)
{
    switch (desc->handler) {
    case DRM_HANDLER_VERSION:
        return drm_ioctl_version(task, data);
    case DRM_HANDLER_GET_UNIQUE:
        return drm_ioctl_get_unique(task, file, data);
    case DRM_HANDLER_GET_MAGIC:
        return drm_ioctl_get_magic(file, data);
    case DRM_HANDLER_GET_CAP:
        return drm_ioctl_get_cap(data);
    case DRM_HANDLER_SET_CLIENT_CAP:
        return -LINUX_EOPNOTSUPP;
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
                               resource_receipt);
    case DRM_HANDLER_KMS:
        return drm_kms_ioctl(task, file, command, data, created_fb_out);
    case DRM_HANDLER_VMW_CAPS:
        return drm_vmwgfx_caps_ioctl(task, file, command, data);
    case DRM_HANDLER_VMW_RESOURCE:
        return drm_vmwgfx_resource_ioctl(task, file, command, data,
                                         resource_receipt);
    case DRM_HANDLER_VMW_EXECBUF:
        return (int)drm_vmwgfx_execbuf_ioctl(task, file, command, data,
                                             user_argument, diagnostics);
    case DRM_HANDLER_UNSUPPORTED:
        return -LINUX_EOPNOTSUPP;
    }
    return -LINUX_EINVAL;
}

int64_t drm_file_ioctl(struct task *task, struct task_file *task_file,
                       uint32_t command, uint64_t argument)
{
    struct drm_file *file = task_file ? task_file->drm_file : NULL;
    const struct drm_ioctl_desc *desc;
    uint32_t size, input_size, output_size, kernel_size;
    uint8_t local_data[128];
    uint8_t *data = local_data;
    bool allocated = false;
    uint32_t created_handle = 0;
    uint32_t created_fb = 0;
    int created_fd = -1;
    struct drm_vmwgfx_rollback_receipt resource_receipt = {0};
    struct drm_faildiag_context diagnostics = {
        .pid = task ? task->pid : 0,
        .command = command,
        .handler = DRM_HANDLER_UNSUPPORTED,
        .handler_name = "unknown",
    };
    bool continuation_handled = false;
    bool skip_input = false;
    int ret;

    if (!file) return -LINUX_EBADF;
    if (_IOC_TYPE(command) != DRM_IOCTL_BASE) return -LINUX_ENOTTY;
    desc = drm_ioctl_find(command);
    if (!desc) {
        /* vmwgfx private ioctls and unknown core slots are not part of this
         * primary-node compatibility slice. */
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
    ret = (int)drm_vmwgfx_prepare_ioctl_continuation(task, task_file, file,
        command, argument, data, kernel_size, &continuation_handled,
        &skip_input);
    if (continuation_handled) goto out;
    if (ret) goto out;
    if (input_size && !skip_input) {
        if (user_copy_from_task(task, data, argument, input_size) < 0) {
            ret = -LINUX_EFAULT;
            goto out;
        }
    }
    /* Linux copies the encoded input payload before checking DRM_AUTH,
     * DRM_MASTER, and DRM_ROOT_ONLY permissions. Preserve that errno order. */
    ret = drm_ioctl_check_permission(task, file, desc->permission);
    if (!ret) ret = drm_ioctl_dispatch(task, file, desc, command, data,
                                       &created_handle, &created_fb,
                                       &created_fd,
                                       &resource_receipt, &diagnostics,
                                       argument);
    if (output_size && user_copy_to_task(task, argument, data, output_size) < 0) {
        ret = -LINUX_EFAULT;
        if (created_handle) (void)drm_bo_handle_delete(file, created_handle);
        if (created_fb) drm_kms_remove_fb(file, created_fb);
        if (created_fd >= 0) task_discard_file_fd(task, created_fd);
        drm_vmwgfx_resource_rollback(file, &resource_receipt);
    }
out:
    if (allocated) kernel_free(data);
    if (ret < 0 && !diagnostics.attempted)
        drm_faildiag_record(&diagnostics, ret,
            drm_ioctl_failure_stage(desc, command), NULL, 0);
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
