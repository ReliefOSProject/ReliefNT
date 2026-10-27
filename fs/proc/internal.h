/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PROC_INTERNAL_H
#define RELIEFNT_PROC_INTERNAL_H
#include <reliefnt/procfs.h>
struct task_identity;

/** @brief Capture an authorized proc mount view's namespace/root at open time.
 * @param name Borrowed intrinsic inode key; ordinary files require no state.
 * @param flags Sanitized open status flags, unchanged.
 * @param out Required owned output; NULL on failure or stateless success.
 * @return Zero or negative errno, no partial ownership on failure.
 * Nonblocking execution-owned capture; retained paths keep providers alive
 * through target exit. No graph/module lock spans this callback.
 */
int proc_file_open(const char *name,uint32_t flags,void **out);
/** @brief Read the live graph through the namespace/root captured at open.
 * @param value Borrowed successful capture, independent of later PID reuse.
 * @param offset Byte position in the rendered view.
 * @param buffer Kernel-accessible destination, nullable at zero capacity.
 * @param capacity Available byte count.
 * @param read Required actual output count, cleared on validation failure.
 * @return Renderer status or negative errno; ownership is unchanged.
 * Execution-owned context; no graph/module lock spans renderer callbacks.
 */
int proc_file_read(void *value,uint64_t offset,void *buffer,uint32_t capacity,uint32_t *read);
/** @brief Consume one captured root before its providers can retire.
 * @param value Owned optional capture; NULL is ignored.
 * @return None. Execution-owned final close, no graph/scheduler/module lock
 * across path release, which can perform provider I/O.
 */
void proc_file_close(void *value);

/* Internal proc keys always start at their own /proc root, independent of the
 * visible mountpoint. These helpers never resolve a global namespace prefix. */
int proc_mount_node_identity(const char *name,struct task_identity **out,bool *magic);
int proc_mount_magic_target(const char *name,struct vfs_path *out);
#endif
