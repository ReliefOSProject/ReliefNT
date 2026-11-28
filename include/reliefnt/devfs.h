/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEVFS_H
#define RELIEFNT_DEVFS_H
#include <reliefnt/mount.h>
/** @brief Create a referenced device filesystem independently of its attachment.
 * @param out Empty output receiving a detached mount, empty on failure.
 * @return Zero or negative errno with balanced key/context rollback.
 * Requires execution ownership. Device operations keep their existing native
 * ABI; this mount supplies directory/node identities. Its shm directory is a
 * mountpoint for a separately owned writable memory filesystem.
 */
int devfs_create_vfs_mount(struct vfs_mount **out);
/** @brief Apply Linux device identities to generated nodes after inode getattr.
 * @param out Required stat with actual type set, updated device/PTY fields.
 * @param node Borrowed held node, nullable for anonymous objects. Native RDEV
 * inodes retain their own st_rdev and are never interpreted as generated kinds.
 * No lookup/allocation/I/O/ownership transfer. Execution ownership stabilizes
 * admitted audio/card identities; no registry lock spans this query.
 */
void devfs_stat_identity(struct linux_stat_abi *out,const struct storage_node *node);
/** @brief Find an admitted native device by Linux type and st_rdev.
 * @param type S_IFCHR or S_IFBLK, required.
 * @param device Linux stat-encoded device number, not an inode or pathname.
 * @param out Required canonical device metadata output, valid only on success.
 * @return Zero, -EINVAL, -ENXIO if no admitted matching device, or provider
 * enumeration errno. Uses only the device provider's intrinsic hierarchy,
 * independent of its mount location or the special inode's name. Caller owns
 * execution and obtains actual device/code ownership during open/mount; no
 * references transfer and no graph/module lock spans provider metadata I/O.
 */
int devfs_lookup_rdev(uint32_t type,uint64_t device,struct storage_node *out);
/** @brief Create a shared writable device filesystem mount.
 * @param source Diagnostic mount source. @param flags Linux mount flags.
 * @param options Optional tmpfs mount data. @param out Owned detached mount.
 * @return Zero or negative errno. Requires execution ownership.
 */
int devtmpfs_create_vfs_mount(const char *source,uint64_t flags,const char *options,
                              struct vfs_mount **out);
/** @brief Mark the device inventory dirty without allocation or hardware I/O.
 * IRQ/task safe. Standalone device fixtures may omit devtmpfs entirely.
 */
void devtmpfs_changed(void) __attribute__((weak));
/** @brief Notify mounted devtmpfs instances after releasing device registry locks. */
static inline void devtmpfs_notify(void)
{
    if(devtmpfs_changed)devtmpfs_changed();
}
/** @brief Query deferred inventory work. Execution ownership required. */
bool devtmpfs_pending(void);
/** @brief Reconcile coalesced device changes. Execution ownership required.
 * @return Zero or negative errno, retaining pending work on failure.
 */
int devtmpfs_refresh(void);
#endif
