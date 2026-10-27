#include "internal.h"
#include <linux/mount.h>

static int exfat_delete_acl_metadata_file(const struct storage_node *directory);


/* Reconstruct the selected record reference from its retained inode identity,
 * including secondary-count validation before directory growth updates it. */
int exfat_node_ref(const struct storage_node *node,struct exfat_dir_ref *out)
{
    if(!node || !out || node->volume_id!=g_storage.volume_id)return -22;
    *out=(struct exfat_dir_ref){0};
    if(node->flags&STORAGE_NODE_FLAG_ROOT)return 0;
    out->directory_cluster=(uint32_t)(node->entry_id>>32)&0x7fffffffu;
    out->directory_nofat=!!(node->entry_id&(UINT64_C(1)<<63));
    out->first_entry=(uint32_t)node->entry_id;
    uint8_t raw[EXFAT_ENTRY_SIZE];
    int ret=exfat_dir_read_entry(out->directory_cluster,out->directory_nofat,out->first_entry,raw);
    if(!ret && (raw[0]!=0x85 || raw[1]<2 || raw[1]>19))ret=-5;
    if(!ret)out->secondary_count=raw[1];
    return ret;
}

int exfat_mkdir_child(const struct storage_node *parent,const char *name,struct storage_node *out)
{
    if(!parent || !name || parent->volume_id!=g_storage.volume_id)return -22;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    struct storage_node existing;struct exfat_dir_ref parent_ref,created;
    uint32_t cluster;uint8_t nofat;
    int ret=exfat_find_in_dir_ref(parent->first_cluster,
        !!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),name,&existing,NULL);
    if(!ret)return -17;
    if(ret!=-2)return ret;
    ret=exfat_node_ref(parent,&parent_ref);if(ret)return ret;
    storage_begin_mutation();
    ret = exfat_allocate_clusters(1u, &cluster, &nofat);
    if (ret < 0) return ret;
    /* Directories are always FAT chained in ReliefOS so that they can grow. */
    if (nofat) {
        ret = exfat_write_fat(cluster, EXFAT_EOC);
        if (ret < 0) {
            (void)exfat_free_clusters(cluster, nofat, g_storage.cluster_bytes);
            console_error("[reliefnt] exfat mkdir stage=link cluster=%u ret=%d\n", cluster, ret);
            return ret;
        }
    }
    ret = exfat_zero_clusters(cluster, 0u, 1u);
    if (ret < 0) {
        (void)exfat_free_clusters(cluster, 0u, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat mkdir stage=zero cluster=%u ret=%d\n", cluster, ret);
        return ret;
    }
    ret = exfat_create_entry(parent->first_cluster,
                             (parent->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                             (parent->flags & STORAGE_NODE_FLAG_ROOT) ? 0 : &parent_ref,
                             name, 1u, cluster, g_storage.cluster_bytes, 0u, &created);
    if (ret < 0) {
        (void)exfat_free_clusters(cluster, 0u, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat mkdir stage=entry name=%s cluster=%u ret=%d\n",
                       name, cluster, ret);
        return ret;
    }
    storage_cache_invalidate();
    if(out) {
        ret=exfat_find_in_dir_ref(parent->first_cluster,
            !!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),name,out,NULL);
        if(ret) {
            int cleanup=exfat_delete_entry(&created);
            if(!cleanup)cleanup=exfat_free_clusters(cluster,0,g_storage.cluster_bytes);
            if(cleanup)return cleanup;
        }
        return ret;
    }
    return 0;
}


static int exfat_delete_acl_metadata_file(const struct storage_node *directory)
{
    struct storage_node metadata;
    struct exfat_dir_ref metadata_ref;
    int ret;

    ret = exfat_find_in_dir_ref(directory->first_cluster,
                                (directory->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                "LEONACL.SYS", &metadata, &metadata_ref);
    if (ret == -2) return 0;
    if (ret < 0) return ret;
    if (metadata.type != RELIEFOS_FS_TYPE_FILE) return -5;
    ret = exfat_delete_entry(&metadata_ref);
    if (ret == 0) {
        bool retained=storage_inode_unlinked(&metadata);
        storage_cache_invalidate();
        if(retained)return 0;
        ret = exfat_free_clusters(metadata.first_cluster,
                                  (metadata.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                  metadata.size);
    }
    return ret;
}

int exfat_remove_child(const struct storage_node *parent,const char *name,bool directory)
{
    if(!parent || !name || parent->volume_id!=g_storage.volume_id)return -22;
    if(g_storage.mount_flags&MS_RDONLY)return -30;
    if(parent->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    if(storage_text_eq_ci(name,"LEONACL.SYS"))return -1;
    struct storage_node node;struct exfat_dir_ref ref;
    int ret=exfat_find_in_dir_ref(parent->first_cluster,
        !!(parent->flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),name,&node,&ref);
    if(ret)return ret;
    if((node.type==RELIEFOS_FS_TYPE_DIR)!=directory)return directory ? -20 : -21;
    if(directory) {
        ret=exfat_dir_is_empty(&node);
        if(ret<=0)return ret ? ret : -39;
    }
    storage_begin_mutation();
    if(directory) {
        ret=exfat_delete_acl_metadata_file(&node);if(ret)return ret;
    }
    ret=exfat_delete_entry(&ref);if(ret)return ret;
    bool retained=storage_inode_unlinked(&node);storage_cache_invalidate();
    return retained ? 0 : exfat_free_clusters(node.first_cluster,
        !!(node.flags&STORAGE_NODE_FLAG_EXFAT_NOFAT),node.size ? node.size : directory ? g_storage.cluster_bytes : 0);
}


/** @brief Rename between explicit retained exFAT parent directories.
 * @param directory Required source directory. @param old_name Literal source leaf.
 * @param destination Required target directory. @param new_name Validated target leaf.
 * @param committed Required irreversible-publication output, initially false.
 * @return Zero or negative native validation/metadata/I/O errno. Errors after
 * publication retain committed=true. Both rename branches preserve all native
 * timestamps; replacement failures restore old target data/time records.
 * Caller owns execution, source/target/code pins and pre-reserved mutation
 * resources. Timestamp transfer uses stack storage; native directory growth
 * follows its existing reservation rules. No global path/callback locks.
 */
int exfat_rename_child(const struct storage_node *directory,const char *old_name,
                          const struct storage_node *destination,const char *new_name,bool *committed)
{
    if(!committed)return -22;*committed=false;
    if(!directory || !destination || !old_name || !new_name || directory->volume_id!=g_storage.volume_id ||
       destination->volume_id!=g_storage.volume_id)return -22;
    if(directory->type!=RELIEFOS_FS_TYPE_DIR || destination->type!=RELIEFOS_FS_TYPE_DIR)return -20;
    if(g_storage.mount_flags&MS_RDONLY)return -30;
    if(storage_text_eq_ci(old_name,"LEONACL.SYS") || storage_text_eq_ci(new_name,"LEONACL.SYS"))return -1;
    struct storage_node parent=*directory,node,existing;
    struct exfat_dir_ref parent_ref,ref,target_ref;int ret;
    {
        uint16_t converted_name[255];
        uint32_t converted_length = 0;
        ret = exfat_utf8_name(new_name, converted_name, &converted_length);
        if (ret < 0) return ret;
    }
    ret=exfat_node_ref(destination,&parent_ref);if(ret)return ret;
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                                (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                old_name, &node, &ref);
    if (ret < 0) return ret;
    if (directory->first_cluster==destination->first_cluster && storage_text_eq_ci(old_name,new_name))return 0;
    struct exfat_entry_timestamps source_times;
    ret=exfat_entry_times_read(&ref,&source_times);if(ret)return ret;
    ret = exfat_find_in_dir_ref(destination->first_cluster,
                            (destination->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, new_name, &existing, &target_ref);
    if (ret == 0) {
        if (node.type != existing.type) return node.type == RELIEFOS_FS_TYPE_DIR ? -20 : -21;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) {
            ret = exfat_dir_is_empty(&existing);
            if (ret <= 0) return ret < 0 ? ret : -39;
        }
        struct exfat_entry_timestamps saved_times;
        ret=exfat_entry_times_read(&target_ref,&saved_times);if(ret)return ret;
        storage_begin_mutation();
        /* Publish the source chain under the existing target name before
         * removing the old name. No lookup can observe a missing target. */
        ret = exfat_update_entry_data(&target_ref, node.first_cluster, node.size,
                                      (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0);
        if(!ret)ret=exfat_entry_times_write(&target_ref,&source_times);
        if(!ret)ret=exfat_delete_entry(&ref);
        if (ret < 0) {
            int restore_data=exfat_update_entry_data(&target_ref, existing.first_cluster, existing.size,
                                         (existing.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0);
            int restore_times=exfat_entry_times_write(&target_ref,&saved_times);
            return restore_data ? restore_data : restore_times ? restore_times : ret;
        }
        *committed=true;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) ret = exfat_delete_acl_metadata_file(&existing);
        bool retained=storage_inode_unlinked(&existing);
        struct storage_node moved=node;
        moved.entry_id=(uint64_t)target_ref.directory_cluster<<32 | target_ref.first_entry |
            (target_ref.directory_nofat ? UINT64_C(1)<<63 : 0);
        storage_inode_note_move(&node,&moved);
        if (!ret && !retained) ret = exfat_free_clusters(existing.first_cluster,
                            (existing.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, existing.size);
        storage_cache_invalidate();
        return ret;
    }
    if (ret != -2) return ret;
    storage_begin_mutation();
    ret = exfat_create_entry(destination->first_cluster,(destination->flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                             (destination->flags & STORAGE_NODE_FLAG_ROOT) ? 0 : &parent_ref,
                             new_name, node.type == RELIEFOS_FS_TYPE_DIR, node.first_cluster,
                             node.size, (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, &target_ref);
    if (ret == 0) {
        ret=exfat_entry_times_write(&target_ref,&source_times);
        if(!ret)ret=exfat_delete_entry(&ref);
        if(ret<0) {
            int cleanup=exfat_delete_entry(&target_ref);
            if(cleanup)return cleanup;
        }
    }
    if (ret == 0) {
        *committed=true;
        struct storage_node moved=node;
        moved.entry_id=(uint64_t)target_ref.directory_cluster<<32 | target_ref.first_entry |
            (target_ref.directory_nofat ? UINT64_C(1)<<63 : 0);
        storage_inode_note_move(&node,&moved);
        storage_cache_invalidate();
    }
    return ret;
}
