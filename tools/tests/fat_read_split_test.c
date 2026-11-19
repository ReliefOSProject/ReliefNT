/* SPDX-License-Identifier: Apache-2.0 */
/* Real FAT directory/cache readers, registry and block splitting share a
 * provider with NVMe-style retained read identity. No FAT/block I/O is faked. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../fs/fat/internal.h"

static uint8_t media[1024 * 512], output[64 * 512];
static unsigned calls, completed, discarded, max_sectors, fail_at;
static unsigned synchronous_calls, asynchronous_calls;
static int injected_error;
static uint64_t deferred_start;
static struct { bool active; uint64_t lba; uint32_t count; void *buffer; } pending;

struct task *sched_current_task(void) { return NULL; }
uint32_t sched_current_pid(void) { return 7; }
uint32_t smp_cpu_count(void) { return 1; }
uint64_t time_uptime_ms(void) { return 0; }
uint64_t time_ticks(void) { return 1; }
void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *pointer) { free(pointer); }
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ *flags = 0; kernel_spin_lock(lock); }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; kernel_spin_unlock(lock); }
void console_printf(const char *format, ...) { (void)format; }
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }
uint8_t x86_64_inb(uint16_t port) { (void)port; abort(); }
uint16_t x86_64_inw(uint16_t port) { (void)port; abort(); }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; abort(); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; abort(); }

/** @brief Model one native read that only completes on an identical retry.
 * First chunks can finish immediately; delayed later chunks expose replay
 * from the beginning rather than allowing a fake all-synchronous provider.
 */
static int read_media(void *context, uint64_t lba, uint32_t sectors, void *buffer)
{
    assert(context == media && sectors && sectors <= max_sectors && lba + sectors <= 1024);
    assert(storage_task_io_owner == 7);
    ++calls;
    if (storage_async_can_yield()) ++asynchronous_calls;
    else ++synchronous_calls;
    if (calls == fail_at) { pending.active = false; return injected_error; }
    if (pending.active) {
        bool matches = pending.lba == lba && pending.count == sectors && pending.buffer == buffer;
        pending.active = false;
        if (!matches) ++discarded;
        else goto complete;
    }
    if (storage_async_can_yield() && lba >= deferred_start) {
        pending.active = true; pending.lba = lba;
        pending.count = sectors; pending.buffer = buffer;
        return -RELIEFOS_EAGAIN;
    }
complete:
    ++completed;
    memcpy(buffer, media + lba * 512, sectors * 512);
    return 0;
}

enum operation { LOOKUP, READDIR, CLUSTER, READ_CACHE, FAT_CACHE, OPERATIONS };
static const char *names[] = {"lookup", "readdir", "cluster", "read-cache", "fat-cache"};
static uint32_t target_offset;
static struct fat32_dir_ref found;
static struct reliefos_dir_entry entry;
static uint32_t next_cluster;

/** @brief Run the production FAT call with no progress state owned by the test. */
static int read_fat(enum operation operation)
{
    switch (operation) {
    case LOOKUP: return fat32_find_dirent_ref_in_dir(2, "resume.txt", &found);
    case READDIR: return fat32_iter_dir_entry(2, 0, &entry);
    case CLUSTER: return fat32_read_cluster(2, output);
    case READ_CACHE: return storage_read_cache_fill(2, 1);
    case FAT_CACHE: return fat32_read_fat_entry(2, &next_cluster);
    default: abort();
    }
}

/** @brief Set up an independently encoded short entry in the second half.
 * Deleted entries before it force the directory decoder to consume that
 * half; an all-zero first half would terminate before reaching the entry.
 */
static void encode_directory(unsigned cluster_sectors)
{
    memset(media, 0, sizeof(media));
    target_offset = cluster_sectors * 512 / 2;
    for (uint32_t off = 0; off < target_offset; off += 32)
        media[64 * 512 + off] = 0xe5;
    uint8_t *record = media + 64 * 512 + target_offset;
    memcpy(record, "RESUME  TXT", 11);
    record[11] = 0x20;
    record[26] = 3;
    record[28] = 0x39; record[29] = 0x30; /* 12345 bytes, little endian. */
    media[512 + 8] = 0xff; media[512 + 9] = 0xff;
    media[512 + 10] = 0xff; media[512 + 11] = 0x0f;
}

/** @brief Start a fresh filesystem read with exact caller context flags. */
static void begin_read(bool asynchronous, bool write_started,
                       uint64_t first_lba, uint32_t sectors)
{
    storage_fat_cache.valid = storage_read_cache.valid = storage_dir_lookup_cache.valid = 0;
    storage_dir_iter_cache.valid = 0;
    memset(output, 0xa5, sizeof(output));
    memset(&found, 0, sizeof(found)); memset(&entry, 0, sizeof(entry));
    calls = completed = discarded = synchronous_calls = asynchronous_calls = 0;
    memset(&pending, 0, sizeof(pending)); fail_at = 0;
    /* Delay a second native chunk when split, otherwise delay the only one. */
    deferred_start = first_lba + (sectors > max_sectors ? max_sectors : 0);
    storage_set_io_async_context(true); assert(!storage_acquire_task_io());
    storage_io_async_context = asynchronous; storage_io_write_started = write_started;
}

/** @brief Verify output independently of the FAT name/metadata encoders. */
static void check_output(enum operation operation)
{
    switch (operation) {
    case LOOKUP:
        assert(found.entry_cluster == 2 && found.entry_offset == target_offset);
        assert(found.dirent.first_cluster_lo == 3 && found.dirent.size == 12345);
        break;
    case READDIR:
        assert(!strcmp(entry.name, "resume.txt") && entry.type == RELIEFOS_FS_TYPE_FILE);
        break;
    case CLUSTER: assert(!memcmp(output, media + 64 * 512, g_storage.cluster_bytes)); break;
    case READ_CACHE:
        assert(storage_read_cache.valid && storage_read_cache.sector_count == g_storage.sectors_per_cluster);
        assert(!memcmp(storage_read_cache_data, media + 64 * 512, g_storage.cluster_bytes));
        break;
    case FAT_CACHE: assert(next_cluster == 0x0fffffff); break;
    default: abort();
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    bool baseline = argc > 1 && !strcmp(argv[1], "--expect-stuck");
    static const unsigned limits[] = {1, 4, 8, 16, 32, 64};
    static const unsigned clusters[] = {1, 8, 16, 32, 64};
    static const struct block_device_operations ops = {.read = read_media};
    for (unsigned li = 0; li < sizeof(limits) / sizeof(limits[0]); ++li) {
        max_sectors = limits[li];
        if (baseline && max_sectors != 16) continue;
        struct block_device_registration device = {
            .info = {.device = 0x801, .blocks = 1024, .block_size = 512,
                .max_blocks = max_sectors, .flags = BLOCK_DEVICE_READ_ONLY},
            .operations = &ops, .context = media};
        assert(!register_block_device(&device));
        assert(!block_device_get(device.info.device, &g_storage.device_ref));
        g_storage.volume_id = 1; g_storage.mount_generation = 1;
        g_storage.filesystem = STORAGE_FILESYSTEM_FAT32;
        g_storage.bytes_per_sector = 512; g_storage.data_start_sector = 64;
        g_storage.fat_start_sector = 1; g_storage.fat_sector_count = 8;
        for (unsigned ci = 0; ci < sizeof(clusters) / sizeof(clusters[0]); ++ci) {
            unsigned cluster_sectors = clusters[ci];
            if (baseline && cluster_sectors != 32) continue;
            g_storage.sectors_per_cluster = cluster_sectors;
            g_storage.cluster_bytes = cluster_sectors * 512;
            encode_directory(cluster_sectors);
            for (enum operation operation = LOOKUP; operation < OPERATIONS; ++operation) {
                if (baseline && operation != LOOKUP && operation != READDIR) continue;
                /* This cache API intentionally rejects a cluster larger than
                 * its fixed workspace; direct directory/cluster reads above
                 * exercise all supported larger cluster geometries. */
                if (operation == READ_CACHE && cluster_sectors > STORAGE_READAHEAD_SECTORS) continue;
                uint32_t sectors = operation == FAT_CACHE ? 8 : cluster_sectors;
                uint64_t start = operation == FAT_CACHE ? 1 : 64;
                for (unsigned flags = 0; flags < (baseline ? 1u : 4u); ++flags) {
                    bool asynchronous = !(flags & 1), write_started = (flags & 2) != 0;
                    begin_read(asynchronous, write_started, start, sectors);
                    int ret = -RELIEFOS_EAGAIN; unsigned attempt = 0;
                    while (ret == -RELIEFOS_EAGAIN && attempt < 64) {
                        ++attempt; ret = read_fat(operation);
                    }
                    if (baseline) {
                        assert(ret == -RELIEFOS_EAGAIN && discarded && pending.active && attempt == 64);
                        printf("BASELINE %s max_sectors=%u cluster_sectors=%u retries=%u discarded=%u\n",
                               names[operation], max_sectors, cluster_sectors, attempt, discarded);
                    } else {
                        if (ret || pending.active || discarded)
                            fprintf(stderr, "FAILED %s limit=%u cluster=%u flags=%u ret=%d pending=%d discarded=%u attempts=%u\n",
                                    names[operation], max_sectors, cluster_sectors, flags,
                                    ret, pending.active, discarded, attempt);
                        assert(!ret && !pending.active && !discarded);
                        check_output(operation);
                        bool native_async = asynchronous && !write_started && sectors <= max_sectors;
                        assert(attempt == (native_async ? 2u : 1u));
                        if (native_async) assert(asynchronous_calls == 2 && !synchronous_calls);
                        else assert(!asynchronous_calls && synchronous_calls == (sectors + max_sectors - 1) / max_sectors);
                        assert(storage_io_async_context == asynchronous && storage_io_write_started == write_started);
                        assert(storage_task_io_owner == 7);
                        printf("PASS %s max_sectors=%u cluster_sectors=%u flags=%u attempts=%u\n",
                               names[operation], max_sectors, cluster_sectors, flags, attempt);
                    }
                    pending.active = false; storage_release_task_io(7);
                }
                if (baseline) continue;
                for (unsigned error = 0; error < 2; ++error) for (unsigned flags = 0; flags < 4; ++flags) {
                    bool asynchronous = !(flags & 1), write_started = (flags & 2) != 0;
                    begin_read(asynchronous, write_started, start, sectors);
                    fail_at = sectors > max_sectors ? 2 : 1;
                    injected_error = error ? -RELIEFOS_ENOMEM : -RELIEFOS_EIO;
                    /* FAT metadata readers intentionally normalize permanent
                     * device errors to EIO; the raw cluster reader preserves it. */
                    int expected_error = operation == CLUSTER ? injected_error : -RELIEFOS_EIO;
                    assert(read_fat(operation) == expected_error);
                    assert(calls == fail_at && !pending.active);
                    assert(storage_io_async_context == asynchronous && storage_io_write_started == write_started);
                    assert(storage_task_io_owner == 7);
                    assert(!storage_fat_cache.valid && !storage_read_cache.valid && !storage_dir_lookup_cache.valid);
                    assert(!storage_dir_iter_cache.valid);
                    fail_at = 0;
                    int ret = -RELIEFOS_EAGAIN; unsigned attempts = 0;
                    while (ret == -RELIEFOS_EAGAIN && attempts++ < 4) ret = read_fat(operation);
                    assert(!ret && !pending.active && !discarded); check_output(operation);
                    storage_release_task_io(7);
                }
            }
        }
        block_device_put(&g_storage.device_ref); assert(!unregister_block_device(&device));
    }
    puts(baseline ? "PASS reproduced original FAT native-split retry livelock" :
         "PASS FAT directory/cache reads across native request caps and context/error exits");
    return 0;
}
