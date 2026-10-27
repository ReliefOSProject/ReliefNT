/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_INITRAMFS_INTERNAL_H
#define RELIEFNT_INITRAMFS_INTERNAL_H
#include "archive.h"
#include "unpack.h"
#include <reliefnt/mount.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/heap.h>
#include <linux/errno.h>

struct unpack_link {
    struct unpack_link *next;
    uint32_t major,minor,inode,type,node;
};
struct unpack_time {
    struct unpack_time *next;
    uint32_t node,mtime;
};
struct unpack_context {
    struct tmpfs_super *fs;
    struct vfs_namespace ns;
    struct vfs_path root;
    struct unpack_link *links;
    struct unpack_time *times;
};
/** @brief Retain a private namei-only mount for a caller-owned fresh tmpfs.
 * @param context Exclusive zero-initialized state with a live fs pointer.
 * @return Zero or native allocation/lookup errno, fully rolled back on error.
 * Owns root/namespace pins until view_stop; never publishes globally or takes
 * ownership of fs itself. Boot execution required, no I/O/provider locks.
 */
int initramfs_view_start(struct unpack_context *context);
/** @brief Retire all namei paths before the constructor destroys/transfers fs.
 * @param context Exclusive state with no in-flight lookup callbacks.
 * @return Zero or EBUSY if a path was incorrectly retained by construction;
 * no fs ownership transfer or I/O. Successful stop leaves namespace empty.
 */
int initramfs_view_stop(struct unpack_context *context);
/** @brief Find the current archive's nondirectory hardlink tuple.
 * @param context Exclusive construction state. @param entry Borrowed record.
 * @return Borrowed matching ledger entry, or NULL. No allocation/I/O/locks.
 */
struct unpack_link *initramfs_link_find(struct unpack_context *context,const struct initramfs_entry *entry);
/** @brief Retain the actual inode for subsequent hardlink records.
 * @param context Exclusive construction state. @param entry Borrowed tuple.
 * @param node Borrowed created inode metadata, never a pathname identity.
 * @return Zero or allocation/inode errno; failure retains no ledger or hold.
 * Caller owns private fs; no VFS/module lock or I/O occurs.
 */
int initramfs_link_add(struct unpack_context *context,const struct initramfs_entry *entry,
                          const struct storage_node *node);
/** @brief Drop tuple inode holds at a trailer or failed/finished unpack.
 * @param context Exclusive construction state; empty ledgers are permitted.
 * @return None. Every ledger/hold retires, possibly freeing unlinked inodes;
 * caller keeps fs alive throughout, no VFS/module lock or I/O occurs.
 */
void initramfs_links_clear(struct unpack_context *context);
/** @brief Defer an explicit directory's mtime until all children are inserted.
 * @param context Exclusive state. @param node Created directory inode.
 * @param mtime Archive modification seconds.
 * @return Zero or ENOMEM, with no extra hold on failure. One hold per inode,
 * repeated records update the time; no pathname replay or disk I/O occurs.
 */
int initramfs_directory_time(struct unpack_context *context,uint32_t node,uint32_t mtime);
/** @brief Apply or discard deferred times, then retire every owned inode hold.
 * @param context Exclusive construction state. @param apply Store if true.
 * @return First native time error, otherwise zero. Always drains the ledger;
 * fs remains caller-owned, no VFS routing/module lock or disk I/O occurs.
 */
int initramfs_times_finish(struct unpack_context *context,bool apply);
#endif
