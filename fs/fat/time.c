/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "../dos_time.h"

/** @brief Read the exact linked FAT directory record of a retained inode.
 * @param node Required current native node in the execution-selected volume.
 * @param out Required copied directory record with its physical metadata location.
 * @return Zero or negative validation/stale/I/O errno. Roots have no record.
 * Execution ownership required; output owns no pointer into shared I/O buffers.
 */
static int fat_time_reference(const struct storage_node *node,struct fat32_dir_ref *out)
{
    if(!node || !out || node->volume_id!=g_storage.volume_id || !(node->flags&STORAGE_NODE_FLAG_FAT32))return -22;
    *out=(struct fat32_dir_ref){.entry_cluster=(uint32_t)(node->entry_id>>32),.entry_offset=(uint32_t)node->entry_id};
    if(out->entry_cluster<2 || out->entry_offset%32 ||
       out->entry_offset>g_storage.cluster_bytes || g_storage.cluster_bytes-out->entry_offset<32)return -116;
    int rc=fat32_read_cluster(out->entry_cluster,storage_cluster_buf);if(rc)return rc;
    storage_memcpy(&out->dirent,storage_cluster_buf+out->entry_offset,sizeof(out->dirent));
    const struct fat32_dirent *entry=&out->dirent;
    if(!entry->name[0] || entry->name[0]==0xe5 || entry->attr==FAT32_ATTR_LFN || (entry->attr&FAT32_ATTR_VOLUME))return -116;
    uint32_t cluster=((uint32_t)entry->first_cluster_hi<<16)|entry->first_cluster_lo;
    return cluster==node->first_cluster ? 0 : -116;
}
/** @brief Decode native FAT modification/access fields without a pathname.
 * @param node Borrowed retained node in the selected live FAT volume.
 * @param out Required output, cleared on failure; invalid date fields stay unknown.
 * @return Zero or negative record/I/O errno. Roots have no persistent timestamp.
 * FAT has no stored status-change time; initial ctime follows native mtime,
 * matching the Linux FAT inode view. Execution ownership; no reference transfer.
 */
int fat32_inode_times(const struct storage_node *node,struct storage_inode_times *out)
{
    if(!out)return -22;
    *out=(struct storage_inode_times){0};
    if(!node || node->volume_id!=g_storage.volume_id)return -22;
    if(node->flags&STORAGE_NODE_FLAG_ROOT)return 0;
    struct fat32_dir_ref ref;int rc=fat_time_reference(node,&ref);if(rc)return rc;
    (void)storage_dos_decode(ref.dirent.last_access_date,0,0,&out->atime,&out->atime_nsec);
    (void)storage_dos_decode(ref.dirent.wrt_date,ref.dirent.wrt_time,0,&out->mtime,&out->mtime_nsec);
    out->ctime=out->mtime;out->ctime_nsec=out->mtime_nsec;return 0;
}
/** @brief Update only a retained FAT inode's access-date field in its current record.
 * @param node Required linked node in the selected volume; roots have no record.
 * @param seconds Current UTC seconds, clamped to the native1980..2107 date range.
 * @param actual Required normalized midnight-seconds output, unchanged on failure.
 * @return Zero or negative record/I/O errno. Execution ownership/synchronous I/O
 * required. Unchanged dates avoid writes; mutation is latched before updating.
 * Caller owns the inode pin and excludes orphans from all directory-record I/O.
 */
int fat32_inode_access(const struct storage_node *node,int64_t seconds,int64_t *actual)
{
    if(!node || !actual || node->volume_id!=g_storage.volume_id)return -22;
    struct storage_dos_time encoded;int rc=storage_dos_encode(seconds,0,&encoded);if(rc)return rc;
    int64_t normalized;uint32_t fraction;
    rc=storage_dos_decode(encoded.date,0,0,&normalized,&fraction);if(rc)return rc;
    if(!(node->flags&STORAGE_NODE_FLAG_ROOT)) {
        struct fat32_dir_ref ref;rc=fat_time_reference(node,&ref);if(rc)return rc;
        if(ref.dirent.last_access_date!=encoded.date) {
            ref.dirent.last_access_date=encoded.date;storage_begin_mutation();
            rc=fat32_update_dirent(&ref);if(rc)return rc;
        }
    }
    *actual=normalized;return 0;
}
