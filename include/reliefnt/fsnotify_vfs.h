/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FSNOTIFY_VFS_H
#define RELIEFNT_FSNOTIFY_VFS_H

#include <reliefnt/types.h>

struct fsnotify_key;
struct storage_node;
struct vfs_path;

/* Independent host fixtures may omit the VFS notification implementation. */
#ifdef RELIEFNT_FSNOTIFY_VFS_IMPLEMENTATION
#define VFS_NOTIFY_OPTIONAL
#else
#define VFS_NOTIFY_OPTIONAL __attribute__((weak))
#endif

/** @brief Capture a weak inode identity from an execution-owned retained path.
 * @param path Borrowed, valid retained VFS path.
 * @param key Required weak-key output, cleared on invalid path.
 * @return Zero or negative errno; no path, backend or inode reference is taken.
 * Call only when the optional function is present; execution writer required.
 */
int vfs_notify_key(const struct vfs_path *path,struct fsnotify_key *key) VFS_NOTIFY_OPTIONAL;
/** @brief Report an operation on a retained inode and its named parent.
 * @param path Borrowed retained inode path, or NULL.
 * @param mask Linux event bit(s).
 * @return None. A disconnected child suppresses parent activity where the
 * watch uses IN_EXCL_UNLINK. Execution writer required, no ownership transfer;
 * call only after checking that the optional hook is present.
 */
void vfs_notify_event(const struct vfs_path *path,uint32_t mask) VFS_NOTIFY_OPTIONAL;
/** @brief Test whether a native inode cache entry is needed by a weak watch.
 * @param node Borrowed native node, or NULL.
 * @return True when its generation is watched. Execution writer is required;
 * the optional hook takes no reference and callers must check its presence.
 */
bool vfs_notify_storage_node_watched(const struct storage_node *node) VFS_NOTIFY_OPTIONAL;

#undef VFS_NOTIFY_OPTIONAL
#endif
