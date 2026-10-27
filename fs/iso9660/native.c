/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/module.h>
#include <reliefnt/fs_type.h>

/** @brief Describe the validated immutable ISO root without reading an inode.
 * @param volume Borrowed parsed instance. @param out Required metadata output.
 * @return Zero. Execution ownership and provider pin required; no I/O or path.
 */
static int iso_native_root(struct storage_volume *volume,struct storage_node *out)
{
    *out=(struct storage_node){.type=RELIEFOS_FS_TYPE_DIR,.flags=STORAGE_NODE_FLAG_ROOT,
        .volume_id=volume->volume_id,.first_cluster=volume->iso_root_extent,
        .size=volume->iso_root_size};return 0;
}
/** @brief Decode one literal ISO child in an already selected directory extent.
 * @param volume Borrowed selected/pinned instance. @param parent Actual directory.
 * @param name Literal component. @param out Required metadata output.
 * @return Zero or native errno. Execution ownership required across optical I/O.
 */
static int iso_native_lookup(struct storage_volume *volume,const struct storage_node *parent,
                               const char *name,struct storage_node *out)
{ (void)volume;return iso9660_find_in_dir(parent->first_cluster,(uint32_t)parent->size,name,out); }
/** @brief Parse one exclusive selected iso9660 instance under its constructor pin.
 * @param volume Borrowed unpublished instance with transport geometry.
 * @return Zero or native parser errno. Execution ownership required across I/O;
 * no registry/module lock, no publication or independent ownership transfer.
 */
static int iso_native_mount(struct storage_volume *volume)
{ (void)volume;return iso9660_mount(); }
/** @brief Read the selected provider's actual retained file extent/stream.
 * @param volume Borrowed selected/pinned instance. @param node Retained file.
 * @param offset Byte position. @param bytes Validated writable destination.
 * @param capacity Available bytes, bounded by refreshed inode size.
 * @param length Optional actual-count output, initially zero.
 * @param cursor Optional caller-owned hint. @param mount_flags Access-local flags.
 * @return Zero or negative provider errno. Execution ownership required across
 * native I/O; no graph/module lock, code/instance lifetime held by caller.
 */
static int iso_native_read(struct storage_volume *volume,const struct storage_node *node,uint64_t offset,
                                  void *bytes,uint32_t capacity,uint32_t *length,
                                  struct storage_read_cursor *cursor,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;if(cursor)cursor->valid=0;
    uint8_t *dst=bytes;int ret;
    uint64_t absolute = (uint64_t)node->first_cluster * ISO9660_BLOCK_SIZE + offset;
    uint32_t done = 0;
    while (done < capacity) {
        uint64_t block = absolute / ISO9660_BLOCK_SIZE;
        uint32_t block_offset = (uint32_t)(absolute % ISO9660_BLOCK_SIZE);
        uint32_t take = min_u32(ISO9660_BLOCK_SIZE - block_offset, capacity - done);
        ret = storage_read_iso_blocks(block, 1, storage_scratch);
        if (ret < 0) {

            return storage_read_failure(ret);
        }
        storage_memcpy(dst + done, storage_scratch + block_offset, take);
        absolute += take;
        done += take;
    }
    if (length) {
        *length = done;
    }

    return 0;

}
/** @brief Enumerate the selected provider's actual retained directory.
 * @param volume Borrowed selected/pinned instance. @param node Actual directory.
 * @param cursor Required opaque position. @param entry Required metadata output.
 * @param mount_flags Current attachment/shared flags for this access only.
 * @return One entry, zero EOF or negative native errno. Execution ownership
 * required; no graph/module lock spans native I/O, no retained output pointer.
 */
static int iso_native_readdir(struct storage_volume *volume,const struct storage_node *node,uint64_t *cursor,
                                     struct reliefos_dir_entry *entry,uint64_t mount_flags)
{
    (void)volume;(void)mount_flags;
    int rc=iso9660_iter_dir_entry(node->first_cluster,(uint32_t)node->size,*cursor,entry);
    if(!rc){++*cursor;return 1;}return rc==-ENOENT ? 0 : rc;
}
/** @brief Report capacity/identity from the selected native filesystem.
 * @param volume Borrowed selected/pinned instance. @param value Required output
 * initialized with generic flags/name limit/device identity by the caller.
 * @return Zero or native metadata/I/O errno. Execution ownership required;
 * no graph/module lock spans bitmap/group reads, output unspecified on error.
 */
static int iso_native_statfs(struct storage_volume *volume,struct linux_statfs_abi *value)
{
    value->f_type = 0x9660;
    value->f_bsize = volume->iso_block_size;
    value->f_blocks = volume->iso_sector_count;
    value->f_flags |= LINUX_ST_RDONLY;
    return 0;
}
const struct storage_filesystem_ops storage_iso_ops={
    .flags=STORAGE_FS_READONLY,
    .statfs=iso_native_statfs,
    .name="iso9660",
    .owner=THIS_MODULE,
    .mount=iso_native_mount,
    .read=iso_native_read,
    .readdir=iso_native_readdir,
    .root=iso_native_root,
    .lookup=iso_native_lookup
};
