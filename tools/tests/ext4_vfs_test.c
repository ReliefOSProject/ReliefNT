#define main legacy_mkdir_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main
int main(int argc,char **argv)
{
    assert(argc==3); load_volume(argv[1],0); load_volume(argv[1],1);
    g_active_volume=&g_volumes[0];
    struct vfs_namespace ns={0};struct vfs_path root={0},target={0};struct vfs_mount *mount=NULL;
    assert(!mount_fixture_begin(&ns,&g_volumes[0],&root));
    assert(!vfs_kernel_mkdir(&ns,"/target",0755));
    assert(!vfs_path_lookup(&ns,&root,&root,"/target",0,&target));
    assert(!storage_create_vfs_mount(&g_volumes[1],NULL,&mount));
    assert(!vfs_mount_attach(&ns,mount,&target));vfs_mount_put(mount);vfs_path_put(&target);
    assert(vfs_kernel_mkdir(&ns,"/a",0755)==0); assert(vfs_kernel_mkdir(&ns,"/b",0755)==0);
    assert(vfs_kernel_mkdir(&ns,"/target/t",0755)==0);
    uint8_t data[8192],out[8192]; memset(data,0xa3,sizeof(data));
    assert(vfs_kernel_write_file(&ns,"/a/file",data,sizeof(data))==0);
    struct storage_node node; assert(mount_fixture_lookup(&ns,"/a/file",&node)==0);
    struct storage_inode_ref *held; assert(storage_inode_get(&node,&held)==0);
    assert(mount_fixture_link(&ns,"/a/file","/b/hard")==0);
    assert(mount_fixture_remove(&ns,"/a/file",false)==0); assert(mount_fixture_remove(&ns,"/b/hard",false)==0);
    uint32_t got; assert(storage_read_node(&node,0,out,sizeof(out),&got)==0 && got==sizeof(out));
    assert(!memcmp(data,out,sizeof(out)));
    assert(storage_write_held_node(&node,13,"held",4,&got)==0 && got==4);
    assert(storage_truncate_held_node(&node,51)==0);
    assert(storage_inode_put(held)==0);
    struct storage_node n1,n2; struct storage_inode_ref *h1,*h2;
    assert(vfs_kernel_write_file(&ns,"/a/open1","one",3)==0 && vfs_kernel_write_file(&ns,"/a/open2","two",3)==0);
    assert(mount_fixture_lookup(&ns,"/a/open1",&n1)==0 && mount_fixture_lookup(&ns,"/a/open2",&n2)==0);
    assert(storage_inode_get(&n1,&h1)==0 && storage_inode_get(&n2,&h2)==0);
    assert(mount_fixture_remove(&ns,"/a/open1",false)==0 && mount_fixture_remove(&ns,"/a/open2",false)==0);
    assert(storage_inode_put(h1)==0); /* Remove a non-head orphan. */
    assert(storage_read_node(&n2,0,out,3,&got)==0 && got==3 && !memcmp(out,"two",3));
    assert(storage_inode_put(h2)==0);
    assert(vfs_kernel_write_file(&ns,"/a/reused","reuse",5)==0);
    assert(storage_inode_refresh(&n1)==-116);
    assert(mount_fixture_remove(&ns,"/a/reused",false)==0);
    assert(vfs_kernel_write_file(&ns,"/a/one",data,999)==0);
    assert(mount_fixture_rename(&ns,"/a/one","/b/two")==0);
    assert(mount_fixture_lookup(&ns,"/b/two",&node)==0);
    assert(mount_fixture_rename(&ns,"/b/two","/target/t/two")==-18);
    assert(mount_fixture_symlink(&ns,"/b/two","/a/link")==0);
    char link[128]; assert(mount_fixture_readlink(&ns,"/a/link",link,sizeof(link),&got)==0);
    assert(got==6 && !memcmp(link,"/b/two",6));
    struct reliefos_permissions p={.uid=70001,.gid=90002,.mode=0600};
    assert(storage_inode_permissions(&node,&p,true)==0);
    struct linux_stat_abi st; assert(storage_inode_stat(&node,&st)==0);
    assert(st.st_uid==70001 && st.st_gid==90002 && st.st_size==999);
    assert(storage_sync_all()==0);
    g_active_volume=&g_volumes[0];
    struct ext4_inode_view native; struct storage_ext4_map_result mapping;
    assert(storage_ext4_read_inode(&g_storage,node.first_cluster,&native)==0);
    assert(storage_ext4_map_block(&g_storage,node.first_cluster,&native,0,false,&mapping)==0);
    assert(storage_read_node(&node,0,out,512,&got)==0 && got==512);
    struct storage_volume alias=g_storage; out[0]=0x77;
    assert(storage_write_device(&alias,mapping.physical*(g_storage.ext4.block_size/512),1,out)==0);
    assert(storage_read_node(&node,0,out,1,&got)==0 && got==1 && out[0]==0x77);
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    g_active_volume=&g_volumes[0]; assert(ext2_mount()==0);
    assert(storage_inode_refresh(&node)==-116);
    FILE *f=fopen(argv[2],"wb"); assert(f);
    assert(fwrite(test_memory_bytes(&g_volumes[0]),1,test_memory_length(&g_volumes[0]),f)==test_memory_length(&g_volumes[0])); fclose(f);
    for (unsigned i=0;i<2;i++) { storage_ext4_journal_close(&g_volumes[i]); test_memory_release(&g_volumes[i]); }
    puts("PASS VFS ext-family namespace, held unlink/write/truncate, permissions, stale generation");
    return 0;
}
