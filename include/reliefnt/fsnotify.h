/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FSNOTIFY_H
#define RELIEFNT_FSNOTIFY_H

#include <reliefnt/storage.h>
#include <linux/inotify.h>

struct vfs_backend_ops;
struct fsnotify_instance;

#define FSNOTIFY_SUPPORTED_MASK (IN_ALL_EVENTS|IN_UNMOUNT|IN_Q_OVERFLOW|IN_IGNORED|\
    IN_ONLYDIR|IN_DONT_FOLLOW|IN_EXCL_UNLINK|IN_MASK_CREATE|IN_MASK_ADD|IN_ISDIR|IN_ONESHOT)

/** @brief Weak inode identity, copied without retaining provider or inode.
 * Every interface is serialized by the execution writer. The caller retires
 * watches before the filesystem instance, provider code, or inode expires.
 */
struct fsnotify_key {
    const struct vfs_backend_ops *ops; /**< Borrowed live identity callbacks. */
    void *context; /**< Borrowed live provider domain, not retained. */
    struct storage_node node; /**< Value copy of the inode identity. */
};

enum fsnotify_limit {
    FSNOTIFY_LIMIT_INSTANCES,
    FSNOTIFY_LIMIT_WATCHES,
    FSNOTIFY_LIMIT_QUEUED_EVENTS,
};

/** @brief Create one per-open notification instance charged to a real UID.
 * @param uid Real UID charged for this open and its watches.
 * @param wake Optional nonblocking notification callback; it must not mutate
 * the core or relinquish execution ownership.
 * @param wake_context Opaque caller-owned context valid until the final put.
 * @param out Required output set to NULL on failure.
 * @return Zero, -EMFILE for the UID quota, -ENOMEM or -EINVAL.
 * The queue limit is snapshotted here. Execution-writer ownership required.
 */
int fsnotify_create(uint32_t uid,void (*wake)(void *),void *wake_context,
                    struct fsnotify_instance **out);
/** @brief Add an ownership reference under execution-writer ownership.
 * @param instance Live instance; NULL is ignored.
 * @return None.
 */
void fsnotify_get(struct fsnotify_instance *instance);
/** @brief Release a reference and destroy watches, events and charges at zero.
 * @param instance Owned reference; NULL is ignored.
 * @return None. Execution-writer ownership required.
 */
void fsnotify_put(struct fsnotify_instance *instance);
/** @brief Add or replace a watch for one actual inode.
 * @param instance Live instance.
 * @param key Weak identity with a live ops->same_node callback.
 * @param mask Linux requested events and watch flags; lookup, ONLYDIR and
 * DONT_FOLLOW enforcement and permissions are the caller's responsibility.
 * @return Positive descriptor or negative errno. Execution writer required.
 */
int fsnotify_add_watch(struct fsnotify_instance *instance,const struct fsnotify_key *key,
                       uint32_t mask);
/** @brief Remove a watch and enqueue its IN_IGNORED event.
 * @param instance Borrowed live instance.
 * @param wd Watch descriptor to remove.
 * @return Zero or -EINVAL. Execution writer required; the watch holds no inode
 * or backend reference, and its UID charge is released.
 */
int fsnotify_remove_watch(struct fsnotify_instance *instance,int wd);
/** @brief Deliver an event to all matching inode watches.
 * @param key Borrowed weak identity whose backend and callback remain live.
 * @param mask Event bits and optional IN_ISDIR (for a directory child).
 * @param cookie Rename cookie or zero.
 * @param name Borrowed NUL-terminated basename up to 255 bytes, or NULL/empty
 * for a nameless event; delivery copies the name.
 * @param unlinked True for a post-unlink child operation subject to EXCL_UNLINK.
 * @return None. Memory pressure causes an overflow event, never operation error.
 * Execution-writer ownership required; callback must not reenter the core.
 */
void fsnotify_emit(const struct fsnotify_key *key,uint32_t mask,uint32_t cookie,
                   const char *name,bool unlinked);
/** @brief Deliver DELETE_SELF to matching watches, then retire them with IGNORED.
 * @param key Terminal inode identity while its backend callback is still live.
 * @return None. Execution-writer ownership required.
 */
void fsnotify_delete(const struct fsnotify_key *key);
/** @brief Deliver UNMOUNT and IGNORED and retire a backend's watches.
 * @param ops Borrowed backend operation table, or NULL.
 * @param context Borrowed backend domain identity.
 * @return None. Call before destroying the backend, under execution writer;
 * matching weak watches do not keep provider code or context alive.
 */
void fsnotify_unmount(const struct vfs_backend_ops *ops,void *context);
/** @brief Obtain a nonzero rename correlation cookie.
 * @return Next cookie, wrapping past zero. Execution writer required.
 */
uint32_t fsnotify_next_cookie(void);
/** @brief Check whether any live instance currently owns a watch.
 * @return True if at least one watch exists. Execution writer required.
 */
bool fsnotify_has_watches(void);
/** @brief Check whether a watched native cache generation needs preservation.
 * @param node Borrowed FAT/exFAT storage identity with nonzero generation.
 * @return True for matching filesystem, volume, mount and inode generation among watches.
 * Execution-writer ownership required; this takes no filesystem references.
 */
bool fsnotify_storage_node_watched(const struct storage_node *node);
/** @brief Borrow the first complete Linux event without consuming it.
 * @param instance Borrowed live instance, or NULL.
 * @param out Required borrowed record output, cleared when empty.
 * @return Record byte length, or zero and NULL when empty/invalid. Pointer is
 * valid until the next queue mutation or final put; copy before consuming.
 * Execution-writer ownership required.
 */
size_t fsnotify_peek(struct fsnotify_instance *instance,const struct inotify_event **out);
/** @brief Consume the previously peeked first record, if one exists.
 * @param instance Borrowed live instance, or NULL.
 * @return None. Execution writer required; the consumed record is released.
 */
void fsnotify_pop(struct fsnotify_instance *instance);
/** @brief Count bytes occupied by complete queued records for FIONREAD.
 * @param instance Borrowed live instance, or NULL.
 * @return Byte count. Execution writer required; ownership is unchanged.
 */
size_t fsnotify_pending_bytes(const struct fsnotify_instance *instance);
/** @brief Check whether a complete event can be read without blocking.
 * @param instance Borrowed live instance, or NULL.
 * @return True if nonempty. Execution writer required; no reference is acquired.
 */
bool fsnotify_ready(const struct fsnotify_instance *instance);
/** @brief Read a global per-UID/queue resource policy.
 * @param limit Policy selector.
 * @return Current limit, or zero for invalid selector. Execution-writer
 * ownership is required; the scalar result transfers no ownership.
 */
uint32_t fsnotify_limit_get(enum fsnotify_limit limit);
/** @brief Change a global resource policy for future admission/instances.
 * @param limit Policy selector.
 * @param value New limit; zero is supported.
 * @return Zero or -EINVAL. Existing resources are not discarded. Execution
 * writer required; queue capacity is snapshotted when an instance is created.
 */
int fsnotify_limit_set(enum fsnotify_limit limit,uint32_t value);

#endif
