#define main storage_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

int main(int argc, char **argv)
{
    assert(argc == 2);
    load_volume(argv[1], 0);
    load_volume(argv[1], 1);
    struct block_device_info media;
    assert(g_volumes[0].device_ref.device && g_volumes[1].device_ref.device);
    assert(!block_device_info(&g_volumes[0].device_ref,&media) && media.transport==BLOCK_TRANSPORT_MEMORY);
    assert(g_volumes[0].device_ref.device!=g_volumes[1].device_ref.device);
    uint8_t input[4096], output[4096];
    uint32_t size = g_storage.ext2_block_size, sectors = size / SECTOR_SIZE;
    /* Use a free block only as isolated cache data; no allocation is published. */
    uint32_t block = g_storage.ext2_blocks_count - 1;
    uint64_t lba = (uint64_t)block * sectors;
    assert(!ext2_cache_release());
    test_fail_cache_alloc = 1;
    memset(input, 'F', size);
    assert(ext2_write_block(block, input) == 0);
    assert(!ext2_cache && !ext2_cache_read(lba, sectors, output));
    assert(ext2_read_block(block, output) == 0 && !memcmp(input, output, size));
    assert(!ext2_cache);
    test_fail_cache_alloc = 0;
    memset(input, 'A', size);
    assert(ext2_write_block(block, input) == 0);
    assert(ext2_cache_read(lba, sectors, output) && !memcmp(input, output, size));
    g_active_volume = &g_volumes[0];
    memset(input, 'B', size);
    assert(ext2_write_block(block, input) == 0);
    assert(ext2_read_block(block, output) == 0 && !memcmp(input, output, size));
    g_active_volume = &g_volumes[1];
    assert(ext2_read_block(block, output) == 0 && output[0] == 'A');

    /* A raw write through an alias must evict the mounted volume's cache. */
    struct storage_volume alias = g_storage;
    memset(input, 'C', size);
    assert(storage_write_device(&alias, lba + 1, 1, input) == 0);
    assert(!ext2_cache_read(lba, sectors, output));
    assert(ext2_read_block(block, output) == 0);
    assert(output[0] == 'A' && output[512] == 'C');

    /* Failed writes cannot publish the requested bytes as clean data. */
    uint64_t previous_failure = test_memory_disk(&g_storage)->failure_lba;
    test_memory_disk(&g_storage)->failure_lba = lba;
    memset(input, 'D', size);
    assert(ext2_write_block(block, input) < 0);
    assert(!ext2_cache_read(lba, sectors, output));
    test_memory_disk(&g_storage)->failure_lba = previous_failure;
    assert(ext2_read_block(block, output) == 0 && output[0] == 'A');

    /* Reusing a volume object at remount must not reuse its old blocks. */
    memset(test_memory_bytes(&g_storage) + lba * SECTOR_SIZE, 'E', size);
    assert(ext2_mount() == 0);
    storage_cache_invalidate();
    assert(!ext2_cache_read(lba, sectors, output));
    assert(ext2_read_block(block, output) == 0 && output[0] == 'E');
    assert(ext2_cache_read(lba, sectors, output));
    for (unsigned i = 0; i < EXT2_CACHE_ENTRIES + 1; ++i)
        ext2_cache_store(i * sectors, sectors, input);
    assert(!ext2_cache_read(lba, sectors, output));
    assert(ext2_read_block(block, output) == 0 && output[0] == 'E');

    uint8_t bitmap[4] = {0xff, 0xff, 0xff, 0xff};
    assert(ext2_free_bitmap_bit(bitmap, 29, 23) == 29);
    bitmap[0] &= ~(1u << 2);
    assert(ext2_free_bitmap_bit(bitmap, 29, 23) == 2);
    bitmap[3] &= ~(1u << 1);
    assert(ext2_free_bitmap_bit(bitmap, 29, 23) == 25);
    assert(ext2_free_bitmap_bit(bitmap, 29, UINT32_MAX) == 2);
    for (unsigned i = 0; i < 2; ++i) test_memory_release(&g_volumes[i]);
    puts("PASS ext2 cache: allocation failure, volume isolation, raw alias writes, failed writes, remount, eviction and bitmap wrap");
}
