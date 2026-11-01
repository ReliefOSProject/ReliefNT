/* SPDX-License-Identifier: Apache-2.0 */
#define main unused_swap_storage_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main
#include <reliefnt/swap_internal.h>

static uint8_t image[1000*512];
/** @brief Read bounded blocks from the fixture disk.
 * @param context Disk bytes. @param first First LBA. @param count Sectors.
 * @param buffer Output. @return Zero.
 */
static int disk_read(void *context,uint64_t first,uint32_t count,void *buffer)
{ assert(context==image && first+count<=1000);memcpy(buffer,image+first*512,(size_t)count*512);return 0; }
/** @brief Write bounded blocks to the fixture disk.
 * @param context Disk bytes. @param first First LBA. @param count Sectors.
 * @param buffer Input. @return Zero.
 */
static int disk_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{ assert(context==image && first+count<=1000);memcpy(image+first*512,buffer,(size_t)count*512);return 0; }

/** @brief Raw aliases are excluded, while trusted filesystem transport works.
 * @return Zero when all checks pass.
 */
int main(void)
{
    static const struct block_device_operations ops={.read=disk_read,.write=disk_write};
    const struct block_device_registration disk={.info={.device=0x10300,.blocks=1000,
        .block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_WRITE_THROUGH,
        .transport=STORAGE_TRANSPORT_AHCI},.operations=&ops,.context=image};
    assert(!register_block_device(&disk));
    storage_refresh_disks(); assert(g_install_disk_count && g_install_disks[0].present);
    struct install_disk_state *entry=&g_install_disks[0];
    entry->block_cache=kernel_malloc(sizeof(*entry->block_cache));assert(entry->block_cache);
    memset(entry->block_cache,0,sizeof(*entry->block_cache));
    entry->block_cache->valid=1;entry->block_cache->disk_sectors=1000;
    entry->block_cache->entries[0]=(struct disk_block_partition_range){.first_lba=100,.sector_count=200,.present=1};
    entry->block_cache->entries[1]=(struct disk_block_partition_range){.first_lba=400,.sector_count=200,.present=1};
    struct block_device_ref ref={0};assert(!block_device_get(disk.info.device,&ref));
    struct swap_storage_claim claim={0};assert(!swap_storage_claim_range(&claim,&ref,100,200,true));
    assert(storage_disk_block_busy(0,-1)==1);
    assert(storage_disk_block_busy(0,0)==1);
    assert(storage_disk_block_busy(0,1)==0);
    assert(disk_partition_mutable(entry)==-EBUSY);
    assert(storage_disk_block_reread_owned(0)==-EBUSY);
    uint8_t page[512];memset(page,0x5a,sizeof(page));uint32_t done=99;
    assert(storage_disk_block_write(0,0,0,page,sizeof(page),&done)==-EBUSY && !done);
    assert(storage_disk_block_write(0,-1,0,page,sizeof(page),&done)==-EBUSY && !done);
    assert(!storage_disk_block_write(0,1,0,page,sizeof(page),&done) && done==sizeof(page));
    struct storage_volume volume={.device_ref=ref};
    assert(!storage_write_device(&volume,101,1,page));
    assert(!memcmp(image+101*512,page,512));
    swap_storage_release_range(&claim);assert(!storage_disk_block_write(0,0,0,page,sizeof(page),&done));
    block_device_put(&ref);storage_disk_block_cache_reset();assert(!unregister_block_device(&disk));
    puts("swap raw alias and trusted filesystem I/O: ok");return 0;
}
