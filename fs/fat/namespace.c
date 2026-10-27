/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <linux/mount.h>

/** @brief Create a FAT directory in one selected actual parent inode.
 * @param parent Borrowed directory on the execution-owned selected volume.
 * @param name Valid single member component.
 * @param out Optional created metadata, without transferring an inode hold.
 * @return Zero or negative errno. Caller retains the parent/volume and checks
 * permissions. Failed post-create metadata lookup removes the new chain/entry;
 * no global pathname lookup or mount-prefix routing is performed.
 */
int fat32_mkdir_child(const struct storage_node *parent,const char *name,struct storage_node *out)
{
    if(!parent || !name || parent->volume_id!=g_storage.volume_id)return -22;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    struct storage_node existing;uint32_t cluster=0;
    int ret=fat32_find_in_dir(parent->first_cluster,name,&existing);
    if(!ret)return -17;
    if(ret!=-2)return ret;
    if (parent->flags & STORAGE_NODE_FLAG_DEV_DIR) {
        return -21;
    }
    ret = fat32_validate_name(name);
    if (ret < 0) {
        return ret;
    }
    ret = fat32_find_free_cluster(&cluster);
    if (ret < 0) {
        return ret;
    }
    storage_begin_mutation();
    if (fat32_write_fat_entry(cluster, FAT32_EOC) < 0) {
        return -5;
    }
    if (fat32_note_allocated() < 0) {
        return -5;
    }
    storage_memzero(storage_cluster_buf, g_storage.cluster_bytes);
    {
        struct fat32_dirent *dot = (struct fat32_dirent *)(void *)storage_cluster_buf;
        struct fat32_dirent *dotdot =
            (struct fat32_dirent *)(void *)(storage_cluster_buf + sizeof(struct fat32_dirent));
        storage_memzero(dot, sizeof(*dot));
        storage_memzero(dotdot, sizeof(*dotdot));
        dot->name[0] = '.';
        for (uint32_t i = 1; i < 11; ++i) {
            dot->name[i] = ' ';
        }
        dot->attr = FAT32_ATTR_DIRECTORY;
        dot->first_cluster_hi = (uint16_t)(cluster >> 16);
        dot->first_cluster_lo = (uint16_t)(cluster & 0xffffu);
        dotdot->name[0] = '.';
        dotdot->name[1] = '.';
        for (uint32_t i = 2; i < 11; ++i) {
            dotdot->name[i] = ' ';
        }
        dotdot->attr = FAT32_ATTR_DIRECTORY;
        /* FAT32 encodes the parent of a root child as cluster zero, not the
         * root directory's physical cluster number. */
        uint32_t dotdot_cluster = (parent->flags & STORAGE_NODE_FLAG_ROOT)
                                      ? 0u : parent->first_cluster;
        dotdot->first_cluster_hi = (uint16_t)(dotdot_cluster >> 16);
        dotdot->first_cluster_lo = (uint16_t)(dotdot_cluster & 0xffffu);
    }
    if (fat32_write_cluster(cluster, storage_cluster_buf) < 0) {
        (void)fat32_write_fat_entry(cluster, 0);
        (void)fat32_note_freed();
        return -5;
    }
    ret = fat32_create_dirent(parent->first_cluster, name, FAT32_ATTR_DIRECTORY, cluster, 0);
    if (ret < 0) {
        (void)fat32_free_chain(cluster);
        return ret;
    }
    if(out) {
        ret=fat32_find_in_dir(parent->first_cluster,name,out);
        if(ret) {
            int cleanup=fat32_delete_dirent(parent->first_cluster,name,NULL);
            if(!cleanup)cleanup=fat32_free_chain(cluster);
            if(cleanup)return cleanup;
        }
        return ret;
    }
    return 0;
}

int fat32_remove_child(const struct storage_node *parent,const char *name,bool directory)
{
    if(!parent || !name || parent->volume_id!=g_storage.volume_id)return -22;
    if(g_storage.mount_flags&MS_RDONLY)return -30;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    if(storage_text_eq_ci(name,"LEONACL.SYS"))return -1;
    struct storage_node node;
    int ret=fat32_find_in_dir(parent->first_cluster,name,&node);if(ret)return ret;
    if((node.type==RELIEFOS_FS_TYPE_DIR)!=directory)return directory ? -20 : -21;
    if(directory) {
        ret=fat32_dir_is_empty(node.first_cluster);
        if(ret<=0)return ret ? ret : -39;
    }
    storage_begin_mutation();
    if(directory) {
        ret=fat32_delete_acl_metadata_file(node.first_cluster);if(ret)return ret;
    }
    ret=fat32_delete_dirent(parent->first_cluster,name,NULL);if(ret)return ret;
    bool retained=storage_inode_unlinked(&node);
    storage_cache_invalidate();
    return !retained && node.first_cluster>=2 ? fat32_free_chain(node.first_cluster) : 0;
}

/* FAT specifies dot and dot-dot as the first two directory records. Update
 * only the held directory's actual parent field, without rebuilding a path.
 * The caller checks ancestry in the mount graph before invoking this core. */
static int fat32_reparent(const struct storage_node *directory,const struct storage_node *old_parent,
                             const struct storage_node *new_parent)
{
    if(directory->type!=RELIEFOS_FS_TYPE_DIR || old_parent->first_cluster==new_parent->first_cluster)return 0;
    int rc=fat32_read_cluster(directory->first_cluster,storage_cluster_buf);if(rc)return rc;
    struct fat32_dir_ref reference={.entry_cluster=directory->first_cluster,.entry_offset=32};
    storage_memcpy(&reference.dirent,storage_cluster_buf+32,sizeof(reference.dirent));
    if(reference.dirent.name[0]!='.' || reference.dirent.name[1]!='.' ||
       !(reference.dirent.attr&FAT32_ATTR_DIRECTORY))return -5;
    for(unsigned i=2;i<11;++i)if(reference.dirent.name[i]!=' ')return -5;
    uint32_t previous=(uint32_t)reference.dirent.first_cluster_hi<<16 | reference.dirent.first_cluster_lo;
    uint32_t expected=old_parent->flags&STORAGE_NODE_FLAG_ROOT ? 0 : old_parent->first_cluster;
    if(previous!=expected)return -5;
    uint32_t cluster=new_parent->flags&STORAGE_NODE_FLAG_ROOT ? 0 : new_parent->first_cluster;
    reference.dirent.first_cluster_hi=(uint16_t)(cluster>>16);reference.dirent.first_cluster_lo=(uint16_t)cluster;
    return fat32_update_dirent(&reference);
}

/** @brief Preserve all native creation/modification/access fields during rename.
 * @param target Required destination record, retaining its new name/case encoding.
 * @param source Required original record, borrowed before mutation.
 * @return None. Copies only timestamp fields; pure execution-owned metadata math.
 */
static void fat32_copy_rename_times(struct fat32_dirent *target,const struct fat32_dirent *source)
{
    target->crt_time_tenth=source->crt_time_tenth;target->crt_time=source->crt_time;
    target->crt_date=source->crt_date;target->last_access_date=source->last_access_date;
    target->wrt_time=source->wrt_time;target->wrt_date=source->wrt_date;
}
/** @brief Rename between explicit retained FAT parent directories.
 * @param parent Required current source directory. @param old_name Literal source leaf.
 * @param destination Required target directory. @param new_name Validated target leaf.
 * @param committed Required irreversible-publication output, initially false.
 * @return Zero or negative native metadata/validation/I/O errno. Errors after
 * publication retain committed=true. Source times survive both new-target and
 * replacement renames; failed publication restores the original target record.
 * Caller owns execution and all inode/code pins, checks mountpoint/ancestry and
 * reserves mutation resources. No global path resolution or callback locks.
 */
int fat32_rename_child(const struct storage_node *parent,const char *old_name,
                          const struct storage_node *destination,const char *new_name,bool *committed)
{
    if(!committed)return -22;*committed=false;
    if(!parent || !destination || !old_name || !new_name || parent->volume_id!=g_storage.volume_id ||
       destination->volume_id!=g_storage.volume_id)return -22;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR || destination->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    if(g_storage.mount_flags&MS_RDONLY)return -30;
    if(storage_text_eq_ci(old_name,"LEONACL.SYS") || storage_text_eq_ci(new_name,"LEONACL.SYS"))return -1;
    int ret=fat32_validate_name(new_name);if(ret)return ret;
    struct storage_node node,existing;struct fat32_dirent deleted;uint32_t first_cluster;
    if (parent->first_cluster==destination->first_cluster && storage_text_eq_ci(old_name,new_name))return 0;
    ret = fat32_find_in_dir(parent->first_cluster,old_name,&node);
    if (ret < 0) {
        return ret;
    }
    struct fat32_dir_ref source;
    ret=fat32_find_dirent_ref_in_dir(parent->first_cluster,old_name,&source);if(ret)return ret;
    ret = fat32_find_in_dir(destination->first_cluster,new_name,&existing);
    if (ret == 0) {
        struct fat32_dir_ref target;
        if (node.type != existing.type) return node.type == RELIEFOS_FS_TYPE_DIR ? -20 : -21;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) {
            ret = fat32_dir_is_empty(existing.first_cluster);
            if (ret <= 0) return ret < 0 ? ret : -39;
        }
        ret = fat32_find_dirent_ref_in_dir(destination->first_cluster,new_name,&target);
        if (ret < 0) return ret;
        struct fat32_dirent saved = target.dirent;
        fat32_copy_rename_times(&target.dirent,&source.dirent);
        target.dirent.first_cluster_hi = (uint16_t)(node.first_cluster >> 16);
        target.dirent.first_cluster_lo = (uint16_t)node.first_cluster;
        target.dirent.size = node.type == RELIEFOS_FS_TYPE_FILE ? (uint32_t)node.size : 0;
        storage_begin_mutation();
        ret=fat32_reparent(&node,parent,destination);if(ret)return ret;
        ret=fat32_update_dirent(&target);
        if(ret){(void)fat32_reparent(&node,destination,parent);return ret;}
        ret = fat32_delete_dirent(parent->first_cluster, old_name, &deleted);
        if (ret < 0) {
            target.dirent = saved;
            (void)fat32_update_dirent(&target);
            (void)fat32_reparent(&node,destination,parent);
            return ret;
        }
        *committed=true;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) ret = fat32_delete_acl_metadata_file(existing.first_cluster);
        bool retained=storage_inode_unlinked(&existing);
        struct storage_node moved=node;
        moved.entry_id=(uint64_t)target.entry_cluster<<32 | target.entry_offset;
        storage_inode_note_move(&node,&moved);
        if (!ret && !retained && existing.first_cluster >= 2) ret = fat32_free_chain(existing.first_cluster);
        storage_cache_invalidate();
        return ret;
    }
    if (ret != -2) {
        return ret;
    }
    first_cluster = node.first_cluster;
    storage_begin_mutation();
    ret = fat32_create_dirent(destination->first_cluster,new_name,
                              node.type == RELIEFOS_FS_TYPE_DIR ? FAT32_ATTR_DIRECTORY : FAT32_ATTR_ARCHIVE,
                              first_cluster, node.type == RELIEFOS_FS_TYPE_FILE ? (uint32_t)node.size : 0);
    if (ret < 0) {
        return ret;
    }
    struct fat32_dir_ref moved_ref;
    ret=fat32_find_dirent_ref_in_dir(destination->first_cluster,new_name,&moved_ref);
    if(ret){(void)fat32_delete_dirent(destination->first_cluster,new_name,NULL);return ret;}
    fat32_copy_rename_times(&moved_ref.dirent,&source.dirent);
    ret=fat32_update_dirent(&moved_ref);
    if(ret){(void)fat32_delete_dirent(destination->first_cluster,new_name,NULL);return ret;}
    ret=fat32_reparent(&node,parent,destination);
    if(ret){(void)fat32_delete_dirent(destination->first_cluster,new_name,NULL);return ret;}
    ret=fat32_delete_dirent(parent->first_cluster,old_name,&deleted);
    if(ret) {
        (void)fat32_reparent(&node,destination,parent);
        (void)fat32_delete_dirent(destination->first_cluster,new_name,NULL);return ret;
    }
    *committed=true;
    struct storage_node moved=node;
    moved.entry_id=(uint64_t)moved_ref.entry_cluster<<32 | moved_ref.entry_offset;
    storage_inode_note_move(&node,&moved);
    return 0;
}
