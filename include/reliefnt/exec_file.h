/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_EXEC_FILE_H
#define RELIEFNT_EXEC_FILE_H
#include <reliefnt/mount.h>

/* A kernel-only immutable ELF backing source, shared by lazy image VMAs.
 * Ordinary mmap keeps its original task_file/OFD; it does not use this type. */
struct vfs_exec_source;
/** @brief Open a held executable's backing without allocating a user descriptor.
 * @param path Borrowed actual regular inode; caller already authorized exec.
 * @param out Receives one owned source or NULL on failure.
 * @return Zero or type/allocation/ETXTBSY/metadata errno. Pins the actual
 * filesystem path, denies writers, then captures current size. Failure rolls
 * back every resource. Execution ownership required; no graph/module lock
 * spans allocation or provider callbacks. No diagnostic name is resolved.
 */
int vfs_exec_source_open(const struct vfs_path *path,struct vfs_exec_source **out);
/** @brief Retain an ELF source for another VMA, clone or temporary owner.
 * @param source Borrowed live source or NULL, under execution ownership.
 * @return The same source with one additional reference, or NULL. No
 * allocation, metadata I/O or extra inode-denial acquisition occurs. Counter
 * saturation retains the source permanently rather than permitting wrap/UAF.
 */
struct vfs_exec_source *vfs_exec_source_get(struct vfs_exec_source *source);
/** @brief Retire one ELF source reference after its mapping stops using it.
 * @param source Owned source or NULL; consumed once.
 * Execution ownership required. Final release allows writers, puts the inode
 * before its provider path, then frees metadata. Final puts may perform I/O;
 * no scheduler/graph/module spinlock may span this operation.
 */
void vfs_exec_source_put(struct vfs_exec_source *source);
/** @brief Borrow an immutable inode/size snapshot while its source is retained.
 * @param source Borrowed live source or NULL, under execution ownership.
 * @return Borrowed node or NULL; valid until the caller drops its source.
 * No I/O or references are acquired. Its identity is never a displayed name.
 */
const struct storage_node *vfs_exec_source_node(const struct vfs_exec_source *source);
/** @brief Read immutable ELF bytes through the actual retained provider.
 * @param source Borrowed live source retaining inode and callback code.
 * @param offset Nonnegative byte offset.
 * @param buffer Writable kernel bytes, nullable only for zero capacity.
 * @param capacity Requested output bytes.
 * @param read Required completed-byte output, zero on pre-I/O failure.
 * @return Zero or provider errno; EOF and short reads are preserved, invalid
 * provider counts fail EIO. Caller owns execution without graph/module locks
 * across I/O. The captured size and node prevent stale OFD-cache EOF limits.
 */
int vfs_exec_source_read(const struct vfs_exec_source *source,uint64_t offset,
                            void *buffer,uint32_t capacity,uint32_t *read);
#endif
