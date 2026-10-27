/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_INITRAMFS_UNPACK_H
#define RELIEFNT_INITRAMFS_UNPACK_H
#include <reliefnt/types.h>
struct tmpfs_super;
/** @brief Construct a private memory root from an uncompressed newc stream.
 * @param image Stable readable length-byte archive, NULL permitted at length0.
 * @param length Complete archive extent; no borrowed archive bytes escape.
 * @param volume Reserved native identity for the resulting filesystem.
 * @param out Required output receiving the owned tmpfs only after full success.
 * @return Zero or negative archive/path/type/device/quota/allocation errno;
 * failure leaves out NULL and retires every path/inode/page/map allocation.
 * Uses the ordinary VFS name walker in a private unpublished namespace;
 * absolute symlinks and dot-dot remain confined to the initial memory root.
 * Existing nonempty directories cannot be replaced by non-directories.
 * Caller owns exclusive boot execution, source lifetime and volume identity;
 * no global namespace is changed and no driver or disk I/O is performed.
 */
int initramfs_unpack(const void *image,size_t length,uint32_t volume,struct tmpfs_super **out);
#endif
