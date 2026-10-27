/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_KERNEL_FILE_H
#define RELIEFNT_KERNEL_FILE_H
#include <reliefnt/mount.h>

/** @brief Read trusted kernel bytes from one actual namespace/open instance.
 * @param ns Borrowed initialized namespace; never inferred from a task cwd.
 * @param name Required absolute kernel pathname; symlinks follow Linux rules.
 * @param offset Initial byte position, no shared descriptor offset is changed.
 * @param buffer Kernel-accessible destination, NULL only at zero capacity.
 * @param capacity Destination size in bytes.
 * @param read Required actual count, cleared on any failure.
 * @return Zero on a full or EOF-short read, otherwise negative lookup/type/
 * capture/allocation/provider errno. Provider paths and captured state survive
 * every short read. Execution ownership required; storage is synchronous for
 * this call; restores prior async state without clearing mutation markers.
 * No graph/module lock spans callbacks; all temporary ownership is retired.
 */
int vfs_kernel_read(struct vfs_namespace *ns,const char *name,uint64_t offset,
                    void *buffer,uint32_t capacity,uint32_t *read);

/** @brief Read the observed size of a trusted regular file into owned pages.
 * @param ns Borrowed initialized namespace, required.
 * @param name Required absolute kernel pathname; final symlinks are followed.
 * @param data Required owned output, NULL on failure; even an empty file owns
 * one page. Release only with vfs_kernel_file_free and the returned length.
 * @param length Required observed byte size, zero on failure.
 * @return Zero or negative lookup/type/stat/allocation/provider errno; premature
 * EOF returns EIO, unrepresentable page counts EFBIG. No artificial 4GiB limit.
 * This is not an atomic content snapshot against concurrent file writers.
 * Execution ownership required, storage temporarily synchronous; restores
 * async state without clearing an existing/newly admitted mutation marker;
 * path/open/code pins retire after I/O. No graph/module lock spans callbacks.
 */
int vfs_kernel_read_file(struct vfs_namespace *ns,const char *name,const void **data,size_t *length);

/** @brief Consume a page buffer returned by vfs_kernel_read_file.
 * @param data Owned optional buffer; NULL does nothing.
 * @param length Exact byte length returned with this buffer, including zero.
 * @return None. Frees at least one page, without any filesystem callback.
 */
void vfs_kernel_file_free(const void *data,size_t length);

/** @brief Resolve trusted absolute kernel input in one explicit namespace.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @param flags VFS_LOOKUP_FOLLOW/PARENT/MISSING, with the common walker semantics.
 * @param out Required empty result, owning its actual path only on success.
 * @return Zero or negative errno; no lexical normalization or task cwd lookup.
 * Caller authorizes any later use, then releases out.path. Execution ownership
 * required; temporarily synchronous storage is restored on all exits, keeping
 * any existing/new mutation latch to prevent caller replay. No
 * graph/module lock spans callbacks and every intermediate path is retired.
 */
int vfs_kernel_lookup(struct vfs_namespace *ns,const char *name,unsigned flags,struct vfs_lookup_result *out);

/** @brief Create a trusted root-owned directory in the visible namespace.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @param mode Explicit permission bits, without a file type or task umask.
 * @return Zero or negative errno including EEXIST, preserving existing metadata.
 * Final symlinks are not followed. Caller authorizes this kernel operation;
 * execution ownership required, synchronous storage scope retired without
 * clearing the outer mutation latch, paths/providers
 * retained across callbacks without any graph/module lock.
 */
int vfs_kernel_mkdir(struct vfs_namespace *ns,const char *name,uint32_t mode);

/** @brief Replace a trusted regular file through one retained inode/writer lease.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @param buffer Kernel bytes, NULL only for an empty replacement.
 * @param length Byte length to write completely after truncation.
 * @return Zero or negative errno. New files are root-owned0644; existing files
 * retain metadata. Follows final symlinks, rejects readonly backing/attachment
 * and executing inodes. Failure after truncation may leave a partial file,
 * matching O_CREAT|O_WRONLY|O_TRUNC; this is not an atomic rename replacement.
 * Caller authorizes the operation. Execution ownership required; storage
 * async context restored without clearing the mutation latch; every
 * writer/path/provider pin retires on all exits,
 * with no graph/module lock spanning callbacks.
 */
int vfs_kernel_write_file(struct vfs_namespace *ns,const char *name,const void *buffer,uint32_t length);

/** @brief Unlink the actual trusted directory member, leaving its target alone.
 * @param ns Borrowed initialized namespace. @param name Absolute kernel path.
 * @return Zero or negative errno. Never follows the final symlink or removes
 * directories/mountpoints. Caller authorizes parent/sticky policy; execution
 * ownership required. Storage context is restored and all paths/providers are
 * retained through callbacks then retired, without graph/module locks.
 */
int vfs_kernel_unlink(struct vfs_namespace *ns,const char *name);
#endif
