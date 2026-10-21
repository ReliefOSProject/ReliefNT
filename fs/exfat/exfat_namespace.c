#include "internal.h"

static int exfat_delete_acl_metadata_file(const struct storage_node *directory);


int exfat_mkdir(const char *path)
{
    char parent_path[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent;
    struct storage_node existing;
    struct exfat_dir_ref parent_ref;
    uint32_t cluster;
    uint8_t nofat;
    int ret;
    if (!path) return -22;
    ret = storage_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret < 0) return ret;
    ret = exfat_lookup_path_ref(parent_path, &parent, &parent_ref);
    if (ret < 0) return ret;
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_lookup_path(path, &existing);
    if (ret == 0) return -17;
    if (ret != -2) return ret;
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
    ret = exfat_create_entry(parent.first_cluster,
                             (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                             (parent.flags & STORAGE_NODE_FLAG_ROOT) ? 0 : &parent_ref,
                             name, 1u, cluster, g_storage.cluster_bytes, 0u, 0);
    if (ret < 0) {
        (void)exfat_free_clusters(cluster, 0u, g_storage.cluster_bytes);
        console_error("[reliefnt] exfat mkdir stage=entry name=%s cluster=%u ret=%d\n",
                       name, cluster, ret);
        return ret;
    }
    storage_cache_invalidate();
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
        ret = exfat_free_clusters(metadata.first_cluster,
                                  (metadata.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                  metadata.size);
    }
    return ret;
}

int exfat_unlink(const char *path)
{
    char parent_path[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent, node;
    struct exfat_dir_ref ref;
    int ret;
    if (!path) return -22;
    ret = storage_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret < 0) return ret;
    ret = exfat_lookup_path(parent_path, &parent);
    if (ret < 0) return ret;
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                                (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                name, &node, &ref);
    if (ret < 0) return ret;
    if (node.type != RELIEFOS_FS_TYPE_FILE) return -21;
    storage_begin_mutation();
    ret = exfat_delete_entry(&ref);
    if (ret == 0) ret = exfat_free_clusters(node.first_cluster,
                                             (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                             node.size);
    if (ret == 0) storage_cache_invalidate();
    return ret;
}

int exfat_rmdir(const char *path)
{
    char parent_path[RELIEFOS_FS_PATH_LEN];
    char name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent, node;
    struct exfat_dir_ref ref;
    int ret;
    if (!path) return -22;
    ret = storage_parent_path(path, parent_path, sizeof(parent_path), name, sizeof(name));
    if (ret < 0) return ret;
    ret = exfat_lookup_path(parent_path, &parent);
    if (ret < 0) return ret;
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                                (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                name, &node, &ref);
    if (ret < 0) return ret;
    if (node.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_dir_is_empty(&node);
    if (ret <= 0) return ret < 0 ? ret : -39;
    storage_begin_mutation();
    ret = exfat_delete_acl_metadata_file(&node);
    if (ret == 0) ret = exfat_delete_entry(&ref);
    if (ret == 0) ret = exfat_free_clusters(node.first_cluster,
                                             (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                             node.size ? node.size : g_storage.cluster_bytes);
    if (ret == 0) storage_cache_invalidate();
    return ret;
}

int exfat_rename(const char *old_path, const char *new_path)
{
    char old_parent_path[RELIEFOS_FS_PATH_LEN];
    char new_parent_path[RELIEFOS_FS_PATH_LEN];
    char old_name[RELIEFOS_FS_NAME_LEN];
    char new_name[RELIEFOS_FS_NAME_LEN];
    struct storage_node parent, node, existing;
    struct exfat_dir_ref parent_ref;
    struct exfat_dir_ref ref;
    struct exfat_dir_ref target_ref;
    int ret;
    if (!old_path || !new_path) return -22;
    ret = storage_parent_path(old_path, old_parent_path, sizeof(old_parent_path),
                              old_name, sizeof(old_name));
    if (ret < 0) return ret;
    ret = storage_parent_path(new_path, new_parent_path, sizeof(new_parent_path),
                              new_name, sizeof(new_name));
    if (ret < 0) return ret;
    if (!storage_text_eq_ci(old_parent_path, new_parent_path)) return -22;
    {
        uint16_t converted_name[255];
        uint32_t converted_length = 0;
        ret = exfat_utf8_name(new_name, converted_name, &converted_length);
        if (ret < 0) return ret;
    }
    ret = exfat_lookup_path_ref(old_parent_path, &parent, &parent_ref);
    if (ret < 0) return ret;
    if (parent.type != RELIEFOS_FS_TYPE_DIR) return -20;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                                (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                                old_name, &node, &ref);
    if (ret < 0) return ret;
    if (storage_text_eq_ci(old_name, new_name)) return 0;
    ret = exfat_find_in_dir_ref(parent.first_cluster,
                            (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, new_name, &existing, &target_ref);
    if (ret == 0) {
        if (node.type != existing.type) return node.type == RELIEFOS_FS_TYPE_DIR ? -20 : -21;
        if (existing.type == RELIEFOS_FS_TYPE_DIR) {
            ret = exfat_dir_is_empty(&existing);
            if (ret <= 0) return ret < 0 ? ret : -39;
        }
        storage_begin_mutation();
        /* Publish the source chain under the existing target name before
         * removing the old name. No lookup can observe a missing target. */
        ret = exfat_update_entry_data(&target_ref, node.first_cluster, node.size,
                                      (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0);
        if (ret < 0) return ret;
        ret = exfat_delete_entry(&ref);
        if (ret < 0) {
            (void)exfat_update_entry_data(&target_ref, existing.first_cluster, existing.size,
                                         (existing.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0);
            return ret;
        }
        if (existing.type == RELIEFOS_FS_TYPE_DIR) ret = exfat_delete_acl_metadata_file(&existing);
        if (!ret) ret = exfat_free_clusters(existing.first_cluster,
                            (existing.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, existing.size);
        storage_cache_invalidate();
        return ret;
    }
    if (ret != -2) return ret;
    storage_begin_mutation();
    ret = exfat_create_entry(parent.first_cluster, (parent.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0,
                             (parent.flags & STORAGE_NODE_FLAG_ROOT) ? 0 : &parent_ref,
                             new_name, node.type == RELIEFOS_FS_TYPE_DIR, node.first_cluster,
                             node.size, (node.flags & STORAGE_NODE_FLAG_EXFAT_NOFAT) != 0, &target_ref);
    if (ret == 0) {
        ret = exfat_delete_entry(&ref);
        if (ret < 0) (void)exfat_delete_entry(&target_ref);
    }
    if (ret == 0) storage_cache_invalidate();
    return ret;
}
