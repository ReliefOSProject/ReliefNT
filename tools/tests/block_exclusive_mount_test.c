#define main unused_ext_mount_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

static unsigned absent_reads;
static int absent_disk_read(void *context,uint64_t lba,uint32_t sectors,void *buffer)
{(void)context;(void)lba;(void)sectors;(void)buffer;++absent_reads;return -EIO;}
static int absent_disk_write(void *context,uint64_t lba,uint32_t sectors,const void *buffer)
{(void)context;(void)lba;(void)sectors;(void)buffer;abort();}
static uint8_t hybrid_metadata[2][512];
static int hybrid_read(void *context,uint64_t lba,uint32_t sectors,void *buffer)
{
    assert(context==hybrid_metadata);
    if(lba>2 || sectors>2-lba)return -EIO;
    memcpy(buffer,hybrid_metadata[lba],(size_t)sectors*512);return 0;
}
/** @brief Match Linux exclusive-open mount conflicts without denying siblings. */
int main(void)
{
    static const struct block_device_operations ops={.read=absent_disk_read,.write=absent_disk_write};
    static const struct block_device_operations hybrid_ops={.read=hybrid_read};
    const struct block_device_registration disks[]={
        {.info={.device=0x10300,.blocks=4096,.block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_WRITE_THROUGH},.operations=&ops},
        {.info={.device=0x110300,.blocks=4096,.block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_WRITE_THROUGH},.operations=&ops},
        {.info={.device=0x120300,.blocks=4096,.block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_READ_ONLY,
            .transport=BLOCK_TRANSPORT_USB},.operations=&ops},
        {.info={.device=0x130300,.blocks=4096,.block_size=512,.max_blocks=64,.flags=BLOCK_DEVICE_READ_ONLY},
            .operations=&hybrid_ops,.context=hybrid_metadata}};
    for(unsigned i=0;i<4;++i){assert(!register_block_device(&disks[i]));g_install_disks[i].device=disks[i].info.device;
        g_install_disks[i].present=1;}
    g_install_disk_count = 4;
    struct install_disk_state *prepared=NULL;uint64_t sectors=0;
    assert(!disk_manage_prepare(2,&prepared,&sectors) && prepared==&g_install_disks[2] && sectors==4096);
    struct block_device_ref readonly={0};unsigned char sector[512]={0};
    assert(!block_device_get(disks[2].info.device,&readonly));
    assert(block_device_write(&readonly,0,1,sector)==-EROFS);
    block_device_put(&readonly);
    uint32_t result=UINT32_MAX;bool fresh=true;
    assert(storage_open_block_partition(2,0,NULL,0,&result,&fresh)==-EROFS);
    assert(!fresh && !result && !absent_reads); /* no filesystem or partition reads */
    assert(storage_open_block_partition(2,0,NULL,MS_RDONLY,&result,&fresh)==-EIO);
    assert(absent_reads); /* read-only admission proceeds to the actual probe */
    /* Damaged GPT signature and a valid-looking MBR data entry must never
     * publish a range when the protective marker's size was corrupted. */
    uint8_t *mbr=hybrid_metadata[0];mbr[510]=0x55;mbr[511]=0xaa;
    mbr[446+4]=0x0c;mbr[446+8]=10;mbr[446+12]=100;
    mbr[462+4]=0xee;mbr[462+8]=1;
    memcpy(hybrid_metadata[1],"BROKEN!!",8);
    uint64_t first=~0ull,count=~0ull;
    assert(storage_disk_block_info(3,0,&first,&count)==-EOPNOTSUPP && !first && !count);
    assert(g_install_disks[3].block_cache && !g_install_disks[3].block_cache->valid);
    const struct reliefos_disk_partition_format unavailable_format={.disk_id=1,.partition_index=0,
        .filesystem=RELIEFOS_DISK_FILESYSTEM_EXT4};
    const struct reliefos_disk_partition_create unavailable_create={.disk_id=1,.size_mib=16,
        .filesystem=RELIEFOS_DISK_FILESYSTEM_EXT4};
    assert(storage_disk_format_partition(&unavailable_format)==-ENODEV);
    assert(storage_disk_create_partition(&unavailable_create)==-ENODEV);
    g_volumes[0] = (struct storage_volume){.ready = true,
        .filesystem = STORAGE_FILESYSTEM_EXT4, .source_disk_id = 0,
        .ext_device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(0, 2))};
    assert(storage_disk_block_busy(0, -1) == 1);
    assert(storage_disk_block_busy(0, 2) == 1);
    assert(storage_disk_block_busy(0, 0) == 0);
    assert(storage_disk_block_busy(1, -1) == 0);
    uint32_t reused=UINT32_MAX;
    bool created=true;
    assert(!storage_open_block_partition(0,2,"ext4",MS_NOEXEC|MS_NOATIME,&reused,&created) && reused==0 && !created);
    assert(!g_volumes[0].mount_flags && !g_volumes[0].vfs_managed);
    assert(storage_open_block_partition(0,2,"ext2",0,&reused,&created)==-EINVAL);
    assert(storage_open_block_partition(0,2,"ext4",MS_RDONLY,&reused,&created)==-EBUSY);
    g_volumes[0].mount_flags=MS_RDONLY;
    assert(!storage_open_block_partition(0,2,NULL,MS_RDONLY|MS_RELATIME,&reused,&created) && reused==0 && !created);
    assert(storage_open_block_partition(0,2,NULL,0,&reused,&created)==-EBUSY);
    g_volumes[0].mount_flags=0;
    g_volumes[STORAGE_VOLUME_BOOT] = (struct storage_volume){.ready = true,
        .filesystem = STORAGE_FILESYSTEM_FAT32, .source_disk_id = 0,
        .esp_device = storage_block_rdev(STORAGE_BLOCK_VOLUME_ID(0, 1))};
    assert(storage_disk_block_busy(0, 1) == 1 && storage_disk_block_busy(0, 0) == 0);
    assert(!storage_open_block_partition(0,1,"vfat",MS_NOSUID|MS_NODEV,&reused,&created) && reused==STORAGE_VOLUME_BOOT && !created);
    assert(storage_open_block_partition(0,1,"ext4",0,&reused,&created)==-EINVAL);
    g_volumes[0].transport = STORAGE_TRANSPORT_MEMORY;
    g_volumes[STORAGE_VOLUME_BOOT].ready = false;
    assert(storage_disk_block_busy(0, -1) == 0);
    /* Optical mounts have no install-disk dev_t; their zero-initialized
     * source_disk_id must not claim the unrelated first writable disk. */
    g_volumes[STORAGE_VOLUME_DYNAMIC_FIRST] = (struct storage_volume){
        .ready = true, .filesystem = STORAGE_FILESYSTEM_ISO9660,
        .kind = STORAGE_VOLUME_AHCI, .transport = STORAGE_TRANSPORT_AHCI};
    assert(storage_disk_block_busy(0, -1) == 0);
    assert(storage_disk_block_busy(0, 0) == 0);
    assert(storage_disk_block_busy(99, -1) == -22);
    puts("PASS exclusive block open: mounted disk/partition busy; siblings, other disks, RAM and unrelated optical mounts excluded");
    storage_disk_block_cache_reset();
    for(unsigned i=0;i<4;++i)assert(!unregister_block_device(&disks[i]));
    return 0;
}
