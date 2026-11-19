/* Real ext cache and native block splitting around a sector-accurate device. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include "../../block/core/storage_state.c"
#include "../../block/core/device_number.c"
#include "../../block/core/storage_state_data.c"
#include "../../fs/ext_common/ext2_cache.c"
#include "../../fs/ext_common/cache.c"
#include "../../fs/ext_common/ops.c"
#include "../../block/device/registry.c"
#include "../../block/core/storage_block.c"

static uint8_t media[1024 * 512];
static uint8_t input[32 * 4096];
static unsigned commands, fail_command, max_sectors;

struct task *sched_current_task(void) { return NULL; }
uint32_t sched_current_pid(void) { return 0; }
uint32_t smp_cpu_count(void) { return 1; }
uint64_t time_uptime_ms(void) { return 0; }
uint64_t time_ticks(void) { return 1; }
void *kernel_malloc(size_t bytes) { return malloc(bytes); }
void kernel_free(void *pointer) { free(pointer); }
uint64_t mm_alloc_pages(uint32_t pages) { return (uintptr_t)calloc(pages, 4096); }
void *mm_phys_to_virt(uint64_t address) { return (void *)(uintptr_t)address; }
void mm_free_pages(uint64_t address, uint32_t pages) { (void)pages; free((void *)(uintptr_t)address); }
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
void exfat_cache_invalidate(void) {}
int storage_ext4_journal_capture(struct storage_volume *v, uint64_t block, const uint8_t *data)
{ (void)v; (void)block; (void)data; return 0; }
int storage_ext4_journal_publish(struct storage_volume *v, uint64_t block, const uint8_t *data)
{ (void)v; (void)block; (void)data; return 0; }
bool storage_ext4_journal_owns(const struct storage_volume *v, uint64_t block)
{ (void)v; (void)block; return false; }

static int read_media(void *context, uint64_t lba, uint32_t sectors, void *buffer)
{
    assert(context == media && sectors <= max_sectors && lba + sectors <= 1024);
    memcpy(buffer, media + lba * 512, sectors * 512);
    return 0;
}

static int write_media(void *context, uint64_t lba, uint32_t sectors, const void *buffer)
{
    assert(context == media && sectors <= max_sectors && lba + sectors <= 1024);
    ++commands;
    if (fail_command == commands) {
        memcpy(media + lba * 512, buffer, 512);
        return -RELIEFOS_EIO;
    }
    memcpy(media + lba * 512, buffer, sectors * 512);
    return 0;
}

int main(void)
{
    static const struct block_device_operations ops = {.read = read_media, .write = write_media};
    static const struct storage_filesystem_ops fs_ops = {
        .invalidate_write = storage_ext4_cache_invalidate_write};
    for (unsigned limit = 64; limit <= 256; limit *= 2) {
        struct block_device_registration device = {
            .info = {.device = 0x801, .blocks = 1024, .block_size = 512, .max_blocks = limit,
                     .flags = BLOCK_DEVICE_WRITE_THROUGH}, .operations = &ops, .context = media};
        assert(!register_block_device(&device));
        assert(!block_device_get(device.info.device, &g_storage.device_ref));
        g_storage.ext4.block_size = 4096;
        g_storage.ext4.blocks_count = 128;
        g_storage.ext_sector_count = 1024;
        g_storage.mount_generation = 1;
        g_storage.fs_ops = &fs_ops;
        max_sectors = limit;
        for (size_t i = 0; i < sizeof(input); ++i) input[i] = (i * 37 + i / 257) % 251;
        memset(media, 0xa5, sizeof(media));
        commands = fail_command = 0;
        assert(!ext4_ops_data_write(&g_storage, 3, 32, input));
        assert(commands == 256 / limit && !memcmp(media + 3 * 4096, input, sizeof(input)));
        printf("{\"max_sectors\":%u,\"payload_bytes\":%zu,\"transport_commands\":%u}\n",
               limit, sizeof(input), commands);

        struct storage_volume alias = g_storage;
        uint8_t out[4096];
        unsigned failures = 256 / limit > 1 ? 2 : 1;
        for (unsigned failure = 1; failure <= failures; ++failure) {
            /* Both writer and a second mounted alias have clean snapshots. */
            for (unsigned i = 0; i < 32; ++i) {
                assert(!storage_ext4_cache_read(&g_storage, 3 + i, out));
                assert(!storage_ext4_cache_read(&alias, 3 + i, out));
            }
            commands = 0; fail_command = failure;
            memset(input, 0x70 + failure, sizeof(input));
            assert(ext4_ops_data_write(&g_storage, 3, 32, input) == -RELIEFOS_EIO);
            assert(commands == failure);
            /* Even after an earlier transport chunk succeeded, no stale
             * clean block from the failed filesystem batch can be flushed. */
            for (unsigned i = 0; i < 32; ++i) {
                assert(!ext4_cache_lookup(&ext4_cache_tables[0], &g_storage, 3 + i));
                assert(!ext4_cache_lookup(&ext4_cache_tables[0], &alias, 3 + i));
            }
            assert(!storage_ext4_cache_flush(&g_storage));
            assert(!storage_ext4_cache_flush(&alias) && commands == failure);
            assert(!storage_ext4_cache_read(&g_storage, 3, out));
            assert(!memcmp(out, media + 3 * 4096, sizeof(out)) && out[0] == 0x70 + failure);
            assert(!storage_ext4_cache_read(&alias, 3, out));
            assert(!memcmp(out, media + 3 * 4096, sizeof(out)));
        }
        storage_ext4_cache_invalidate(&alias);
        storage_ext4_cache_invalidate(&g_storage);
        assert(!storage_ext4_cache_release());
        block_device_put(&g_storage.device_ref);
        assert(!unregister_block_device(&device));
    }
    puts("PASS real ext cache aliases, native transport limits and partial write failures");
    return 0;
}
