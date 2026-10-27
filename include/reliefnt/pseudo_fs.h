/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PSEUDO_FS_H
#define RELIEFNT_PSEUDO_FS_H
#include <reliefnt/mount.h>

/* Immutable generated-file adapters use intrinsic backend keys, independently
 * of their visible namespace attachment. They never route global pathnames.
 * The descriptor and callbacks remain valid through the provider module pin. */
struct vfs_pseudo_provider {
    const char *root;
    const char *filesystem; /* Optional immutable proc mount type/source label. */
    struct module *owner;
    uint64_t mount_flags;
    int (*lookup)(const char *name,struct storage_node *node);
    int (*read)(const char *name,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
    /* Optional whole-value controls; default generated files remain readonly.
     * The adapter validates the retained instance before each operation. */
    int (*write)(const char *name,uint64_t offset,const void *buffer,uint32_t length,uint32_t *written);
    int (*truncate)(const char *name,uint64_t length);
    int (*readdir)(const char *name,uint64_t *cursor,struct reliefos_dir_entry *entry);
    int (*readlink)(const char *name,char *buffer,uint32_t capacity);
    /* Optional identity trio: get transfers an owned pin on success, NULL for
     * an identity-free inode. Equal pins identify the same object; alive must
     * detect retirement without retargeting a reused identifier. */
    int (*get_identity)(const char *name,void **pin,bool *magic);
    bool (*identity_alive)(void *pin);
    void (*put_identity)(void *pin);
    /* Authorize and transfer an exact target for a marked magic link. */
    int (*follow)(const char *name,struct vfs_path *out);
    int (*permissions)(const char *intrinsic,const struct storage_node *node,
                        struct reliefos_permissions *value,bool write);
    /* Optional nonblocking per-open capture. An owned state is consumed by
     * close even on failed open; NULL means ordinary stateless reads. Reads
     * through captured state remain valid after the inode identity retires. */
    int (*open)(const char *intrinsic,uint32_t flags,void **state);
    int (*read_open)(void *state,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
    void (*close)(void *state);
};

/** @brief Create a referenced generated-file filesystem with owned inode keys.
 * @param provider Persistent builtin/module descriptor; root and lookup required.
 * @param out Empty output receiving a detached mount, empty on failure.
 * @return Zero or negative errno with balanced context/identity/provider pins.
 * Task context under execution ownership. Pins the provider before its first
 * callback. Live keys have no fixed capacity; one unretained lookup result is
 * cached until retain, replacement or final cleanup. Attachment names and
 * MS_MOVE do not alter intrinsic keys. No graph lock spans callbacks.
 */
int vfs_pseudo_mount_new(const struct vfs_pseudo_provider *provider,struct vfs_mount **out);
#endif
