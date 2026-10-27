/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_INITRAMFS_ARCHIVE_H
#define RELIEFNT_INITRAMFS_ARCHIVE_H
#include <reliefnt/types.h>

/* Borrowed immutable wire operands. The scanner does not resolve names or
 * create objects; the root constructor enforces VFS path/link/device policy. */
struct initramfs_entry {
    const char *name;
    const uint8_t *data;
    uint32_t name_length,size,inode,mode,uid,gid,links,mtime;
    uint32_t device_major,device_minor,rdevice_major,rdevice_minor;
};

/** @brief Decode uncompressed Linux newc/crc records without modifying the archive.
 * @param image Stable readable length-byte archive, NULL permitted at length0.
 * @param length Complete source extent; all names/data/alignment stay in bounds.
 * @param visitor Optional task-context callback, zero succeeds/negative errno
 * stops. An entry borrows archive bytes only for this call; NULL entry reports
 * an exact TRAILER!!! boundary so callers reset their hard-link tuple state.
 * @param context Borrowed callback state, never retained or interpreted here.
 * @return Zero, -EINVAL for invalid operands/callback status, -ENOEXEC for
 * malformed/truncated records, -EBADMSG for checksum mismatch, -EOPNOTSUPP for
 * compressed input, or unchanged callback errno. Supports concatenation/NUL
 * padding and optional trailer. No allocation, locks, I/O or ownership transfer;
 * caller pins source and visitor code and rolls back any failed construction.
 */
int initramfs_scan(const void *image,size_t length,
                      int (*visitor)(void *,const struct initramfs_entry *),void *context);
#endif
