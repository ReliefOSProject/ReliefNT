#include <reliefnt/export.h>
#include <reliefnt/permissions.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/heap.h>
#include <reliefnt/pty.h>
#include <linux/mount.h>

/**
 * @brief Check filesystem or real primary and supplementary group membership.
 * @param task Borrowed task credentials, NULL for the kernel root-group default.
 * @param gid Group identity to compare.
 * @param real_ids Use the real primary gid instead of fsgid when true.
 * @return True for a matching group; a NULL task matches only gid zero.
 * @par Context
 * Execution-owned credential snapshot; no reference, allocation, user access or capability override.
 */
bool task_in_group(const struct task *task, uint32_t gid, bool real_ids)
{
    if (!task) return !gid;
    uint32_t primary = real_ids ? task->gid : task->fsgid;
    if (gid == primary) return true;
    const struct task_groups *groups = task->groups;
    if (groups) for (uint32_t i = 0; i < groups->count; ++i)
        if (groups->ids[i] == gid) return true;
    return false;
}

void task_groups_retain(struct task_groups *groups)
{
    if (groups) __atomic_add_fetch(&groups->references, 1, __ATOMIC_RELAXED);
}

void task_groups_release(struct task *task)
{
    struct task_groups *groups = task->groups;
    task->groups = NULL;
    if (groups && !__atomic_sub_fetch(&groups->references, 1, __ATOMIC_ACQ_REL)) kernel_free(groups);
}

int task_groups_set(struct task *task, const uint32_t *ids, uint32_t count)
{
    if (!task || count > 65536u) return -RELIEFOS_EINVAL;
    struct task_groups *groups = NULL;
    if (count) {
        groups = kernel_malloc(sizeof(*groups) + (size_t)count * sizeof(*ids));
        if (!groups) return -RELIEFOS_ENOMEM;
        groups->references = 1;
        groups->count = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (ids[i] == UINT32_MAX) { kernel_free(groups); return -RELIEFOS_EINVAL; }
            groups->ids[i] = ids[i];
        }
        /* Linux exposes the sorted supplementary list, retaining duplicates.
         * Heapsort bounds work for the full NGROUPS_MAX input. */
        for (uint32_t i = 1; i < count; ++i) {
            uint32_t child = i;
            while (child && groups->ids[(child - 1) / 2] < groups->ids[child]) {
                uint32_t parent = (child - 1) / 2, tmp = groups->ids[parent];
                groups->ids[parent] = groups->ids[child]; groups->ids[child] = tmp;
                child = parent;
            }
        }
        for (uint32_t end = count - 1; end; --end) {
            uint32_t tmp = groups->ids[0]; groups->ids[0] = groups->ids[end]; groups->ids[end] = tmp;
            uint32_t parent = 0;
            while (parent * 2 + 1 < end) {
                uint32_t child = parent * 2 + 1;
                if (child + 1 < end && groups->ids[child] < groups->ids[child + 1]) ++child;
                if (groups->ids[parent] >= groups->ids[child]) break;
                tmp = groups->ids[parent]; groups->ids[parent] = groups->ids[child]; groups->ids[child] = tmp;
                parent = child;
            }
        }
    }
    task_groups_release(task);
    task->groups = groups;
    return 0;
}

static struct {
    bool used;
    uint32_t kind, volume;
    struct reliefos_permissions value;
} device_modes[128];

static int device_metadata(const struct storage_node *node,
                           struct reliefos_permissions *value, bool write)
{
    unsigned free_slot = 128;
    for (unsigned i = 0; i < 128; ++i) {
        if (!device_modes[i].used) { if (free_slot == 128) free_slot = i; continue; }
        if (device_modes[i].kind == node->first_cluster && device_modes[i].volume == node->volume_id) {
            if (write) device_modes[i].value = *value;
            else *value = device_modes[i].value;
            return 0;
        }
    }
    if (!write) {
        uint32_t mode = node->type == RELIEFOS_FS_TYPE_DIR ? 0755 : 0666;
        if (node->flags & STORAGE_NODE_FLAG_DEV_BLOCK) mode = 0600;
        uint32_t gid = 0;
        if (node->first_cluster == STORAGE_DEV_KIND_DRM_CARD0) {
            mode = 0660;
            gid = 44; /* Linux video group. */
        } else if (node->first_cluster ==
                   STORAGE_DEV_KIND_DRM_RENDERD128) {
            mode = 0660;
            gid = 44; /* Linux video group. */
        }
        *value = (struct reliefos_permissions){mode, 0, gid};
        return 0;
    }
    if (free_slot == 128) return -RELIEFOS_ENOSPC;
    device_modes[free_slot].used = true;
    device_modes[free_slot].kind = node->first_cluster;
    device_modes[free_slot].volume = node->volume_id;
    device_modes[free_slot].value = *value;
    return 0;
}

static bool metadata_path(const char *path)
{
    const char *base = path;
    for (const char *p = path; *p; ++p) if (*p == '/') base = p + 1;
    const char *name = "LEONACL.SYS";
    while (*base && *name) {
        char ch = *base++;
        if (ch >= 'a' && ch <= 'z') ch -= 'a' - 'A';
        if (ch != *name++) return false;
    }
    return !*base && !*name;
}

int fs_virtual_permissions(const char *intrinsic,const struct storage_node *node,
                              struct reliefos_permissions *value,bool write)
{
    if(!intrinsic || !node || !value)return -RELIEFOS_EINVAL;
    if(node->flags&STORAGE_NODE_FLAG_PTY)return pty_inode_permissions(node,value,write);
    if(node->flags&STORAGE_NODE_FLAG_DEV_LINK) {
        if(write)return -RELIEFOS_EROFS;
        *value=(struct reliefos_permissions){0777,0,0};return 0;
    }
    if(node->flags&(STORAGE_NODE_FLAG_DEV_NODE|STORAGE_NODE_FLAG_DEV_DIR|STORAGE_NODE_FLAG_DEV_FB0))
        return device_metadata(node,value,write);
    if(!(node->flags&(STORAGE_NODE_FLAG_PROC|STORAGE_NODE_FLAG_SYSFS)))return -RELIEFOS_EOPNOTSUPP;
    if(write)return -RELIEFOS_EROFS;
    uint32_t mode=node->type==RELIEFOS_FS_TYPE_DIR ? 0555 : node->type==RELIEFOS_FS_TYPE_SYMLINK ? 0777 : 0444;
    if(node->flags&STORAGE_NODE_FLAG_SYSFS) {
        const char *base=intrinsic;
        for(const char *p=intrinsic;*p;++p)if(*p=='/')base=p+1;
        if(!__builtin_strcmp(base,"product_serial") || !__builtin_strcmp(base,"product_uuid") ||
           !__builtin_strcmp(base,"board_serial") || !__builtin_strcmp(base,"config"))mode=0400;
    }
    *value=(struct reliefos_permissions){mode,0,0};return 0;
}

/* Shared DAC policy; the caller selects exactly one inode metadata authority. */
static int check_mode(const struct task *task,const struct storage_node *node,
                          const struct reliefos_permissions *value,uint32_t access,bool real_ids)
{
    uint32_t uid = task ? (real_ids ? task->uid : task->fsuid) : 0;
    uint64_t caps = !task ? UINT64_MAX : real_ids ?
        (task->uid ? 0 : task->cap_permitted) : task->cap_effective;
    uint32_t shift = uid == value->uid ? 6 : task_in_group(task, value->gid, real_ids) ? 3 : 0;
    if (((value->mode >> shift) & access) == access) return 0;
    if ((caps & (1ULL << CAP_DAC_OVERRIDE)) &&
        (!(access & FS_ACCESS_EXEC) || node->type == RELIEFOS_FS_TYPE_DIR || (value->mode & 0111u))) return 0;
    if ((caps & (1ULL << CAP_DAC_READ_SEARCH)) &&
        (node->type == RELIEFOS_FS_TYPE_DIR ? !(access & FS_ACCESS_WRITE) : access == FS_ACCESS_READ)) return 0;
    return -RELIEFOS_EACCES;
}

int fs_permissions_check_path(const struct task *task,const struct vfs_path *path,
                                 uint32_t access,bool real_ids)
{
    const struct storage_node *node=vfs_path_node(path);
    if(!node || access&~(FS_ACCESS_READ|FS_ACCESS_WRITE|FS_ACCESS_EXEC))return -RELIEFOS_EINVAL;
    const char *name=vfs_path_name(path);
    if((access&FS_ACCESS_WRITE) && name && metadata_path(name))return -RELIEFOS_EPERM;
    uint64_t flags=vfs_path_mount_flags(path);
    /* Linux inode_permission enforces readonly for regular files, directories
     * and symlinks; device/FIFO/socket data is not stored on that mount. */
    if((access&FS_ACCESS_WRITE) && (flags&MS_RDONLY) &&
       (node->type==RELIEFOS_FS_TYPE_FILE || node->type==RELIEFOS_FS_TYPE_DIR ||
        node->type==RELIEFOS_FS_TYPE_SYMLINK))return -RELIEFOS_EROFS;
    if(node->type==RELIEFOS_FS_TYPE_FILE && (access&FS_ACCESS_EXEC) && (flags&MS_NOEXEC))return -RELIEFOS_EACCES;
    struct reliefos_permissions value;int ret=vfs_path_permissions(path,&value,false);
    return ret<0 ? ret : check_mode(task,node,&value,access,real_ids);
}

/** @brief Apply the common chmod policy without performing metadata I/O.
 * @param task Borrowed credentials, NULL for a trusted kernel caller.
 * @param node Borrowed inode type; caller retains its backing object.
 * @param value Caller-owned current metadata, updated only after authorization.
 * @param mode Requested permission and special bits.
 * @return Zero or negative errno. No locks or references are acquired.
 */
static int chmod_value(const struct task *task,const struct storage_node *node,
                       struct reliefos_permissions *value,uint32_t mode)
{
    if (task && task->fsuid != value->uid && !(task->cap_effective & (1ULL << CAP_FOWNER)))
        return -RELIEFOS_EPERM;
    if (node->type == RELIEFOS_FS_TYPE_SYMLINK) return -RELIEFOS_EOPNOTSUPP;
    value->mode = (value->mode & LINUX_S_IFMT) | (mode & 07777u);
    if (task && !(task->cap_effective & (1ULL << CAP_FSETID)) &&
        !task_in_group(task, value->gid, false)) value->mode &= ~02000u;
    return 0;
}

/** @brief Apply the common ownership/set-ID policy without metadata I/O.
 * @param task Borrowed credentials, NULL for a trusted kernel caller.
 * @param node Borrowed inode type with caller-retained lifetime.
 * @param value Caller-owned metadata, updated only after authorization.
 * @param uid New user ID, UINT32_MAX to retain the existing ID.
 * @param gid New group ID, UINT32_MAX to retain the existing ID.
 * @return Zero or negative errno. No locks or references are acquired.
 */
static int chown_value(const struct task *task,const struct storage_node *node,
                       struct reliefos_permissions *value,uint32_t uid,uint32_t gid)
{
    bool drop_sgid = (value->mode & 02000u) &&
        ((value->mode & 0010u) || (task && !(task->cap_effective & (1ULL << CAP_FSETID)) &&
                                 !task_in_group(task, value->gid, false)));
    if (task) {
        bool owner = task->fsuid == value->uid;
        bool chown_cap = (task->cap_effective & (1ULL << CAP_CHOWN)) != 0;
        if (uid != UINT32_MAX && (!owner || uid != value->uid) && !chown_cap) return -RELIEFOS_EPERM;
        if (gid != UINT32_MAX && (!owner || (gid != value->gid && !task_in_group(task, gid, false))) &&
            !chown_cap) return -RELIEFOS_EPERM;
        if (node->type != RELIEFOS_FS_TYPE_DIR && ((value->mode & 04000u) || drop_sgid) &&
            !owner && !(task->cap_effective & (1ULL << CAP_FOWNER))) return -RELIEFOS_EPERM;
    }
    if (uid != UINT32_MAX) value->uid = uid;
    if (gid != UINT32_MAX) value->gid = gid;
    if (node->type != RELIEFOS_FS_TYPE_DIR) {
        value->mode &= ~04000u;
        if (drop_sgid) value->mode &= ~02000u;
    }
    return 0;
}

/** @brief Change only the admitted implicit virtual endpoint's mode.
 * @param task Borrowed filesystem credentials, NULL for trusted kernel access.
 * @param intrinsic Borrowed provider key, never a namespace lookup operand.
 * @param node Borrowed synthetic inode retained by endpoint admission.
 * @param mode New permission/special bits.
 * @return Zero or negative metadata/type/authorization errno. Common owner,
 * capability and set-ID policy applies; regular disk inodes are unsupported.
 * Execution ownership required; no locks or new inode references are taken.
 */
int fs_virtual_chmod(const struct task *task,const char *intrinsic,
                         const struct storage_node *node,uint32_t mode)
{
    struct reliefos_permissions value;
    int rc=fs_virtual_permissions(intrinsic,node,&value,false);
    if(!rc)rc=chmod_value(task,node,&value,mode);
    if(!rc)rc=fs_virtual_permissions(intrinsic,node,&value,true);
    return rc;
}

/** @brief Change only the admitted implicit virtual endpoint's ownership.
 * @param task Borrowed filesystem credentials, NULL for trusted kernel access.
 * @param intrinsic Borrowed provider key, never a namespace lookup operand.
 * @param node Borrowed synthetic inode retained by endpoint admission.
 * @param uid New user ID, UINT32_MAX retains it.
 * @param gid New group ID, UINT32_MAX retains it.
 * @return Zero or negative metadata/type/authorization errno. Common owner,
 * capability and set-ID policy applies; no disk/name fallback exists. Caller
 * owns execution and endpoint lifetime; no locks or inode refs are acquired.
 */
int fs_virtual_chown(const struct task *task,const char *intrinsic,
                         const struct storage_node *node,uint32_t uid,uint32_t gid)
{
    struct reliefos_permissions value;
    int rc=fs_virtual_permissions(intrinsic,node,&value,false);
    if(!rc)rc=chown_value(task,node,&value,uid,gid);
    if(!rc)rc=fs_virtual_permissions(intrinsic,node,&value,true);
    return rc;
}

int fs_permissions_chmod_path(const struct task *task,const struct vfs_path *path,uint32_t mode)
{
    if (!vfs_path_node(path)) return -RELIEFOS_EINVAL;
    if (vfs_path_mount_flags(path) & MS_RDONLY) return -RELIEFOS_EROFS;
    struct reliefos_permissions value;
    int ret = vfs_path_permissions(path,&value,false);
    if (!ret) ret = chmod_value(task,vfs_path_node(path),&value,mode);
    if (!ret) ret = vfs_path_permissions(path,&value,true);
    return ret;
}

int fs_permissions_chown_path(const struct task *task,const struct vfs_path *path,uint32_t uid,uint32_t gid)
{
    if (!vfs_path_node(path)) return -RELIEFOS_EINVAL;
    if (vfs_path_mount_flags(path) & MS_RDONLY) return -RELIEFOS_EROFS;
    struct reliefos_permissions value;
    int ret = vfs_path_permissions(path,&value,false);
    if (!ret) ret = chown_value(task,vfs_path_node(path),&value,uid,gid);
    if (!ret) ret = vfs_path_permissions(path,&value,true);
    return ret;
}

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(task_in_group);
