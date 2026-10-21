#include "internal.h"



int fat32_find_free_cluster(uint32_t *out_cluster)
{
    uint32_t max_cluster;
    uint32_t start;
    if (!out_cluster) {
        return -22;
    }
    max_cluster = g_storage.data_cluster_count + 1u;
    start = g_storage.next_free_cluster;
    if (start < 2 || start > max_cluster) {
        start = 2;
    }
    for (uint32_t pass = 0; pass < 2; ++pass) {
        uint32_t begin = pass == 0 ? start : 2;
        uint32_t end = pass == 0 ? max_cluster : (start > 2 ? start - 1u : 1u);
        for (uint32_t cluster = begin; cluster <= end; ++cluster) {
            uint32_t value = 0;
            int ret = fat32_read_fat_entry(cluster, &value);
            if (ret < 0) {
                return storage_read_failure(ret);
            }
            if (value == 0) {
                *out_cluster = cluster;
                g_storage.next_free_cluster = cluster < max_cluster ? cluster + 1u : 2u;
                return 0;
            }
        }
    }
    return -28;
}

int fat32_collect_chain(uint32_t first_cluster, uint32_t chain[FAT32_MAX_FILE_CLUSTERS], uint32_t *out_count)
{
    uint32_t count = 0;
    uint32_t cluster = first_cluster;
    if (!out_count || !chain) {
        return -22;
    }
    *out_count = 0;
    if (cluster < 2) {
        return 0;
    }
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = 0;
        if (count >= FAT32_MAX_FILE_CLUSTERS) {
            return -28;
        }
        chain[count++] = cluster;
        int ret = fat32_read_fat_entry(cluster, &next);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        if (next >= FAT32_EOC) {
            break;
        }
        cluster = next;
    }
    *out_count = count;
    return 0;
}

int fat32_free_chain(uint32_t first_cluster)
{
    uint32_t cluster = first_cluster;
    uint32_t count = 0;
    if (first_cluster >= 2 &&
        (g_storage.next_free_cluster < 2 || first_cluster < g_storage.next_free_cluster)) {
        g_storage.next_free_cluster = first_cluster;
    }
    while (cluster >= 2 && cluster < FAT32_EOC) {
        uint32_t next = 0;
        if (count++ >= FAT32_MAX_FILE_CLUSTERS) {
            return -28;
        }
        int ret = fat32_read_fat_entry(cluster, &next);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        ret = fat32_write_fat_entry(cluster, 0);
        if (ret < 0) {
            return storage_read_failure(ret);
        }
        ret = fat32_note_freed();
        if (ret < 0) {
            return ret;
        }
        if (next >= FAT32_EOC) {
            break;
        }
        cluster = next;
    }
    return 0;
}
