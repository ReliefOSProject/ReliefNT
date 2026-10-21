#include "../internal.h"

/** @brief Encode a standard audio node as Linux ALSA major/minor device ID.
 * @param node Storage node carrying an audio control or PCM identity.
 * @return Linux old-style dev_t value, or zero for a stale identity.
 */
static uint64_t linux_audio_rdev(const struct storage_node *node);


uint64_t linux_stat_inode(const char *path, const struct reliefos_stat *st)
{

    uint64_t hash = 1469598103934665603ULL;
    if (path) {
        for (const unsigned char *p = (const unsigned char *)path; *p; ++p) {
            hash ^= *p;
            hash *= 1099511628211ULL;
        }
    }
    if (st) hash ^= st->size + ((uint64_t)st->type << 32);
    hash &= 0x7fffffffffffffffULL;
    return hash ? hash : 1;
}

/** @brief Encode a standard audio node as Linux ALSA major/minor device ID.
 * @param node Storage node carrying an audio control or PCM identity.
 * @return Linux old-style dev_t value, or zero for a stale identity.
 */
static uint64_t linux_audio_rdev(const struct storage_node *node)
{
    uint32_t card_token;
    uint32_t ordinal;
    uint32_t minor;
    int index;

    if (!node) return 0;
    if ((node->flags & STORAGE_NODE_FLAG_DEV_NODE) &&
        !(node->flags & STORAGE_NODE_FLAG_AUDIO_PCM) &&
        node->first_cluster == STORAGE_DEV_KIND_AUDIO)
        return ((uint64_t)14u << 8) | 3u;
    if ((node->flags & STORAGE_NODE_FLAG_AUDIO_MIXER) &&
        node->first_cluster == STORAGE_DEV_KIND_AUDIO_MIXER)
        return (uint64_t)14u << 8;
    if (node->flags & STORAGE_NODE_FLAG_AUDIO_TIMER)
        return ((uint64_t)116u << 8) | 33u;
    if (!(node->flags & (STORAGE_NODE_FLAG_AUDIO_PCM |
                                  STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                  STORAGE_NODE_FLAG_AUDIO_MIXER |
                                  STORAGE_NODE_FLAG_AUDIO_TIMER))) return 0;
    if (node->flags & STORAGE_NODE_FLAG_AUDIO_PCM)
        card_token = AUDIO_DEVICE_CARD(node->volume_id);
    else
        card_token = node->volume_id;
    index = audio_card_index(card_token);
    if (index < 0) return 0;
    ordinal = (uint32_t)index;
    if (node->flags & STORAGE_NODE_FLAG_AUDIO_MIXER) {
        minor = ordinal * 16u;
        return ((uint64_t)14u << 8) | (minor & 0xffu) |
            ((uint64_t)(minor & ~0xffu) << 12);
    }
    if (node->flags & STORAGE_NODE_FLAG_AUDIO_CONTROL) {
        minor = ordinal * 32u;
    } else {
        uint32_t device = AUDIO_DEVICE_INDEX(node->volume_id);
        uint32_t direction = AUDIO_DEVICE_DIRECTION(node->volume_id);
        if (device > 7u) return 0;
        minor = ordinal * 32u + (direction ? 24u : 16u) + device;
    }
    return ((uint64_t)116u << 8) | (minor & 0xffu) |
        ((uint64_t)(minor & ~0xffu) << 12);
}

int linux_stat_from_legacy(struct linux_stat_abi *out,
                                   const struct reliefos_stat *st,
                                   const char *path, const struct storage_node *node)
{
    struct storage_node found;
    if (!node && path && storage_lookup_path(path, &found) == 0) node = &found;
    if (node && (node->flags & (STORAGE_NODE_FLAG_EXT2 | STORAGE_NODE_FLAG_TMPFS))) return storage_inode_stat(node, out);
    uint32_t type = st ? st->type : RELIEFOS_FS_TYPE_FILE;
    uint32_t mode = type == RELIEFOS_FS_TYPE_DIR ? 0040755u :
                    type == RELIEFOS_FS_TYPE_FIFO ? LINUX_S_IFIFO | 0666u :
                    type == RELIEFOS_FS_TYPE_SOCKET ? LINUX_S_IFSOCK | 0777u :
                    type == RELIEFOS_FS_TYPE_SYMLINK ? 0120777u :
                    type == RELIEFOS_FS_TYPE_DEVICE ?
                        ((node && (node->flags & STORAGE_NODE_FLAG_DEV_BLOCK)) ? 0060660u : 0020660u) : 0100644u;
    if (node && (node->first_cluster == STORAGE_DEV_KIND_DRM_CARD0 ||
                 node->first_cluster == STORAGE_DEV_KIND_DRM_RENDERD128))
        mode = LINUX_S_IFCHR | 0660u;
    *out = (struct linux_stat_abi){0};
    out->st_dev = node && (node->flags & STORAGE_NODE_FLAG_SYSFS) ? STORAGE_SYSFS_DEVICE :
        node && (node->flags & STORAGE_NODE_FLAG_PROC) ? STORAGE_PROCFS_DEVICE :
        node && (node->flags & (STORAGE_NODE_FLAG_DEV_NODE | STORAGE_NODE_FLAG_DEV_DIR |
                               STORAGE_NODE_FLAG_DEV_LINK)) ? STORAGE_DEVFS_DEVICE :
        node ? storage_volume_device(node->volume_id) : 1;
    out->st_ino = linux_stat_inode(path, st);
    out->st_nlink = 1;
    out->st_mode = mode;
    if (path) {
        struct reliefos_permissions value;
        int ret = fs_permissions_get(path, node, &value);
        if (ret < 0) return ret;
        out->st_uid = value.uid;
        out->st_gid = value.gid;
        out->st_mode = ((value.mode & LINUX_S_IFMT) ? value.mode & LINUX_S_IFMT : mode & LINUX_S_IFMT) |
            (value.mode & 07777u);
    }
    /* Fixed virtual consoles use Linux's tty major (4), which Xorg's
     * xf86HasTTYs() probes before entering VT_PROCESS.  Dynamic /dev/pts
     * endpoints retain the Unix98 PTY major (136); other synthetic devices
     * continue to use the per-kind minor identity used by this devfs. */
    uint32_t device_major = node && (node->flags & STORAGE_NODE_FLAG_PTY) &&
                            node->first_cluster >= 1u && node->first_cluster <= 6u
                                ? 4u : 136u;
    out->st_rdev = type == RELIEFOS_FS_TYPE_DEVICE
        ? (device_major << 8) | (node ? node->first_cluster : 0u) : 0;
    if (node && (node->flags & STORAGE_NODE_FLAG_DEV_BLOCK))
        out->st_rdev = storage_block_rdev(node->volume_id);
    if (node && (node->flags & STORAGE_NODE_FLAG_PTY)) {
        out->st_ino = (uint64_t)node->volume_id * 16 + node->first_cluster;
        out->st_rdev = (device_major << 8) | node->first_cluster;
    }
    if (node && (node->flags & STORAGE_NODE_FLAG_DEV_NODE) &&
        !(node->flags & STORAGE_NODE_FLAG_PTY)) {
        if (node->first_cluster == STORAGE_DEV_KIND_CONSOLE)
            out->st_rdev = (5u << 8) | 1u;
        else if (node->first_cluster == STORAGE_DEV_KIND_KMSG)
            out->st_rdev = (1u << 8) | 11u;
    }
    if ((node && (node->flags & (STORAGE_NODE_FLAG_AUDIO_PCM |
                                 STORAGE_NODE_FLAG_AUDIO_CONTROL |
                                 STORAGE_NODE_FLAG_AUDIO_MIXER |
                                 STORAGE_NODE_FLAG_AUDIO_TIMER))) ||
        (node && (node->flags & STORAGE_NODE_FLAG_DEV_NODE) &&
         node->first_cluster == STORAGE_DEV_KIND_AUDIO))
        out->st_rdev = linux_audio_rdev(node);
    if (node && node->first_cluster == STORAGE_DEV_KIND_DRM_CARD0)
        out->st_rdev = (226u << 8);
    if (node && node->first_cluster == STORAGE_DEV_KIND_DRM_RENDERD128)
        out->st_rdev = (226u << 8) | 128u;
    out->st_size = st ? (int64_t)st->size : 0;
    out->st_blksize = 4096;
    out->st_blocks = (out->st_size + 511) / 512;
    return 0;
}

void linux_stat_fd_type(struct linux_stat_abi *out, const struct task_file *file)
{
    if (file && file->drm_prime_file) {
        /* TTM's anonymous dma-buf inode deliberately has no S_IFMT type. */
        out->st_dev = 0;
        out->st_ino = file->drm_prime_ino;
        out->st_mode = 0600u;
        out->st_nlink = 1;
        out->st_uid = file->drm_prime_uid;
        out->st_gid = file->drm_prime_gid;
        out->st_rdev = 0;
        out->st_size = (int64_t)file->drm_prime_size;
        out->st_blksize = 4096;
        out->st_blocks = (out->st_size + 511) / 512;
        return;
    }
    /* The legacy SDK labels anonymous pipes as devices. Linux get_pipe_inode()
     * creates S_IFIFO | 0600; it must not look like a terminal to libc/sudo. */
    if (file && (file->flags & TASK_FILE_FLAG_PIPE) && !file->inode) {
        out->st_mode = LINUX_S_IFIFO | 0600u;
        out->st_rdev = 0;
    }
}

void linux_statx_from_legacy(struct linux_statx *out,
                                    const struct linux_stat_abi *st,
                                    uint32_t mask)
{
    const uint32_t available = LINUX_STATX_TYPE | LINUX_STATX_MODE |
        LINUX_STATX_NLINK | LINUX_STATX_UID | LINUX_STATX_GID |
        LINUX_STATX_INO | LINUX_STATX_SIZE | LINUX_STATX_BLOCKS |
        LINUX_STATX_ATIME | LINUX_STATX_MTIME | LINUX_STATX_CTIME;
    uint32_t returned = mask & available;
    *out = (struct linux_statx){0};
    out->stx_mask = returned;
    out->stx_dev_major = (st->st_dev >> 8) & 0xfff;
    out->stx_dev_minor = (st->st_dev & 0xff) | ((st->st_dev >> 12) & 0xffffff00);
    out->stx_rdev_major = (st->st_rdev >> 8) & 0xfff;
    out->stx_rdev_minor = (st->st_rdev & 0xff) | ((st->st_rdev >> 12) & 0xffffff00);
    out->stx_blksize = st->st_blksize > 0 ? (uint32_t)st->st_blksize : 4096;
    if (returned & (LINUX_STATX_TYPE | LINUX_STATX_MODE)) out->stx_mode = (uint16_t)st->st_mode;
    if (returned & LINUX_STATX_NLINK) out->stx_nlink = (uint32_t)st->st_nlink;
    if (returned & LINUX_STATX_UID) out->stx_uid = st->st_uid;
    if (returned & LINUX_STATX_GID) out->stx_gid = st->st_gid;
    if (returned & LINUX_STATX_INO) out->stx_ino = st->st_ino;
    if (returned & LINUX_STATX_SIZE) out->stx_size = st->st_size < 0 ? 0 : (uint64_t)st->st_size;
    if (returned & LINUX_STATX_BLOCKS) out->stx_blocks = st->st_blocks < 0 ? 0 : (uint64_t)st->st_blocks;
    if (returned & LINUX_STATX_ATIME) out->stx_atime=(struct linux_statx_timestamp){.tv_sec=st->atime_sec,.tv_nsec=st->atime_nsec};
    if (returned & LINUX_STATX_MTIME) out->stx_mtime=(struct linux_statx_timestamp){.tv_sec=st->mtime_sec,.tv_nsec=st->mtime_nsec};
    if (returned & LINUX_STATX_CTIME) out->stx_ctime=(struct linux_statx_timestamp){.tv_sec=st->ctime_sec,.tv_nsec=st->ctime_nsec};
}

/**
 * Stat for fd.
 * @param fd Value supplied by the caller.
 * @param task Value supplied by the caller.
 * @param st Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int stat_for_fd(int fd, struct task *task, struct reliefos_stat *st)
{
    struct task_file *file;
    if (!st) {
        return -RELIEFOS_EFAULT;
    }
    file = task_file_for_fd(task, fd);
    if (file) {
        st->type = ((file->flags & (TASK_FILE_FLAG_PIPE | TASK_FILE_FLAG_DEV_NULL)) != 0)
                       ? RELIEFOS_FS_TYPE_DEVICE
                                                         : file->node.type;
        st->reserved = 0;
        st->size = file->node.size;
        return 0;
    }
    if (fd >= 0 && fd < (int)SCHED_TASK_STDIO_MAX &&
        (sched_task_fds(task)->closed_stdio_mask & (1u << (uint32_t)fd)) != 0) {
        return -RELIEFOS_EBADF;
    }
    if ((fd >= 0 && fd < 3) || task_pty_fd_for_fd(task, fd)) {
        st->type = RELIEFOS_FS_TYPE_DEVICE;
        st->reserved = 0;
        st->size = 0;
        return 0;
    }
    return -RELIEFOS_EBADF;
}
