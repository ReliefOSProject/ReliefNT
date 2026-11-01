/* SPDX-License-Identifier: Apache-2.0 */
#include "../partition/internal.h"
#include <reliefnt/proc_stats.h>

/** @brief Emit one Linux partition or completed-I/O statistics row.
 * @param s Required stream. @param device Wide dev_t. @param name Display only.
 * @param sectors Geometry in 512-byte sectors. @param st Actual counters or NULL.
 */
static void proc_disk_row(struct text_stream *s, uint64_t device, const char *name,
                          uint64_t sectors, const struct block_io_stats *st)
{
    if (!__builtin_strncmp(name, "/dev/", 5))
        name += 5;
    text_unsigned(s, (device >> 8) & 4095u);
    text_string(s, " ");
    text_unsigned(s, (device & 255u) | ((device >> 12) & 0xfffff00u));
    text_string(s, " ");
    if (!st) {
        text_unsigned(s, sectors / 2);
        text_string(s, " ");
        text_string(s, name);
    } else {
        text_string(s, name);
        uint64_t fields[] = {st->reads,     0,         st->read_sectors,  st->read_ms,
                             st->writes,    0,         st->write_sectors, st->write_ms,
                             st->in_flight, st->io_ms, st->weighted_ms};
        for (unsigned i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
            text_string(s, " ");
            text_unsigned(s, fields[i]);
        }
    }
    text_string(s, "\n");
}
/** @brief Enumerate current disks and real partition geometry with balanced refs.
 * @param s Output. @param statistics Select whole-device completion counters.
 * @return Zero or allocation/geometry/native errno; execution ownership held.
 * Partition parsing can perform metadata I/O; never holds a registry spinlock.
 */
int block_proc_emit(struct text_stream *s, bool statistics)
{
    int rc = storage_refresh_disks();
    if (rc)
        return rc;
    if (!statistics)
        text_string(s, "major minor  #blocks  name\n\n");
    for (uint32_t i = 0; i < g_install_disk_count; ++i) {
        struct install_disk_state *disk = &g_install_disks[i];
        if (!disk->present)
            continue;
        struct block_device_ref ref = {0};
        rc = block_device_get(disk->device, &ref);
        if (rc)
            continue;
        struct block_device_info info;
        struct block_io_stats st;
        rc = block_device_info(&ref, &info);
        if (!rc)
            rc = block_device_statistics(&ref, &st);
        block_device_put(&ref);
        if (rc)
            return rc;
        char name[RELIEFOS_FS_PATH_LEN];
        rc = storage_disk_device_name(i, -1, name, sizeof(name));
        if (rc)
            return rc;
        proc_disk_row(s, disk->device, name, info.blocks * (info.block_size / 512),
                      statistics ? &st : NULL);
        if (statistics)
            continue;
        uint64_t first, count;
        for (uint32_t p = 0; p < RELIEFOS_DISK_MAX_PARTITIONS; ++p) {
            rc = storage_disk_block_info(i, p, &first, &count);
            if (rc == -2 || rc == -22)
                continue;
            if (rc)
                return rc;
            rc = storage_disk_device_name(i, p, name, sizeof(name));
            if (rc)
                return rc;
            proc_disk_row(s, storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(i, p)), name, count, NULL);
        }
    }
    uint64_t cursor = 0;
    uint32_t optical;
    int next;
    while ((next = storage_optical_next(&cursor, &optical)) > 0) {
        struct block_device_ref ref = {0};
        rc = storage_disk_block_get(optical, &ref);
        if (rc)
            return rc;
        struct block_device_info info;
        struct block_io_stats st;
        rc = block_device_info(&ref, &info);
        if (!rc)
            rc = block_device_statistics(&ref, &st);
        block_device_put(&ref);
        if (rc)
            return rc;
        char name[RELIEFOS_FS_PATH_LEN];
        rc = storage_disk_device_name(optical, -1, name, sizeof(name));
        if (rc)
            return rc;
        proc_disk_row(s, info.device, name, info.blocks * (info.block_size / 512),
                      statistics ? &st : NULL);
    }
    if (next < 0)
        return next;
    return 0;
}
