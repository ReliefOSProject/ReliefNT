/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PROC_INTERNAL_H
#define RELIEFNT_PROC_INTERNAL_H
#include <reliefnt/procfs.h>
struct task_identity;

/** @brief Capture a mount view or diagnostic snapshot at open time.
 * @param name Required borrowed NUL-terminated intrinsic inode key.
 * @param flags Sanitized open status flags; write-only controls need no view.
 * @param out Required writable output; owned state or NULL on stateless success.
 * @return Zero or negative errno with no partial ownership on failure.
 * Execution-owned task context; mount roots retain providers across target
 * exit, and diagnostic read snapshots remain fixed until close. */
int proc_file_open(const char *name,uint32_t flags,void **out);
/** @brief Read a captured mount graph or fixed diagnostic snapshot.
 * @param value Required borrowed open capture, independent of later PID reuse.
 * @param offset Byte position in the rendered view.
 * @param buffer Writable validated destination, nullable at zero capacity.
 * @param capacity Available byte count.
 * @param read Required writable actual count, cleared on validation failure.
 * @return Renderer status or negative errno; ownership is unchanged.
 * Execution-owned context; no graph/module lock spans renderer callbacks. */
int proc_file_read(void *value,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
/** @brief Consume an open capture and release its root or diagnostic data.
 * @param value Owned optional capture; NULL is ignored.
 * @return None. Execution-owned final close, no graph/scheduler/module lock
 * across path release, which can perform provider I/O. */
void proc_file_close(void *value);

/* Internal proc keys always start at their own /proc root, independent of the
 * visible mountpoint. These helpers never resolve a global namespace prefix. */
#ifdef CONFIG_LOCK_STAT
/** @brief Apply one privileged control command to the intrinsic proc key.
 * @param name Required borrowed NUL-terminated intrinsic key.
 * @param offset Required zero byte position for this whole-value command.
 * @param buffer Required validated readable command bytes.
 * @param length Exact byte count, at most six including an optional newline.
 * @param written Required writable count, cleared on failure.
 * @return Zero or negative errno. Execution-owned task context; CAP_SYS_ADMIN
 * is checked at write time and VFS checks the retained mount's read-only state. */
int proc_lock_stats_write(const char *name, uint64_t offset, const void *buffer,
                          uint32_t length, uint32_t *written);
/** @brief Permit no-op zero truncation on the control node for redirection.
 * @param name Required borrowed NUL-terminated intrinsic key.
 * @param length Requested size; only zero is accepted on the control node.
 * @return Zero or negative errno. Execution-owned task context; the caller
 * retains the provider and VFS checks the live read-only mount policy. */
int proc_lock_stats_truncate(const char *name, uint64_t length);
/** @brief Expose writable mode for the control node and preserve other modes.
 * @param name Required borrowed NUL-terminated intrinsic key.
 * @param node Required borrowed retained generated inode metadata.
 * @param value Required writable permission output or proposed metadata.
 * @param write True for an unsupported metadata mutation.
 * @return Zero or negative errno. Execution-owned, no allocation or transfer. */
int proc_lock_stats_permissions(const char *name, const struct storage_node *node,
                                struct reliefos_permissions *value, bool write);
#endif
int proc_mount_node_identity(const char *name,struct task_identity **out,bool *magic);
int proc_mount_magic_target(const char *name,struct vfs_path *out);
#endif
