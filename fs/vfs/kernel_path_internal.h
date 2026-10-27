/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_KERNEL_PATH_INTERNAL_H
#define RELIEFNT_KERNEL_PATH_INTERNAL_H
#include "mount_internal.h"
#include "../../block/core/storage_internal.h"
#include <reliefnt/kernel_file.h>
struct vfs_kernel_io_scope {bool async,mutation;uint64_t generation;};
/** @brief Suspend storage yielding for one execution-owned kernel operation.
 * @return Saved async/mutation state, consumed by vfs_kernel_io_end. Nested
 * scopes are allowed. Caller holds execution ownership; this does not drain
 * or cancel an already admitted task request or acquire a graph/module lock.
 */
static inline struct vfs_kernel_io_scope vfs_kernel_io_begin(void)
{
    struct vfs_kernel_io_scope saved={storage_io_async_context,storage_io_write_started,storage_metadata_generation()};
    storage_io_async_context=false;return saved;
}
/** @brief Restore the exact outer storage context after synchronous cleanup.
 * @param saved Scope returned by vfs_kernel_io_begin, under execution ownership.
 * @return None. All operation-owned callbacks/paths have finished before entry;
 * never resets an outer transaction's mutation marker or pending I/O state.
 * A metadata epoch change keeps an outer asynchronous transaction beyond its
 * replayable-read phase, even when this operation later returned an error.
 */
static inline void vfs_kernel_io_end(struct vfs_kernel_io_scope saved)
{
    storage_io_async_context=saved.async;
    storage_io_write_started=saved.mutation || (saved.async && storage_metadata_generation()!=saved.generation);
}
#endif
