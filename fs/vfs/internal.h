#ifndef RELIEFNT_FS_VFS_VFS_C_INTERNAL_H
#define RELIEFNT_FS_VFS_VFS_C_INTERNAL_H

#include "../../block/core/storage_internal.h"
#include <reliefnt/drm.h>


struct storage_dev_entry {
    const char *name;
    uint32_t kind;
    uint32_t type;
    uint8_t directory;
};

/* The devfs namespace is intentionally small and stable.  Drivers expose
 * their portable userspace ABI through these names; the legacy ioctl entry
 * points remain available for applications that have not migrated yet. */


/** @brief Stable entries exposed through the private devfs root directory. */
extern const struct storage_dev_entry storage_dev_entries[37];
/** @brief Stable entries exposed through the private devfs input directory. */
extern const struct storage_dev_entry storage_dev_input_entries[5];

bool storage_ready(void);

const struct storage_dev_entry *storage_dev_find(const char *name,
                                                         const struct storage_dev_entry *entries,
                                                         uint32_t count);

void storage_format_u32(char *out, uint32_t cap, const char *prefix,
                               uint32_t value, int partition);

void storage_dev_node(const struct storage_dev_entry *entry,
                             struct storage_node *out);

/**
 * @brief Reports the filesystem implementation backing the runtime root at runtime.
 * @return A static lowercase name suitable for boot diagnostics.
 */
const char *storage_root_filesystem_name(void);

bool storage_installer_root_active(void);

int storage_resolve_path(const char *cwd, const char *input, char *out, uint32_t cap);

int storage_lookup_path_unlocked(const char *path, struct storage_node *out);

int storage_lookup_path(const char *path, struct storage_node *out);

int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buf, uint32_t len, uint32_t *out_read);

int storage_read_node_cursor(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor);

int storage_readdir_node(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry);

int storage_read_file(const char *path, const void **out_data, size_t *out_len);

int storage_write_node(const char *path, uint64_t offset,
                       const void *buf, uint32_t len, uint32_t *out_written);

int storage_write_file(const char *path, const void *buf, uint32_t len);

int storage_truncate_file(const char *path, uint64_t length);

int storage_mkdir(const char *path);

int storage_unlink(const char *path);

int storage_write_boot_esp_file(const char *path, const void *buf, uint32_t len);

int storage_unlink_boot_esp_file(const char *path);

int storage_rmdir(const char *path);

int storage_rename(const char *old_path, const char *new_path);

int storage_link(const char *old_path, const char *new_path);

int storage_symlink(const char *target, const char *path);

int storage_readlink(const char *path, char *buffer, uint32_t capacity, uint32_t *out_len);

int storage_list_dir(const char *path, struct reliefos_dir_entry *entries,
                     uint32_t capacity, uint32_t *out_count);

int storage_stat_path(const char *path, struct reliefos_stat *st);

/**
 * @brief Create a socket or FIFO inode on a supporting filesystem.
 * @param path Resolved, parent-authorized absolute path.
 * @param mode S_IFSOCK or S_IFIFO, without permission bits.
 * @param out Receives the created inode identity.
 * @return Zero or negative errno; unsupported FIFO backends are unchanged.
 */
int storage_create_special(const char *path, uint32_t mode, struct storage_node *out);

/**
 * @brief Create a filesystem socket inode.
 * @param path Resolved, authorized absolute path.
 * @param out Receives the created inode.
 * @return Zero or negative errno.
 */
int storage_create_socket(const char *path, struct storage_node *out);

#endif
