#ifndef RELIEFNT_FS_VFS_VFS_C_INTERNAL_H
#define RELIEFNT_FS_VFS_VFS_C_INTERNAL_H

#include "../../block/core/storage_internal.h"
#include <reliefnt/drm.h>
#include "../devfs/internal.h"


/**
 * @brief Report whether the reserved runtime-root filesystem instance is ready.
 * @return True after successful native root discovery, false before or on failure.
 * Execution-owned observation only; no I/O, allocation or ownership transfer.
 */
bool storage_ready(void);

/**
 * @brief Format a bounded diagnostic prefix and unsigned disk/partition number.
 * @param out Writable output, ignored if NULL. @param cap Byte capacity including NUL.
 * @param prefix Borrowed optional literal prefix.
 * @param value Unsigned diagnostic number. @param partition Negative to omit the
 * partition suffix, otherwise zero-based index rendered as p(index+1).
 * @return None. Truncates with a terminator whenever out/cap permit; no allocation,
 * I/O or reference transfer. Caller owns its buffer; no shared mutable state.
 */
void storage_format_u32(char *out, uint32_t cap, const char *prefix,
                               uint32_t value, int partition);

/**
 * @brief Reports the filesystem implementation backing the runtime root at runtime.
 * @return A static lowercase name suitable for boot diagnostics.
 */
const char *storage_root_filesystem_name(void);

/**
 * @brief Report whether the current runtime root is the installer RAM image.
 * @return True for an active reserved installer root, false for disk/absent root.
 * Execution-owned observation only; retains no pointer or module reference.
 */
bool storage_installer_root_active(void);



int storage_read_node(const struct storage_node *node, uint64_t offset,
                      void *buf, uint32_t len, uint32_t *out_read);

int storage_read_node_cursor(const struct storage_node *node, uint64_t offset,
                             void *buf, uint32_t len, uint32_t *out_read,
                             struct storage_read_cursor *cursor);

int storage_readdir_node(const struct storage_node *node, uint64_t *cursor,
                         struct reliefos_dir_entry *entry);






int storage_write_boot_esp_file(const char *path, const void *buf, uint32_t len);

int storage_unlink_boot_esp_file(const char *path);








#endif
