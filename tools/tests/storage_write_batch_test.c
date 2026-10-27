#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include "../../block/core/storage_state.c"
#include "../../block/core/device_number.c"
#include "../../block/core/storage_state_data.c"
#include "../../fs/ext_common/ext2_cache.c"
#include "../../block/device/registry.c"

static uint8_t media[32768];
static unsigned commands, reject_batch, fail_sector;
static int read_result;
static unsigned read_failures;
static int fake_ahci_read(void *context, uint64_t lba,
                          uint32_t sectors, void *buffer)
{
    assert(context==media && lba == 100 && sectors == 8);
    if (!read_result) memcpy(buffer, media, sectors * 512);
    return read_result;
}
static int fake_ahci_write(void *context, uint64_t lba,
                            uint32_t sectors, const void *buffer)
{
    assert(context==media && lba >= 100 && lba + sectors <= 164 && sectors <= 64);
    ++commands;
    if (reject_batch && sectors > 1) {
        memcpy(media + (lba - 100) * 512, buffer, 512);
        return -5;
    }
    if (fail_sector && lba == 103) return -5;
    memcpy(media + (lba - 100) * 512, buffer, sectors * 512);
    return 0;
}
#include "../../block/core/storage_block.c"

void console_printf(const char *format, ...)
{ if (strstr(format, "device read failed")) ++read_failures; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; if (strstr(format, "device read failed")) ++read_failures;
}
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
uint32_t smp_cpu_count(void) { return 1; }
uint8_t x86_64_inb(uint16_t port) { (void)port; assert(0); return 0; }
uint16_t x86_64_inw(uint16_t port) { (void)port; assert(0); return 0; }
void x86_64_outb(uint8_t value, uint16_t port) { (void)value; (void)port; assert(0); }
void x86_64_outw(uint16_t value, uint16_t port) { (void)value; (void)port; assert(0); }
uint64_t time_ticks(void) { return 1; }
void exfat_cache_invalidate(void) {}
void storage_ext4_cache_invalidate_write(const struct storage_volume *writer,
                                         uint64_t lba, uint32_t count)
{
    (void)writer;
    (void)lba;
    (void)count;
}

int main(void)
{
    static const struct block_device_operations ops={.read=fake_ahci_read,.write=fake_ahci_write};
    const struct block_device_registration device={.info={.device=0x801,.blocks=164,.block_size=512,.max_blocks=64,
        .flags=BLOCK_DEVICE_WRITE_THROUGH},.operations=&ops,.context=media};
    assert(!register_block_device(&device));
    assert(!block_device_get(device.info.device,&g_storage.device_ref));
    g_storage.kind = STORAGE_VOLUME_AHCI;
    g_storage.transport = STORAGE_TRANSPORT_AHCI;
    uint8_t data[32768];
    for (unsigned i = 0; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 17);
    assert(!storage_write_sectors(100, 64, data));
    assert(!memcmp(data, media, sizeof(data)) && commands == 1);
    memset(media, 0, sizeof(media));
    commands = 0;
    reject_batch = 1;
    assert(!storage_write_sectors(100, 64, data));
    assert(!memcmp(data, media, sizeof(data)) && commands == 65);
    fail_sector = 1;
    assert(storage_write_sectors(100, 64, data) == -5);
    read_result = -RELIEFOS_EAGAIN;
    assert(storage_read_device(&g_storage, 100, 8, data) == -RELIEFOS_EAGAIN);
    assert(read_failures == 0);
    read_result = 0;
    assert(storage_read_device(&g_storage, 100, 8, data) == 0);
    assert(!memcmp(data, media, 4096));
    read_result = -5;
    assert(storage_read_device(&g_storage, 100, 8, data) == -5);
    assert(read_failures == 1);
    block_device_put(&g_storage.device_ref);assert(!unregister_block_device(&device));
    puts("PASS bounded registered-block write batching, partial-command retry and error propagation");
}

void *kernel_malloc(size_t n){return malloc(n);}
void kernel_free(void *p){free(p);}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags){*flags=0;kernel_spin_lock(lock);}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags){(void)flags;kernel_spin_unlock(lock);}
