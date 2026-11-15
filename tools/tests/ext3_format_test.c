/* SPDX-License-Identifier: Apache-2.0 */
/* File-backed probe for the real installer ext-family formatter. */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "../../block/core/storage_internal.h"

uint8_t storage_scratch[STORAGE_SCRATCH_SECTORS * SECTOR_SIZE] __attribute__((aligned(4096)));
struct kernel_spinlock storage_transport_lock = KERNEL_SPINLOCK_INIT;
static int image_fd;
static uint64_t image_sectors, partition_start, partition_sectors;
static unsigned writes, flushes, fail_write, fail_flush, lock_depth, references;
static bool fail_get, check_order, wrong_geometry, readonly;
bool storage_io_async_context;

/** @brief Clear the formatter's scratch bytes. @param dst Writable bytes. @param n Byte count. */
void storage_memzero(void *dst, size_t n) { memset(dst, 0, n); }
/** @brief Copy formatter bytes. @param dst Writable bytes. @param src Readable bytes. @param n Byte count. */
void storage_memcpy(void *dst, const void *src, size_t n) { memcpy(dst, src, n); }
/** @brief Choose the smaller count. @param a First count. @param b Second count. @return Smaller count. */
uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }
/** @brief Record transport exclusion. @param lock Shared fixture lock. */
void kernel_spin_lock(struct kernel_spinlock *lock) { assert(lock == &storage_transport_lock); ++lock_depth; }
/** @brief Release transport exclusion. @param lock Shared fixture lock. */
void kernel_spin_unlock(struct kernel_spinlock *lock) { assert(lock == &storage_transport_lock && lock_depth); --lock_depth; }
/** @brief Admit the fixture provider. @param device Fixture identity. @param ref Output token. @return Zero or injected errno. */
int block_device_get(uint64_t device, struct block_device_ref *ref)
{ assert(device == 1); if (fail_get) return -ENODEV; ++references; ref->device = (void *)(uintptr_t)1; return 0; }
/** @brief Report fixture disk geometry. @param ref Live token. @param info Output geometry. @return Zero. */
int block_device_info(const struct block_device_ref *ref, struct block_device_info *info)
{ assert(ref->device); *info = (struct block_device_info){.blocks=image_sectors, .block_size=wrong_geometry ? 4096u : 512u, .flags=readonly ? BLOCK_DEVICE_READ_ONLY : 0u}; return 0; }
/** @brief Release a fixture provider token. @param ref Writable token. */
void block_device_put(struct block_device_ref *ref) { assert(ref->device && references); --references; ref->device = NULL; }
/** @brief Flush the file backend under transport exclusion. @param ref Live token. @return Zero or injected errno. */
int block_device_flush(const struct block_device_ref *ref)
{ assert(ref->device && lock_depth == 1 && !storage_io_async_context); ++flushes; return fail_flush == flushes ? -EIO : 0; }
/** @brief Flush the borrowed formatter volume using the core transport contract.
 * @param volume Live fixture volume. @return Zero or injected flush errno.
 */
int storage_ext4_device_flush(const struct storage_volume *volume)
{
    kernel_spin_lock(&storage_transport_lock);
    int ret = block_device_flush(&volume->device_ref);
    kernel_spin_unlock(&storage_transport_lock);
    return ret;
}

/** @brief Persist complete sectors or return an injected failure.
 * @param disk Borrowed fixture disk. @param lba First sector. @param count Sector count.
 * @param data Readable sector bytes. @return Zero or negative transport errno.
 */
int install_write_sectors(struct install_disk_state *disk, uint64_t lba, uint32_t count, const void *data)
{
    assert(disk && disk->present && data && lba <= image_sectors && count <= image_sectors - lba);
    assert(lba >= partition_start && lba - partition_start <= partition_sectors &&
           count <= partition_sectors - (lba - partition_start));
    ++writes;
    if (fail_write == writes) return -EIO;
    if (check_order && lba == partition_start) {
        const uint8_t *bytes = data;
        if (writes == 1) {
            for (size_t i = 0; i < 4096; ++i) assert(bytes[i] == 0);
        } else {
            assert(flushes >= 2 && bytes[1024 + 56] == 0x53 && bytes[1024 + 57] == 0xef);
        }
    }
    size_t total = (size_t)count * 512u, done = 0;
    while (done < total) {
        ssize_t n = pwrite(image_fd, (const uint8_t *)data + done, total - done,
                           (off_t)(lba * 512u + done));
        if (n <= 0) return -EIO;
        done += (size_t)n;
    }
    return 0;
}

/** @brief Put an old recognizable superblock in the fixture.
 * @param start First partition sector. @return Nothing; aborts on fixture I/O error.
 */
static void seed_super(uint64_t start)
{
    const uint8_t magic[2] = {0x53, 0xef};
    assert(pwrite(image_fd, magic, sizeof(magic), (off_t)(start * 512u + 1080u)) == 2);
}
/** @brief Check whether the current primary superblock carries ext magic.
 * @param start First partition sector. @return True if ext magic is present.
 */
static bool primary_valid(uint64_t start)
{
    uint8_t magic[2];
    assert(pread(image_fd, magic, sizeof(magic), (off_t)(start * 512u + 1080u)) == 2);
    return magic[0] == 0x53 && magic[1] == 0xef;
}
/** @brief Read the first backup superblock's ext magic.
 * @param start First partition sector. @return True if the backup is recognizable.
 */
static bool backup_valid(uint64_t start)
{
    uint8_t magic[2];
    uint64_t offset = (start + (uint64_t)INSTALL_EXT2_BLOCKS_PER_GROUP * 8u) * 512u + 56u;
    assert(pread(image_fd, magic, sizeof(magic), (off_t)offset) == 2);
    return magic[0] == 0x53 && magic[1] == 0xef;
}
/** @brief Reset injected failures and operation counters. */
static void reset_fixture(void)
{ assert(!references && !lock_depth); writes = flushes = fail_write = fail_flush = 0; fail_get = wrong_geometry = readonly = false; storage_io_async_context = true; }

/** @brief Test invalid geometry and observable write/flush failures.
 * @param disk Borrowed fixture disk. @param sectors Usable partition sectors.
 */
static void exercise_failures(struct install_disk_state *disk, uint64_t sectors)
{
    partition_start = 8; check_order = true;
    reset_fixture();
    assert(install_format_ext3(disk, partition_start, 16383) == -ENOSPC && !writes);
    assert(install_format_ext3(disk, UINT64_MAX - 3u, sectors) < 0 && !writes);
    assert(install_format_ext3(disk, partition_start, (UINT64_C(1) << 35)) < 0 && !writes);
    assert(install_format_ext3(disk, partition_start + 1, sectors) < 0 && !writes);
    reset_fixture(); wrong_geometry = true;
    assert(install_format_ext3(disk, partition_start, sectors) < 0 && !writes);
    reset_fixture(); readonly = true;
    assert(install_format_ext3(disk, partition_start, sectors) < 0 && !writes);
    reset_fixture(); fail_get = true;
    assert(install_format_ext3(disk, partition_start, sectors) == -ENODEV && !writes);
    reset_fixture(); seed_super(partition_start); fail_write = 1;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO && !flushes);
    reset_fixture(); seed_super(partition_start); fail_flush = 1;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO && !primary_valid(partition_start));
    reset_fixture(); seed_super(partition_start); fail_write = 3;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO && !primary_valid(partition_start));
    reset_fixture(); seed_super(partition_start);
    const uint8_t magic[2] = {0x53, 0xef};
    uint64_t backup_offset = (partition_start + (uint64_t)INSTALL_EXT2_BLOCKS_PER_GROUP * 8u) * 512u + 56u;
    assert(pwrite(image_fd, magic, sizeof(magic), (off_t)backup_offset) == 2);
    assert(backup_valid(partition_start));
    fail_flush = 2;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO &&
           !primary_valid(partition_start) && !backup_valid(partition_start));
    reset_fixture(); seed_super(partition_start); fail_flush = 3;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO && !primary_valid(partition_start));
    reset_fixture(); seed_super(partition_start); fail_flush = 4;
    assert(install_format_ext3(disk, partition_start, sectors) == -EIO && primary_valid(partition_start));
    assert(!lock_depth && !references && storage_io_async_context);
    puts("PASS formatter bounds and injected write/flush failures");
}

/** @brief Run the production formatter on a bounded sparse image.
 * @param argc Argument count. @param argv Image path, sector count and kind.
 * @return Zero after success, nonzero for invalid usage or fixture I/O.
 */
int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    uint64_t sectors = strtoull(argv[2], NULL, 10);
    partition_sectors = sectors;
    image_sectors = sectors + 8u;
    image_fd = open(argv[1], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (image_fd < 0 || ftruncate(image_fd, (off_t)(image_sectors * 512u))) return 2;
    struct install_disk_state disk = {.present = true, .device = 1, .sector_count = image_sectors};
    if (!strcmp(argv[3], "ext3-failures")) {
        exercise_failures(&disk, sectors);
        close(image_fd);
        return 0;
    }
    partition_start = 0; check_order = !strcmp(argv[3], "ext3"); reset_fixture();
    int ret = !strcmp(argv[3], "ext3") ? install_format_ext3(&disk, 0, sectors) :
              !strcmp(argv[3], "ext2") ? install_format_ext2(&disk, 0, sectors) :
              !strcmp(argv[3], "ext4") ? install_format_ext4(&disk, 0, sectors) : -EINVAL;
    if (ret) { fprintf(stderr, "formatter returned %d\n", ret); return 1; }
    if (check_order) assert(flushes == 4 && primary_valid(0) && !lock_depth &&
                            !references && storage_io_async_context);
    assert(ftruncate(image_fd, (off_t)(sectors * 512u)) == 0);
    close(image_fd);
    printf("PASS %s native formatter: %llu sectors, %u writes, %u flushes\n",
           argv[3], (unsigned long long)sectors, writes, flushes);
    return 0;
}
