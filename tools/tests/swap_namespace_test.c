/* SPDX-License-Identifier: Apache-2.0 */
#define RELIEFNT_FIXTURE_REAL_DAC
#define main unused_swap_namespace_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

/** @brief Active file inode excludes pathname mutations but not unrelated writes.
 * @param argc Argument count. @param argv Real ext image path.
 * @return Zero on success.
 */
int main(int argc,char **argv)
{
    assert(argc==2);FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);rewind(file);
    uint8_t *image=malloc((size_t)size);assert(image && fread(image,1,(size_t)size,file)==(size_t)size);fclose(file);
    struct storage_volume *volume=&g_volumes[0];*volume=(struct storage_volume){.volume_id=0,.ext_sector_count=size/512};
    test_memory_bind(volume,image,(uint64_t)size);assert(!storage_ext4_mount(volume));
    volume->fs_ops=&storage_ext_ops;volume->ready=true;g_active_volume=volume;
    struct vfs_namespace ns={0};struct vfs_path root={0};struct vfs_lookup_result swap={0};
    assert(!mount_fixture_begin(&ns,volume,&root));
    assert(!vfs_kernel_lookup(&ns,"/swap",0,&swap));
    struct storage_inode_ref *inode=NULL;assert(!storage_inode_get(vfs_path_node(&swap.path),&inode) && inode);
    assert(!storage_inode_swap_claim(inode));
    assert(vfs_path_remount(&root,MS_RDONLY)==-EBUSY);
    assert(mount_fixture_remove(&ns,"/swap",false)==-EPERM);
    assert(mount_fixture_rename(&ns,"/swap","/renamed")==-EPERM);
    assert(mount_fixture_rename(&ns,"/other","/swap")==-EPERM);
    assert(vfs_path_fallocate(&swap.path,1,0,4096)==-ETXTBSY);
    uint32_t done=0;char z='z';assert(!mount_fixture_write(&ns,"/other",0,&z,1,&done) && done==1);
    storage_inode_swap_release(inode);assert(!mount_fixture_remove(&ns,"/swap",false));
    assert(!vfs_path_remount(&root,MS_RDONLY));
    puts("swap namespace exclusion: ok");return 0;
}
