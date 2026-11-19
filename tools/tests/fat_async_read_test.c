#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../../fs/fat/native.c"

#define TEST_SECTORS 65536u
#define TEST_CLUSTERS 2050u
#define TEST_DATA_SECTOR 256u

static struct storage_volume volumes[2];
struct storage_volume *g_active_volume = &volumes[0];
struct storage_sector_cache storage_fat_cache, storage_read_cache;
struct storage_cluster_cache storage_dir_lookup_cache;
struct storage_dir_iter_cache storage_dir_iter_cache;
uint8_t storage_fat_cache_data[STORAGE_FAT_CACHE_SECTORS * SECTOR_SIZE] __attribute__((aligned(4096)));
uint8_t storage_read_cache_data[STORAGE_READAHEAD_SECTORS * SECTOR_SIZE] __attribute__((aligned(4096)));
uint8_t storage_cluster_buf[64 * SECTOR_SIZE] __attribute__((aligned(4096)));
uint8_t storage_dir_lookup_cache_data[64 * SECTOR_SIZE] __attribute__((aligned(4096)));
static unsigned char *images[2];
static uint64_t epoch = 1, commands, discarded, completed;
static uint64_t pending_sector, fail_sector = UINT64_MAX;
static uint32_t pending_count, pending_volume;
static int asynchronous, pending, fragmented;

/** @brief Select disjoint fixture chain heads without using the native reader. */
static uint32_t first_cluster(unsigned file)
{ return fragmented ? 2 + file : 2 + file * (TEST_CLUSTERS + 16); }

void storage_memcpy(void *destination, const void *source, size_t size)
{ memcpy(destination, source, size); }
void storage_memzero(void *buffer, size_t size) { memset(buffer, 0, size); }
void storage_copy_text(char *destination, uint32_t capacity, const char *source)
{ snprintf(destination, capacity, "%s", source); }
int storage_text_eq_ci(const char *first, const char *second) { return !strcasecmp(first, second); }
int storage_dir_index_lookup(uint32_t directory, const char *name, struct storage_node *out)
{ (void)directory; (void)name; (void)out; return 0; }
void storage_dir_index_store(uint32_t directory, const char *name, const struct storage_node *node)
{ (void)directory; (void)name; (void)node; }
int storage_inode_cache_node(struct storage_node *node) { (void)node; return 0; }
int storage_is_acl_metadata_name(const char *name) { return !strcasecmp(name, "LEONACL.SYS"); }
uint64_t min_u64(uint64_t first, uint64_t second)
{ return first < second ? first : second; }
uint64_t fat_sector_for_cluster(uint32_t cluster)
{ return g_storage.fat_start_sector + cluster * 4u / SECTOR_SIZE; }
uint64_t cluster_to_lba(uint32_t cluster)
{ return g_storage.data_start_sector + (uint64_t)(cluster - 2) * g_storage.sectors_per_cluster; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
uint64_t storage_metadata_generation(void) { return epoch; }
int storage_read_failure(int error)
{ return error == -RELIEFOS_EAGAIN ? error : -RELIEFOS_EIO; }

/** @brief Model one delayed, owned read and AHCI's discard of a different retry.
 * @param sector First bounded sector. @param count Number of sectors.
 * @param buffer Current caller output. @return Zero, EAGAIN or injected EIO.
 */
int storage_read_sectors(uint64_t sector, uint32_t count, void *buffer)
{
    assert(sector + count <= TEST_SECTORS);
    if (sector <= fail_sector && fail_sector - sector < count) return -RELIEFOS_EIO;
    if (asynchronous) {
        if (pending && pending_sector == sector && pending_count == count && pending_volume == g_storage.volume_id) {
            pending = 0;
            ++completed;
        } else {
            if (pending) ++discarded;
            pending = 1;
            pending_sector = sector;
            pending_count = count;
            pending_volume = g_storage.volume_id;
            ++commands;
            return -RELIEFOS_EAGAIN;
        }
    }
    memcpy(buffer, images[g_storage.volume_id] + sector * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

/** @brief Invalidate media caches after a fixture-owned raw metadata mutation. */
static void invalidate(void)
{
    ++epoch;
    pending = 0;
    storage_fat_cache.valid = storage_read_cache.valid = 0;
    storage_dir_lookup_cache.valid = storage_dir_iter_cache.valid = 0;
}

/** @brief Generate independent fragmented FAT chains and known file bytes.
 * @param sectors_per_cluster One of the supported 512-byte-sector geometries.
 */
static void prepare(uint32_t sectors_per_cluster)
{
    for (unsigned volume = 0; volume < 2; ++volume) {
        memset(images[volume], 0, TEST_SECTORS * SECTOR_SIZE);
        volumes[volume] = (struct storage_volume){.volume_id = volume, .mount_generation = 1,
            .bytes_per_sector = SECTOR_SIZE, .sectors_per_cluster = sectors_per_cluster,
            .cluster_bytes = sectors_per_cluster * SECTOR_SIZE, .fat_start_sector = 32,
            .fat_sector_count = 128, .data_start_sector = TEST_DATA_SECTOR,
            .data_cluster_count = (TEST_SECTORS - TEST_DATA_SECTOR) / sectors_per_cluster};
        for (unsigned file = 0; file < 2; ++file) {
            for (uint32_t logical = 0; logical < TEST_CLUSTERS; ++logical) {
                uint32_t stride = fragmented ? 3 : 1;
                uint32_t cluster = first_cluster(file) + logical * stride;
                uint32_t next = logical + 1 == TEST_CLUSTERS ? FAT32_EOC : cluster + stride;
                memcpy(images[volume] + 32 * SECTOR_SIZE + cluster * 4, &next, sizeof(next));
                unsigned char *data = images[volume] + (TEST_DATA_SECTOR + (cluster - 2) * sectors_per_cluster) * SECTOR_SIZE;
                for (uint32_t index = 0; index < sectors_per_cluster * SECTOR_SIZE; ++index)
                    data[index] = (unsigned char)(volume * 71 + file * 29 + logical * 17 + index * 3);
            }
        }
    }
    invalidate();
}

/** @brief Read with the production partial-byte contract and verify every byte.
 * @param volume Native volume. @param file Independent fragmented fixture file.
 * @param offset Requested byte. @param count Exact bounded request.
 * @param cursor Optional shared or descriptor-owned hint.
 */
static void verify(unsigned volume, unsigned file, uint64_t offset, uint32_t count,
                   struct storage_read_cursor *cursor)
{
    g_active_volume = &volumes[volume];
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
        .volume_id = volume, .mount_generation = volumes[volume].mount_generation,
        .inode_generation = 10 + file, .first_cluster = first_cluster(file),
        .size = (uint64_t)TEST_CLUSTERS * g_storage.cluster_bytes};
    unsigned char *buffer = malloc(count);
    assert(buffer);
    uint32_t received = 0;
    unsigned attempts = 0;
    uint64_t before_discards = discarded;
    while (received < count) {
        uint32_t bytes = UINT32_MAX;
        int result = fat_native_read(g_active_volume, &node, offset + received,
            buffer + received, count - received, &bytes, cursor, 0);
        assert(bytes <= count - received);
        received += bytes;
        assert(result == 0 || result == -RELIEFOS_EAGAIN);
        assert(++attempts < 20000);
        assert(bytes || result == -RELIEFOS_EAGAIN);
    }
    assert(discarded == before_discards && !pending);
    for (uint32_t index = 0; index < count; ++index) {
        uint64_t position = offset + index;
        unsigned logical = position / g_storage.cluster_bytes;
        unsigned within = position % g_storage.cluster_bytes;
        assert(buffer[index] == (unsigned char)(volume * 71 + file * 29 + logical * 17 + within * 3));
    }
    free(buffer);
}

/** @brief Read a GRUB-sized directory with LFNs spanning delayed cluster reads. */
static void directory_tests(void)
{
    prepare(2);
    g_active_volume = &volumes[0];
    unsigned char *directory = images[0] + (TEST_DATA_SECTOR + (6500 - 2) * 2) * SECTOR_SIZE;
    unsigned records = 2;
    directory[0] = directory[32] = 0xe5;
    for (unsigned index = 0; index < 600; ++index) {
        char name[80];
        snprintf(name, sizeof(name), "module-file-number-%04u.mod", index);
        uint16_t text[80];
        unsigned length = (unsigned)strlen(name);
        for (unsigned character = 0; character < length; ++character) text[character] = (uint8_t)name[character];
        unsigned parts = (length + 12) / 13;
        for (unsigned part = parts; part; --part) {
            struct fat32_lfn *lfn = (void *)(directory + records++ * 32);
            fat32_fill_lfn_entry(lfn, text, length, part - 1, parts, 0);
        }
        struct fat32_dirent *entry = (void *)(directory + records++ * 32);
        memset(entry, 0, sizeof(*entry));
        memcpy(entry->name, "MOD00000MOD", 11);
        entry->first_cluster_lo = 2;
        entry->size = index;
    }
    unsigned clusters = (records * 32 + 1 + 1023) / 1024;
    for (unsigned index = 0; index < clusters; ++index) {
        uint32_t next = index + 1 == clusters ? FAT32_EOC : 6501 + index;
        memcpy(images[0] + 32 * SECTOR_SIZE + (6500 + index) * 4, &next, sizeof(next));
    }
    invalidate();
    asynchronous = 1;
    struct storage_node node;
    int result = -RELIEFOS_EAGAIN;
    unsigned attempts = 0;
    while (result == -RELIEFOS_EAGAIN) {
        result = fat32_find_in_dir(6500, "module-file-number-0599.mod", &node);
        assert(++attempts < 10000);
    }
    assert(!result && node.size == 599 && !pending);
    attempts = 0; result = -RELIEFOS_EAGAIN;
    while (result == -RELIEFOS_EAGAIN) {
        result = fat32_find_in_dir(6500, "absent.mod", &node);
        assert(++attempts < 10000);
    }
    assert(result == -RELIEFOS_ENOENT && !pending);
    for (unsigned index = 0; index <= 600; ++index) {
        struct reliefos_dir_entry entry;
        char expected[80];
        snprintf(expected, sizeof(expected), "module-file-number-%04u.mod", index);
        result = -RELIEFOS_EAGAIN; attempts = 0;
        while (result == -RELIEFOS_EAGAIN) {
            result = fat32_iter_dir_entry(6500, index, &entry);
            assert(++attempts < 10000);
        }
        assert(!pending);
        if (index < 600) assert(!result && !strcmp(entry.name, expected));
        else assert(result == -RELIEFOS_ENOENT);
    }
    puts("PASS FAT delayed directory lookup/absence/enumeration: 600 long module names crossing clusters");
}

/** @brief Exercise delayed readahead, cross-window seeks and stale hint rejection. */
int main(void)
{
    for (unsigned volume = 0; volume < 2; ++volume) {
        images[volume] = malloc(TEST_SECTORS * SECTOR_SIZE);
        assert(images[volume]);
    }
    for (fragmented = 0; fragmented < 2; ++fragmented) {
      for (unsigned sectors = 1; sectors <= 8; sectors *= 2) {
        prepare(sectors);
        for (asynchronous = 0; asynchronous < 2; ++asynchronous) {
            struct storage_read_cursor cursor = {0};
            invalidate();
            verify(0, 0, 0, 32768, &cursor);
            verify(0, 0, 32768, 32768, &cursor);
            invalidate();
            verify(0, 0, (uint64_t)1020 * sectors * SECTOR_SIZE, 16384, &cursor);
            invalidate();
            verify(0, 0, (uint64_t)1600 * sectors * SECTOR_SIZE + 19, 32768, &cursor);
            verify(0, 0, (uint64_t)1500 * sectors * SECTOR_SIZE + 23, 4096, NULL);
            for (unsigned file = 0; file < 2; ++file) {
                for (unsigned volume = 0; volume < 2; ++volume) {
                    uint64_t offset = cursor.offset;
                    verify(volume, file, offset, 4096, &cursor);
                }
            }
            ++volumes[1].mount_generation;
            verify(1, 1, cursor.offset, 4096, &cursor);
            invalidate();
            verify(1, 1, cursor.offset, 4096, &cursor);
            verify(0, 0, 0, 1, &cursor);
            verify(0, 0, 1, sectors * SECTOR_SIZE + 37, &cursor);
            verify(0, 0, (uint64_t)TEST_CLUSTERS * sectors * SECTOR_SIZE - 4096, 4096, &cursor);
        }
        printf("PASS FAT async reads: cluster=%u fragmented=%d, seek, volume/inode/remount/epoch identity, partial reads and EOF\n", sectors * SECTOR_SIZE, fragmented);
      }
    }
    fragmented = 1;
    prepare(2);
    asynchronous = 0;
    struct storage_node node = {.type = RELIEFOS_FS_TYPE_FILE, .flags = STORAGE_NODE_FLAG_FAT32,
        .first_cluster = 2, .inode_generation = 10, .size = TEST_CLUSTERS * 1024};
    struct storage_read_cursor cursor = {0};
    unsigned char buffer[8192];
    uint32_t bytes = 0;
    g_active_volume = &volumes[0];
    fail_sector = TEST_DATA_SECTOR + 6;
    assert(fat_native_read(g_active_volume, &node, 0, buffer, sizeof(buffer), &bytes, &cursor, 0) == -RELIEFOS_EIO);
    assert(bytes == 1024 && !cursor.valid);
    printf("PASS FAT permanent I/O error retains the completed prefix; commands=%llu completed=%llu discarded=%llu\n",
        (unsigned long long)commands, (unsigned long long)completed, (unsigned long long)discarded);
    fail_sector = UINT64_MAX;
    directory_tests();
    for (unsigned volume = 0; volume < 2; ++volume) free(images[volume]);
    return 0;
}
