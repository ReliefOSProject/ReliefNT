/* Exercise the real registry + storage_transfer_blocks split with a provider
 * that retains one deferred read identity, as the native NVMe driver does. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
/* Storage completion wakeups are outside this host fixture. */
void storage_completion_notify(void) {}
#include "../../block/core/storage_state.c"
#include "../../block/core/device_number.c"
#include "../../block/core/storage_state_data.c"
#include "../../fs/ext_common/ext2_cache.c"
#include "../../fs/ext_common/cache.c"
#include "../../block/device/registry.c"
#include "../../block/core/storage_block.c"

static uint8_t media[1024 * 512], output[32 * 4096];
static unsigned calls, completed, discarded, max_sectors, allocation_fail;
static unsigned synchronous_calls, asynchronous_calls, fail_at;
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
uint64_t mm_alloc_pages(uint32_t pages)
{ return allocation_fail ? 0 : (uintptr_t)calloc(pages, 4096); }
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

int main(int argc, char **argv)
{
    bool baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    static const struct block_device_operations ops = {.read = read_media};
    for (unsigned limit = 4; limit <= 256; limit *= 2) {
        struct block_device_registration device = {
            .info = {.device = 0x801, .blocks = 1024, .block_size = 512,
                .max_blocks = limit, .flags = BLOCK_DEVICE_READ_ONLY},
            .operations = &ops, .context = media};
        assert(!register_block_device(&device));
        assert(!block_device_get(device.info.device, &g_storage.device_ref));
        g_storage.ext4.block_size = 4096; g_storage.ext4.blocks_count = 128;
        g_storage.ext_sector_count = 1024; g_storage.mount_generation = 1;
        max_sectors = limit;
        for (size_t i = 0; i < sizeof(media); ++i) media[i] = (i * 37 + i / 257) % 251;
        for (unsigned pressure = 0; pressure < (baseline ? 1u : 7u); ++pressure) {
            allocation_fail = pressure == 1;
            if (pressure == 2) {
                assert(ext4_cache_ensure(&ext4_cache_tables[0]));
                for (unsigned i = 0; i < ext4_cache_tables[0].capacity; ++i) {
                    struct storage_ext4_cache_entry *e = &ext4_cache_tables[0].entries[i];
                    *e = (struct storage_ext4_cache_entry){.valid = 1, .dirty = 1,
                        .pinned = 1, .volume = &g_storage, .volume_generation = 1,
                        .block = 64 + i};
                }
            }
            if (pressure >= 3) {
                assert(ext4_cache_ensure(&ext4_cache_tables[0]));
                unsigned block = pressure == 5 ? 3 : pressure == 6 ? 34 : 10;
                struct storage_ext4_cache_entry *e = &ext4_cache_tables[0].entries[0];
                *e = (struct storage_ext4_cache_entry){.valid = 1,
                    .volume = &g_storage, .volume_generation = 1, .block = block,
                    .dirty = pressure == 4, .pinned = pressure == 4};
                memcpy(e->data, media + block * 4096, 4096);
                if (pressure == 4) memset(e->data, 0x7e, 4096);
            }
            memset(output, 0xa5, sizeof(output));
            calls = completed = discarded = 0; memset(&pending, 0, sizeof(pending));
            synchronous_calls = asynchronous_calls = fail_at = 0;
            /* First native chunk succeeds synchronously. The second and every
             * later chunk require an identical later callback to complete. */
            deferred_start = 3 * 8 + (limit < 256 ? limit : 0);
            storage_set_io_async_context(true); assert(!storage_acquire_task_io());
            int ret = -RELIEFOS_EAGAIN; unsigned attempt = 0;
            while (ret == -RELIEFOS_EAGAIN && attempt++ < 24)
                ret = storage_ext4_cache_read_blocks(&g_storage, 3, 32, output);
            if (baseline && limit < 256) {
                assert(ret == -RELIEFOS_EAGAIN && discarded && pending.active);
                printf("{\"max_sectors\":%u,\"baseline_stuck_attempts\":%u,\"discarded_reads\":%u}\n",
                       limit, attempt - 1, discarded);
            } else {
                assert(!ret && !pending.active && !discarded);
                for (unsigned i = 0; i < sizeof(output); ++i) {
                    uint8_t expected = pressure == 4 && i >= 7 * 4096 && i < 8 * 4096 ?
                        0x7e : media[3 * 4096 + i];
                    assert(output[i] == expected);
                }
                assert(storage_async_can_yield() && !storage_io_write_started && storage_task_io_owner == 7);
                if (limit < 256 || pressure >= 3)
                    assert(synchronous_calls && !asynchronous_calls && attempt == 1);
                else assert(!synchronous_calls && asynchronous_calls == 2 && attempt == 2);
                printf("{\"max_sectors\":%u,\"cache_pressure\":%u,\"attempts\":%u,\"transport_calls\":%u}\n",
                       limit, pressure, attempt, calls);
            }
            pending.active = false; storage_release_task_io(7);
            storage_ext4_cache_invalidate(&g_storage); assert(!storage_ext4_cache_release());
        }
        if (!baseline) for (unsigned mode = 0; mode < 2; ++mode) {
            /* Two miss runs around dirty cache bytes require synchronous
             * completion even when total bytes fit one native command. Both
             * errno exits restore the caller's exact two context flags. */
            allocation_fail = 0; assert(ext4_cache_ensure(&ext4_cache_tables[0]));
            struct storage_ext4_cache_entry *e = &ext4_cache_tables[0].entries[0];
            *e = (struct storage_ext4_cache_entry){.valid = 1, .dirty = 1, .pinned = 1,
                .volume = &g_storage, .volume_generation = 1, .block = 10};
            memcpy(e->data, media + 10 * 4096, 4096);
            calls = completed = discarded = synchronous_calls = asynchronous_calls = 0;
            memset(&pending, 0, sizeof(pending));
            fail_at = 2; injected_error = mode ? -RELIEFOS_ENOMEM : -RELIEFOS_EIO;
            storage_set_io_async_context(true); assert(!storage_acquire_task_io());
            assert(storage_ext4_cache_read_blocks(&g_storage, 3, 32, output) == injected_error);
            assert(calls == 2 && synchronous_calls == 2 && !asynchronous_calls && !pending.active);
            assert(storage_io_async_context && !storage_io_write_started && storage_task_io_owner == 7);
            /* Already synchronous committed context remains exactly so. */
            storage_ext4_cache_invalidate(&g_storage);
            *e = (struct storage_ext4_cache_entry){.valid = 1, .dirty = 1, .pinned = 1,
                .volume = &g_storage, .volume_generation = 1, .block = 10};
            memcpy(e->data, media + 10 * 4096, 4096);
            storage_io_async_context = false; storage_io_write_started = true;
            calls = synchronous_calls = 0;
            assert(storage_ext4_cache_read_blocks(&g_storage, 3, 32, output) == injected_error);
            assert(!storage_io_async_context && storage_io_write_started && storage_task_io_owner == 7);
            storage_release_task_io(7);
            storage_ext4_cache_invalidate(&g_storage); assert(!storage_ext4_cache_release());
        }
        if (!baseline) {
            allocation_fail = 0; assert(ext4_cache_ensure(&ext4_cache_tables[0]));
            for (unsigned i = 0; i < 32; ++i) {
                struct storage_ext4_cache_entry *e = &ext4_cache_tables[0].entries[i];
                *e = (struct storage_ext4_cache_entry){.valid = 1, .volume = &g_storage,
                    .volume_generation = 1, .block = 3 + i};
                memcpy(e->data, media + (3 + i) * 4096, 4096);
            }
            calls = 0; fail_at = 0; memset(&pending, 0, sizeof(pending));
            storage_set_io_async_context(true); assert(!storage_acquire_task_io());
            assert(!storage_ext4_cache_read_blocks(&g_storage, 3, 32, output));
            assert(!calls && !memcmp(output, media + 3 * 4096, sizeof(output)));
            assert(storage_async_can_yield() && !storage_io_write_started && storage_task_io_owner == 7);
            storage_release_task_io(7);
            storage_ext4_cache_invalidate(&g_storage); assert(!storage_ext4_cache_release());
        }
        if (!baseline && limit == 256) {
            /* 33 KiB fits the provider's 128 KiB limit but exceeds the fixed
             * 32-filesystem-block workspace at 1 KiB. Its second command must
             * also complete under scoped synchronous context. */
            g_storage.ext4.block_size = 1024; g_storage.ext4.blocks_count = 512;
            calls = completed = discarded = synchronous_calls = asynchronous_calls = 0;
            deferred_start = 3 * 2 + 64; fail_at = 0;
            memset(&pending, 0, sizeof(pending)); memset(output, 0xa5, sizeof(output));
            storage_set_io_async_context(true); assert(!storage_acquire_task_io());
            assert(!storage_ext4_cache_read_blocks(&g_storage, 3, 33, output));
            assert(calls == 2 && synchronous_calls == 2 && !asynchronous_calls && !pending.active);
            assert(!memcmp(output, media + 3 * 1024, 33 * 1024));
            assert(output[33 * 1024] == 0xa5 && storage_async_can_yield() && !storage_io_write_started);
            storage_release_task_io(7);
            storage_ext4_cache_invalidate(&g_storage); assert(!storage_ext4_cache_release());
        }
        block_device_put(&g_storage.device_ref); assert(!unregister_block_device(&device));
    }
    puts("PASS exact block provider read continuation across native request caps");
    return 0;
}
