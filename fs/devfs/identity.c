#include <reliefnt/sound_service.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/devfs.h>

/** @brief Encode a standard audio node as Linux ALSA major/minor device ID.
 * @param node Storage node carrying an audio control or PCM identity.
 * @return Linux old-style dev_t value, or zero for a stale identity.
 */
static uint64_t devfs_audio_device_number(const struct storage_node *node)
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
    index = sound_service_card_index(card_token);
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

/** @brief Encode standard devfs identities after inode metadata is obtained.
 * @param out Actual stat result with type already set; updated device fields.
 * @param node Borrowed held node, nullable for anonymous non-device objects.
 * No pathname lookup or ownership change. Audio identities use the currently
 * admitted card registry under execution ownership; no spinlock spans I/O.
 */
void devfs_stat_identity(struct linux_stat_abi *out,const struct storage_node *node)
{
    /* A real special inode already supplied st_rdev through getattr. Its
     * inode number must never be interpreted as a generated devfs kind. */
    if(node && (node->flags&STORAGE_NODE_FLAG_RDEV))return;
    /* Fixed virtual consoles use Linux's tty major (4), which Xorg's
     * xf86HasTTYs() probes before entering VT_PROCESS.  Dynamic /dev/pts
     * endpoints retain the Unix98 PTY major (136); other synthetic devices
     * continue to use the per-kind minor identity used by this devfs. */
    uint32_t device_major = node && (node->flags & STORAGE_NODE_FLAG_PTY) &&
                            node->first_cluster >= 1u && node->first_cluster <= 6u
                                ? 4u : 136u;
    out->st_rdev = (out->st_mode & LINUX_S_IFMT) == LINUX_S_IFCHR
        ? (device_major << 8) | (node ? node->first_cluster : 0u) : 0;
    if (node && (node->flags & STORAGE_NODE_FLAG_DEV_BLOCK))
        out->st_rdev = storage_block_rdev(node->volume_id);
    if (node && (node->flags & STORAGE_NODE_FLAG_PTY)) {
        out->st_ino = (uint64_t)node->volume_id * 16 + node->first_cluster;
        out->st_rdev = (device_major << 8) | node->first_cluster;
    }
    if (node && (node->flags & STORAGE_NODE_FLAG_DEV_NODE) &&
        !(node->flags & (STORAGE_NODE_FLAG_PTY | STORAGE_NODE_FLAG_DEV_BLOCK))) {
        uint32_t major=0,minor=0;
        switch(node->first_cluster) {
        case STORAGE_DEV_KIND_NULL:major=1;minor=3;break;
        case STORAGE_DEV_KIND_ZERO:major=1;minor=5;break;
        case STORAGE_DEV_KIND_FULL:major=1;minor=7;break;
        case STORAGE_DEV_KIND_RANDOM:major=1;minor=8;break;
        case STORAGE_DEV_KIND_URANDOM:major=1;minor=9;break;
        case STORAGE_DEV_KIND_TTY:major=5;break;
        case STORAGE_DEV_KIND_PTMX:major=5;minor=2;break;
        case STORAGE_DEV_KIND_SERIAL:major=4;minor=64;break;
        case STORAGE_DEV_KIND_FB0:major=29;break;
        default:break;
        }
        if(major)out->st_rdev=(uint64_t)major<<8|minor;
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
        out->st_rdev = devfs_audio_device_number(node);
    if (node && node->first_cluster == STORAGE_DEV_KIND_DRM_CARD0)
        out->st_rdev = (226u << 8);
    if (node && node->first_cluster == STORAGE_DEV_KIND_DRM_RENDERD128)
        out->st_rdev = (226u << 8) | 128u;
}
