#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
void exfat_cache_invalidate(void) {}
/* This metadata-only test seeds scalar disk identities; no hardware scan. */
int storage_refresh_disks(void){return 0;}
#include "../../block/core/storage_state.c"
#include "../../block/core/device_number.c"
#include "../../block/core/storage_state_data.c"
static unsigned locked;
/* Metadata-only ext-family fixture: no registered ISO transport is entered. */
int block_device_info(const struct block_device_ref *reference,struct block_device_info *out)
{(void)reference;(void)out;abort();}
void kernel_execution_lock_irqsave(uint64_t *flags) { assert(!locked); locked = 1; *flags = 17; }
void kernel_execution_unlock_irqrestore(uint64_t flags) { assert(locked && flags == 17); locked = 0; }
void storage_format_u32(char *out, uint32_t cap, const char *prefix, uint32_t n, int part)
{
    if (part < 0) snprintf(out, cap, "%s%u", prefix, n);
    else snprintf(out, cap, "%s%up%u", prefix, n, part + 1);
}
int storage_disk_device_name(uint32_t disk, int32_t part, char *out, uint32_t cap)
{
    if (disk >= 2 || part < -1 || part >= (int32_t)RELIEFOS_DISK_MAX_PARTITIONS) return -2;
    if (part < 0) snprintf(out, cap, "/dev/sd%c", (char)('a' + disk));
    else snprintf(out, cap, "/dev/sd%c%u", (char)('a' + disk), (unsigned)part + 1u);
    return 0;
}
#include "../../fs/vfs/mounts.c"
static int duplicate, io_error;
int storage_disk_partition_uuid(uint32_t disk, uint32_t part, char uuid[37])
{
    if (io_error) return -5;
    if (part != 3 || (disk && !duplicate)) return -2;
    strcpy(uuid, "12345678-1234-5678-90ab-112233445566");
    return 0;
}
#include "../../block/device/storage_devlinks.c"
int main(void)
{
    g_devfs_enabled = 1;
    g_volumes[0] = (struct storage_volume){.ready=1, .filesystem=STORAGE_FILESYSTEM_EXT4,
        .transport=STORAGE_TRANSPORT_MEMORY,.ext_device=0x100};
    g_install_disks[0].device=block_device_make_number(259,0);g_install_disks[0].present = 1; g_install_disks[0].transport = STORAGE_TRANSPORT_AHCI;
    g_install_disks[1].device=block_device_make_number(259,256);g_install_disks[1].present = 1; g_install_disks[1].transport = STORAGE_TRANSPORT_AHCI;
    g_install_disk_count = 2;
    g_volumes[1] = (struct storage_volume){.ready=1, .volume_id=1, .filesystem=STORAGE_FILESYSTEM_FAT32,
        .data_partition_mount=1, .source_disk_id=0, .source_partition_index=0};
    g_volumes[2] = (struct storage_volume){.ready=1, .volume_id=2, .filesystem=STORAGE_FILESYSTEM_EXT2,
        .data_partition_mount=1, .source_disk_id=1, .source_partition_index=3};
    g_volumes[3] = (struct storage_volume){.ready=1, .volume_id=3, .filesystem=STORAGE_FILESYSTEM_EXT2};
    assert(storage_volume_device(1) == storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(0, 0)));
    assert(storage_volume_device(2) == storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(1, 3)));
    assert(storage_volume_device(0) == 0x100 && storage_volume_device(3) == 4);
    assert(storage_volume_device(STORAGE_MAX_VOLUMES) == 0);
    /* Boot-selected GPT identities must also work before an fstab mount. */
    g_volumes[0].transport = STORAGE_TRANSPORT_AHCI;
    g_volumes[0].ext_device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(1, 7));
    assert(storage_volume_device(0) == g_volumes[0].ext_device);
    g_volumes[0].transport = STORAGE_TRANSPORT_MEMORY;g_volumes[0].ext_device=0x100;
    const uint8_t guid[]={0x78,0x56,0x34,0x12,0x34,0x12,0x78,0x56,0x90,0xab,0x11,0x22,0x33,0x44,0x55,0x66};
    char uuid[37],target[48]; storage_partition_guid_text(guid,uuid);
    assert(!strcmp(uuid,"12345678-1234-5678-90ab-112233445566"));
    assert(storage_devlink_target("/dev/disk/by-partuuid/12345678-1234-5678-90ab-112233445566",target)==0);
    assert(!strcmp(target,"../../sda4"));
    assert(storage_devlink_target("/dev/disk/by-partuuid/x",target)==-2);
    duplicate=1;
    assert(storage_devlink_target("/dev/disk/by-partuuid/12345678-1234-5678-90ab-112233445566",target)==-76);
    io_error=1;
    assert(storage_devlink_target("/dev/disk/by-partuuid/12345678-1234-5678-90ab-112233445566",target)==-5);
    puts("PASS volume stat device numbers, GPT identity, ambiguous identities and I/O errors");
}
