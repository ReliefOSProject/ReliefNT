/* SPDX-License-Identifier: Apache-2.0 */
#define main unused_swap_ext_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

#include <reliefnt/swap_internal.h>
extern int swap_storage_file_extent(const struct storage_node *,uint64_t,struct swap_storage_extent *) __attribute__((weak));
extern int swap_storage_file_initialize(const struct storage_node *,uint64_t,uint64_t) __attribute__((weak));
extern int swap_storage_file_device(const struct storage_node *,struct block_device_ref *,uint64_t *,uint64_t *) __attribute__((weak));

/** @brief Exercise ext holes and conversion with its real inode/block provider.
 * @param argc Argument count. @param argv Path to fresh ext4 image.
 * @return Zero after checking the resulting block map and file bytes.
 */
int main(int argc,char **argv)
{
    if(!swap_storage_file_extent || !swap_storage_file_initialize || !swap_storage_file_device) {
        fputs("FAIL: real ext swap extent provider is absent\n",stderr);return 1;
    }
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);rewind(file);
    uint8_t *image=malloc((size_t)size);assert(image && fread(image,1,(size_t)size,file)==(size_t)size);fclose(file);
    struct storage_volume *volume=&g_volumes[0];*volume=(struct storage_volume){.volume_id=0,.ext_sector_count=size/512};
    test_memory_bind(volume,image,(uint64_t)size);assert(!storage_ext4_mount(volume));
    volume->fs_ops=&storage_ext_ops;volume->ready=true;g_active_volume=volume;
    struct ext4_inode_view in;assert(!storage_ext4_read_inode(volume,12,&in));
    assert(in.flags&EXT4_EXTENTS_FL);struct storage_node node;
    storage_ext4_node_from_inode(volume,12,&in,&node);
    struct storage_inode_ref *pin=NULL;assert(!storage_inode_get(&node,&pin) && pin);
    assert(!storage_inode_swap_claim(pin));
    test_memory_disk(volume)->registration.info.flags|=BLOCK_DEVICE_SWAP_SAFE;
    struct block_device_ref device={0};uint64_t first=~0ull,count=0;
    assert(!swap_storage_file_device(&node,&device,&first,&count));
    assert(device.device==volume->device_ref.device && !first && count==(uint64_t)size/512);
    block_device_put(&device);
    struct swap_storage_extent extent;
    assert(!swap_storage_file_extent(&node,0,&extent) && !extent.unwritten && extent.logical==0);
    assert(swap_storage_file_extent(&node,4096,&extent)==-EINVAL); /* a hole */
    assert(!storage_ext4_fallocate(volume,12,1,4096,4096));
    assert(!swap_storage_file_extent(&node,4096,&extent) && extent.unwritten);
    assert(!swap_storage_file_initialize(&node,4096,4096));
    assert(!swap_storage_file_extent(&node,4096,&extent) && !extent.unwritten);
    assert(!storage_ext4_read_inode(volume,12,&in));
    uint8_t bytes[4096];uint32_t done=0;memset(bytes,0xff,sizeof(bytes));
    assert(!storage_ext4_read_file_range(volume,12,&in,4096,bytes,sizeof(bytes),&done) && done==4096);
    for(unsigned i=0;i<sizeof(bytes);++i)assert(!bytes[i]);
    puts("ext4 swap extents and unwritten conversion: ok");return 0;
}
