/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include "internal.h"

/** @brief Choose a native exFAT cluster-size shift for a validated extent.
 * @param sector_count Extent length in 512-byte sectors.
 * @return Cluster-size shift. Pure arithmetic, no I/O or ownership transfer.
 */
static uint8_t install_choose_exfat_spc_shift(uint64_t sector_count)
{
    uint64_t bytes = sector_count * SECTOR_SIZE;
    if (bytes <= 256ULL * 1024ULL * 1024ULL) return 3u;   /* 4 KiB */
    if (bytes <= 32ULL * 1024ULL * 1024ULL * 1024ULL) return 6u; /* 32 KiB */
    return 8u; /* 128 KiB */
}

/** @brief Compute the native checksum over the generated exFAT boot region.
 * @param boot Borrowed complete 512-byte main boot sector.
 * @return Boot-region checksum including generated extended-sector signatures.
 * Pure byte arithmetic, no I/O or pointer retention.
 */
static uint32_t install_exfat_boot_checksum(const uint8_t boot[SECTOR_SIZE])
{
    uint32_t sum = 0;
    for (uint32_t sector = 0; sector < 11u; ++sector) {
        for (uint32_t byte = 0; byte < SECTOR_SIZE; ++byte) {
            /* The eight extended boot sectors carry the standard 0x55AA
             * signature.  They are part of the checksum even when their
             * executable payload is intentionally empty. */
            uint8_t value = sector ?
                ((sector <= 8u && byte == 510u) ? 0x55u :
                 (sector <= 8u && byte == 511u) ? 0xaau : 0u) : boot[byte];
            if (sector == 0u && (byte == 106u || byte == 107u || byte == 112u)) continue;
            sum = (sum << 31) | (sum >> 1);
            sum += value;
        }
    }
    return sum;
}

/** @brief Extend the native compressed upcase-stream checksum by one byte.
 * @param sum Prior checksum. @param value Next serialized byte.
 * @return Updated checksum; pure arithmetic without I/O or shared state.
 */
static uint32_t install_exfat_upcase_checksum(uint32_t sum, uint8_t value)
{
    return ((sum << 31) | (sum >> 1)) + value;
}

/*
 * Create the standard single-FAT exFAT subset used by ReliefOS.  The upcase
 * file is the Microsoft-recommended compressed UTF-16 table embedded by the
 * storage backend. It is the same standard table used by host mkfs.exfat.
 */
/** @brief Write native exFAT metadata into an authorized unmounted extent.
 * @param disk Borrowed managed target disk. @param start_lba First validated sector.
 * @param sector_count Validated extent length in 512-byte sectors.
 * @return Zero or layout/I/O errno. Execution ownership and admitted filesystem
 * provider pin required across all writes; no graph/module lock or queued work.
 */
int install_format_exfat(struct install_disk_state *disk, uint64_t start_lba,
                                uint64_t sector_count)
{
    uint8_t boot[SECTOR_SIZE];
    uint8_t spc_shift;
    uint32_t sectors_per_cluster;
    uint32_t fat_length = 1;
    uint32_t fat_offset = 24u;
    uint32_t heap_offset;
    uint32_t cluster_count = 0;
    uint64_t bitmap_bytes;
    uint32_t bitmap_clusters;
    const uint64_t upcase_bytes = exfat_standard_upcase_len;
    uint32_t upcase_clusters;
    /* Keep the system streams in the layout emitted by mkfs.exfat: the
     * allocation bitmap starts at cluster 2, the upcase stream follows it,
     * and the root directory follows both.  Some firmware/guest storage
     * implementations make assumptions about this canonical ordering. */
    uint32_t bitmap_cluster = 2u;
    uint32_t upcase_cluster;
    uint32_t root_cluster;
    uint32_t checksum = 0;
    int ret;
    if (!disk || sector_count < 262144ULL || sector_count > UINT32_MAX) return -28;
    spc_shift = install_choose_exfat_spc_shift(sector_count);
    sectors_per_cluster = 1u << spc_shift;
    for (uint32_t i = 0; i < 16u; ++i) {
        uint32_t next;
        heap_offset = fat_offset + fat_length;
        if (sector_count <= heap_offset) return -28;
        cluster_count = (uint32_t)((sector_count - heap_offset) / sectors_per_cluster);
        if (cluster_count < 16u) return -28;
        next = (uint32_t)((((uint64_t)cluster_count + 2u) * 4u + SECTOR_SIZE - 1u) /
                          SECTOR_SIZE);
        if (next == fat_length) break;
        fat_length = next;
    }
    heap_offset = fat_offset + fat_length;
    cluster_count = (uint32_t)((sector_count - heap_offset) / sectors_per_cluster);
    bitmap_bytes = ((uint64_t)cluster_count + 7u) / 8u;
    bitmap_clusters = (uint32_t)((bitmap_bytes + ((uint64_t)sectors_per_cluster * SECTOR_SIZE) - 1u) /
                                 ((uint64_t)sectors_per_cluster * SECTOR_SIZE));
    upcase_clusters = (uint32_t)((upcase_bytes + ((uint64_t)sectors_per_cluster * SECTOR_SIZE) - 1u) /
                                 ((uint64_t)sectors_per_cluster * SECTOR_SIZE));
    upcase_cluster = bitmap_cluster + bitmap_clusters;
    root_cluster = upcase_cluster + upcase_clusters;
    if ((uint64_t)root_cluster - 2u >= cluster_count) return -28;

    storage_begin_mutation();
    ret = install_clear_sectors(disk, start_lba,
                                (uint64_t)heap_offset +
                                (uint64_t)(1u + bitmap_clusters + upcase_clusters) * sectors_per_cluster);
    if (ret < 0) {
        return ret;
    }

    /* exFAT system files are contiguous extents and do not require FAT links.
     * Initialize every FAT sector nevertheless, including the two reserved
     * entries and the root-directory EOC marker.  Leaving the remainder zero
     * is intentional: the bitmap, not stale FAT entries, owns allocation. */
    for (uint32_t fat_sector = 0u; fat_sector < fat_length; ++fat_sector) {
        storage_memzero(storage_scratch, SECTOR_SIZE);
        if (fat_sector == 0u) {
            storage_put_u32(storage_scratch + 0u, 0xfffffff8u);
            storage_put_u32(storage_scratch + 4u, 0xffffffffu);
        }
        /* System files are contiguous extents. Their first FAT entry is kept
         * at EOC so readers can distinguish NoFatChain streams from a
         * truncated ordinary FAT chain. */
        for (uint32_t marker = 0u; marker < 3u; ++marker) {
            uint32_t cluster = marker == 0u ? root_cluster :
                               (marker == 1u ? bitmap_cluster : upcase_cluster);
            if (cluster / (SECTOR_SIZE / 4u) == fat_sector) {
                storage_put_u32(storage_scratch + (cluster % (SECTOR_SIZE / 4u)) * 4u,
                                EXFAT_EOC);
            }
        }
        ret = install_write_sectors(disk, start_lba + fat_offset + fat_sector, 1u,
                                    storage_scratch);
        if (ret < 0) return ret;
    }

    /* Mark root, allocation bitmap and upcase-table clusters allocated. */
    for (uint64_t bitmap_sector = 0u;
         bitmap_sector < (bitmap_bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
         ++bitmap_sector) {
        storage_memzero(storage_scratch, SECTOR_SIZE);
        uint64_t first_bit = bitmap_sector * SECTOR_SIZE * 8u;
        uint64_t last_bit = min_u64((uint64_t)cluster_count,
                                    first_bit + SECTOR_SIZE * 8u);
        for (uint64_t bit = first_bit; bit < last_bit; ++bit) {
            uint32_t cluster = (uint32_t)bit + 2u;
            if (cluster < root_cluster + 1u) {
                storage_scratch[(bit - first_bit) / 8u] |=
                    (uint8_t)(1u << ((bit - first_bit) & 7u));
            }
        }
        ret = install_write_sectors(disk, start_lba + heap_offset +
                                    (uint64_t)(bitmap_cluster - 2u) * sectors_per_cluster +
                                    bitmap_sector, 1u, storage_scratch);
        if (ret < 0) return ret;
    }

    /* Write the recommended compressed standard exFAT upcase table. */
    for (uint64_t byte_offset = 0; byte_offset < upcase_bytes; ) {
        uint32_t bytes = (uint32_t)min_u64(sizeof(storage_scratch), upcase_bytes - byte_offset);
        uint32_t sectors = (bytes + SECTOR_SIZE - 1u) / SECTOR_SIZE;
        storage_memzero(storage_scratch, sizeof(storage_scratch));
        storage_memcpy(storage_scratch, exfat_standard_upcase + byte_offset, bytes);
        for (uint32_t off = 0; off < bytes; ++off) {
            checksum = install_exfat_upcase_checksum(checksum, storage_scratch[off]);
        }
        ret = install_write_sectors(disk, start_lba + heap_offset +
                                    (uint64_t)(upcase_cluster - 2u) * sectors_per_cluster +
                                    byte_offset / SECTOR_SIZE, sectors, storage_scratch);
        if (ret < 0) return ret;
        byte_offset += bytes;
    }

    /* Root directory system entries: volume label, allocation bitmap, upcase table. */
    storage_memzero(storage_scratch, sizeof(storage_scratch));
    storage_scratch[0] = 0x83u;
    /* Keep the volume label within FAT/exFAT's 11-character limit. */
    storage_scratch[1] = 8u;
    for (uint32_t i = 0; i < 8u; ++i) exfat_put_u16(storage_scratch + 2u + i * 2u, "RELIEFOS"[i]);
    storage_scratch[32u] = EXFAT_ENTRY_BITMAP;
    storage_put_u32(storage_scratch + 32u + 20u, bitmap_cluster);
    exfat_put_u64(storage_scratch + 32u + 24u, bitmap_bytes);
    storage_scratch[64u] = EXFAT_ENTRY_UPCASE;
    storage_put_u32(storage_scratch + 64u + 4u, checksum);
    storage_put_u32(storage_scratch + 64u + 20u, upcase_cluster);
    exfat_put_u64(storage_scratch + 64u + 24u, upcase_bytes);
    ret = install_write_sectors(disk, start_lba + heap_offset +
                                (uint64_t)(root_cluster - 2u) * sectors_per_cluster,
                                STORAGE_SCRATCH_SECTORS,
                                storage_scratch);
    if (ret < 0) return ret;

    storage_memzero(boot, sizeof(boot));
    boot[0] = 0xebu; boot[1] = 0x76u; boot[2] = 0x90u;
    storage_memcpy(boot + 3u, "EXFAT   ", 8u);
    exfat_put_u64(boot + 64u, start_lba);
    exfat_put_u64(boot + 72u, sector_count);
    storage_put_u32(boot + 80u, fat_offset);
    storage_put_u32(boot + 84u, fat_length);
    storage_put_u32(boot + 88u, heap_offset);
    storage_put_u32(boot + 92u, cluster_count);
    storage_put_u32(boot + 96u, root_cluster);
    storage_put_u32(boot + 100u, 0x4c344658u);
    exfat_put_u16(boot + 104u, 0x0100u);
    boot[108u] = 9u;
    boot[109u] = spc_shift;
    boot[110u] = 1u;
    boot[111u] = 0x80u;
    boot[112u] = (uint8_t)(((uint64_t)(1u + bitmap_clusters + upcase_clusters) * 100u) / cluster_count);
    boot[510u] = 0x55u; boot[511u] = 0xaau;
    ret = install_write_sectors(disk, start_lba, 1u, boot);
    if (ret < 0) return ret;
    ret = install_write_sectors(disk, start_lba + 12u, 1u, boot);
    if (ret < 0) return ret;
    storage_memzero(storage_scratch, sizeof(storage_scratch));
    for (uint32_t sector = 1u; sector < 11u; ++sector) {
        if (sector <= 8u) {
            storage_scratch[510u] = 0x55u;
            storage_scratch[511u] = 0xaau;
        }
        ret = install_write_sectors(disk, start_lba + sector, 1u, storage_scratch);
        if (ret < 0) return ret;
        ret = install_write_sectors(disk, start_lba + 12u + sector, 1u, storage_scratch);
        if (ret < 0) return ret;
        storage_scratch[510u] = 0u;
        storage_scratch[511u] = 0u;
    }
    storage_memzero(storage_scratch, SECTOR_SIZE);
    for (uint32_t offset = 0; offset < SECTOR_SIZE; offset += 4u) {
        storage_put_u32(storage_scratch + offset, install_exfat_boot_checksum(boot));
    }
    ret = install_write_sectors(disk, start_lba + 11u, 1u, storage_scratch);
    if (ret < 0) return ret;
    ret = install_write_sectors(disk, start_lba + 23u, 1u, storage_scratch);
    if (ret < 0) return ret;
    return 0;
}
