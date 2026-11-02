/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PROC_INTERNAL_H
#define RELIEFNT_PROC_INTERNAL_H
#include <reliefnt/procfs.h>
struct task_identity;
/** @brief Test whether an intrinsic proc key names a memory snapshot file.
 * @param name Borrowed key, nullable. @return True for swaps, meminfo or vmstat.
 */
bool proc_memory_is_file(const char *name);
/** @brief Capture one immutable memory snapshot for a proc open.
 * @param name Intrinsic memory key. @param out Required owned output.
 * @return Zero or negative errno. Execution writer pins snapshot inputs; no
 * path or area owner is retained after capture.
 */
int proc_memory_open(const char *name, void **out);
/** @brief Render a byte range from an immutable memory snapshot.
 * @param view Borrowed capture. @param offset Byte position. @param buffer
 * Writable destination, nullable for zero capacity. @param capacity Maximum
 * bytes. @param read Required actual byte count. @return Zero or -EINVAL.
 */
int proc_memory_read(void *view, uint64_t offset, void *buffer, uint32_t capacity, uint32_t *read);
/** @brief Release an owned memory snapshot; NULL is ignored. */
void proc_memory_close(void *view);
/** @brief Obtain immutable proc permissions, including per-task OOM ownership.
 * @param name Intrinsic key. @param node Retained node. @param value Metadata
 * output or proposal. @param write True for unsupported metadata mutation.
 * @return Zero or negative errno. Task identity remains execution-owned.
 */
int proc_control_permissions(const char *name, const struct storage_node *node,
                             struct reliefos_permissions *value, bool write);
/** @brief Commit a whole proc control write after normal VFS authorization.
 * @param name Intrinsic live key. @param offset File position. @param buffer
 * Validated bytes. @param length Byte count. @param written Completed bytes.
 * @return Zero or negative errno. OOM policy is checked under the sole writer.
 */
int proc_control_write(const char *name, uint64_t offset, const void *buffer,
                       uint32_t length, uint32_t *written);
/** @brief Admit zero truncation on writable proc controls for shell redirection.
 * @param name Intrinsic key. @param length Requested size. @return Zero or errno.
 */
int proc_control_truncate(const char *name, uint64_t length);
/** @brief Identify controls whose successful write needs no subsequent flush.
 * @param name Intrinsic key. @return True for a supported immediate control.
 */
bool proc_control_immediate(const char *name);
struct task;
struct text_stream;
/* Standalone legacy proc fixtures omit optional producers. Production Kbuild
 * always links inotify.o and the fs/notify core; its definitions are strong. */
#ifdef RELIEFNT_PROC_INOTIFY_IMPLEMENTATION
#define PROC_INOTIFY_LINKAGE
#else
#define PROC_INOTIFY_LINKAGE __attribute__((weak))
#endif
/** @brief Identify an inotify sysctl directory or value by intrinsic key.
 * @param name Nullable borrowed key. @return File=1, directory=2, absent=0.
 */
int proc_inotify_kind(const char *name) PROC_INOTIFY_LINKAGE;
/** @brief Borrow the static canonical key for a supported inotify value.
 * @param name Nullable intrinsic key. @return Static key or NULL, no ownership.
 */
const char *proc_inotify_key(const char *name) PROC_INOTIFY_LINKAGE;
/** @brief Stream the current inotify limit while execution ownership is held.
 * @param name Intrinsic value key. @param stream Required output stream.
 * @return Zero or -ENOENT; no references or state are changed.
 */
int proc_inotify_emit(const char *name, struct text_stream *stream) PROC_INOTIFY_LINKAGE;
/** @brief Enumerate inotify's three sysctl values with a rewindable cookie.
 * @param offset Required in/out cookie. @param entry Required output.
 * @return Entry=1 or EOF=0; no references escape.
 */
int proc_inotify_readdir(uint64_t *offset, struct reliefos_dir_entry *entry) PROC_INOTIFY_LINKAGE;
/** @brief Apply a numeric inotify sysctl under the caller's execution writer.
 * @param name Intrinsic key. @param offset Byte position; later writes are ignored.
 * @param buffer Validated input bytes. @param length Byte count.
 * @param written Required actual count, cleared on failure.
 * @return Zero or negative errno; VFS enforces root-owned mode and mount policy.
 */
int proc_inotify_write(const char *name, uint64_t offset, const void *buffer,
                       uint32_t length, uint32_t *written) PROC_INOTIFY_LINKAGE;
#undef PROC_INOTIFY_LINKAGE
/** @brief Resolve a validated proc process/thread path; no references escape.
 * @param path Intrinsic path. @param leaf Required borrowed remaining name.
 * @return Borrowed live task or NULL, under caller execution ownership. */
struct task *proc_view_task(const char *path, const char **leaf);
/** @brief Append the existing private task identity fields for ABI compatibility.
 * @param task Borrowed live task. @param stream Required output stream.
 * Caller holds execution ownership; no allocation or reference changes.
 */
void proc_task_identity_fields(const struct task *task, struct text_stream *stream);
/** @brief Check Linux-style cross-task read access using filesystem credentials.
 * @param caller Live caller. @param target Live target. @return Authorization. */
bool proc_stat_mm_visible(struct task *caller, struct task *target);
/** @brief Format a retained object for maps/fd diagnostics, never reopen by name.
 * @param object Borrowed path. @param target Output. @param capacity Bytes.
 * @return Zero or errno, execution-owned with balanced temporary root reference. */
int proc_object_target(const struct vfs_path *object, char *target, uint32_t capacity);
/** @brief Identify supported streaming proc leaves without generating content.
 * @param path Intrinsic path. @return File=1, directory=2, symlink=3, absent=0. */
int proc_extra_kind(const char *path);
/** @brief Render a supported proc slice from actual subsystem state.
 * @param path Intrinsic key. @param s Required validated stream.
 * @return Zero or native errno, ENOENT if not owned; execution ownership held. */
int proc_extra_emit(const char *path, struct text_stream *s);
/** @brief Enumerate extra directories with stable numeric cookies.
 * @param path Intrinsic path. @param offset In/out cookie. @param entry Output.
 * @return Entry=1, EOF=0, or errno; ENOTDIR means legacy directory handling. */
int proc_extra_readdir(const char *path, uint64_t *offset, struct reliefos_dir_entry *entry);

/** @brief Capture a mount view, generated leaf or diagnostic snapshot at open.
 * @param name Required borrowed NUL-terminated intrinsic inode key.
 * @param flags Sanitized open status flags; write-only controls need no view.
 * @param out Required writable output; owned state or NULL on stateless success.
 * @return Zero or negative errno with no partial ownership on failure.
 * Execution-owned task context; mount roots retain providers across target
 * exit, and generated byte snapshots remain fixed until close. Partition
 * metadata can perform I/O; no graph lock spans renderer callbacks. */
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
