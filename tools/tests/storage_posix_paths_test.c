/* Exercise actual retained ext-family paths and native filename restrictions. */
#define main mounted_mkdir_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

static void finish_volume(void)
{
    storage_ext4_journal_close(&g_volumes[0]);
    test_memory_release(&g_volumes[0]);
}
int main(int argc,char **argv)
{
    assert(argc==2 || argc==3);load_volume(argv[1],0);g_active_volume=&g_volumes[0];
    const char *backend_path="/A:0-backend";
    assert(!storage_ext4_replace(&g_storage,backend_path,"cookie",6));
    struct storage_node node;assert(!storage_ext4_ops.lookup(&g_storage,backend_path,&node));
    char content[8];uint32_t got;
    assert(!storage_read_node(&node,0,content,sizeof(content),&got) && got==6 && !memcmp(content,"cookie",6));
    assert(!storage_ext4_replace(&g_storage,"/A\\0-backend","literal",7));
    assert(!storage_ext4_ops.lookup(&g_storage,"/A\\0-backend",&node));
    assert(!storage_read_node(&node,0,content,sizeof(content),&got) && got==7 && !memcmp(content,"literal",7));
    char deep[128]="";
    for(unsigned level=0;level<24;++level) {
        strcat(deep,"/d");assert(!storage_ext4_ops.mkdir(&g_storage,deep));
    }
    strcat(deep,"/file");assert(!storage_ext4_replace(&g_storage,deep,"deep",4));
    assert(!storage_ext4_ops.lookup(&g_storage,deep,&node));
    assert(!storage_read_node(&node,0,content,sizeof(content),&got) && got==4 && !memcmp(content,"deep",4));
    puts("PASS native ext-family backend creates and reads colon/backslash names and 24 directory levels");
    if(argc==3 && !strcmp(argv[2],"--backend-only")){finish_volume();return 0;}

    struct vfs_namespace ns={0};struct vfs_path root={0},session={0};
    assert(!mount_fixture_begin(&ns,&g_volumes[0],&root));
    const char *directories[]={"/var","/var/lib","/var/lib/xdm","/var/lib/xdm/authdir",
        "/var/lib/xdm/authdir/authfiles","/session:0","/A\\0"};
    for(unsigned i=0;i<sizeof(directories)/sizeof(directories[0]);++i)
        assert(!vfs_kernel_mkdir(&ns,directories[i],0755));
    struct vfs_lookup_result found={0};char parent[256];
    assert(!vfs_kernel_lookup(&ns,"/var/lib/xdm/authdir/authfiles/A:0-XXXXXX",VFS_LOOKUP_MISSING,&found));
    assert(found.missing && !strcmp(found.last,"A:0-XXXXXX"));
    assert(!vfs_path_format(&ns,&root,&found.path,parent,sizeof(parent)) &&
        !strcmp(parent,"/var/lib/xdm/authdir/authfiles"));vfs_path_put(&found.path);
    assert(!vfs_path_lookup(&ns,&root,&root,"/session:0",0,&session));
    found=(struct vfs_lookup_result){0};struct vfs_lookup_policy policy={.flags=VFS_LOOKUP_MISSING};
    assert(!vfs_path_resolve(&ns,&root,&session,"./A:0-cookie",&policy,&found));
    assert(found.missing && vfs_path_equal(&found.path,&session) && !strcmp(found.last,"A:0-cookie"));
    vfs_path_put(&found.path);
    assert(!vfs_kernel_lookup(&ns,"/session:0/A:0-cookie",VFS_LOOKUP_PARENT,&found));
    assert(vfs_path_equal(&found.path,&session) && !strcmp(found.last,"A:0-cookie"));vfs_path_put(&found.path);
    assert(vfs_kernel_lookup(&ns,"relative:A",0,&found)==-EINVAL);
    /* Backslash is an ordinary ext-family component byte. The old lexical
     * helper's blanket rejection never belonged in the shared POSIX walker. */
    assert(!vfs_kernel_write_file(&ns,"/A\\0/file","slash",5));
    assert(!mount_fixture_lookup(&ns,"/A\\0/file",&node));
    assert(!storage_read_node(&node,0,content,sizeof(content),&got) && got==5 && !memcmp(content,"slash",5));
    assert(fat32_validate_name("A:0-cookie")==-EINVAL && !fat32_validate_name("A-0-cookie"));
    uint16_t utf16[255];uint32_t utf16_len;
    assert(exfat_utf8_name("A:0-cookie",utf16,&utf16_len)==-EINVAL && !exfat_utf8_name("A-0-cookie",utf16,&utf16_len));

    assert(vfs_kernel_mkdir(&ns,"/session:0",0755)==-EEXIST);
    assert(!vfs_kernel_write_file(&ns,"/session:0/A:0-cookie","secret",6));
    assert(!mount_fixture_lookup(&ns,"/session:0/A:0-cookie",&node));
    struct reliefos_permissions permissions={0600,1000,1000};
    assert(!storage_inode_permissions(&node,&permissions,true));memset(&permissions,0,sizeof(permissions));
    assert(!storage_inode_permissions(&node,&permissions,false));
    assert(permissions.mode==0600 && permissions.uid==1000 && permissions.gid==1000);
    assert(!storage_read_node(&node,0,content,sizeof(content),&got) && got==6 && !memcmp(content,"secret",6));
    assert(!mount_fixture_rename(&ns,"/session:0/A:0-cookie","/session:0/B:1-cookie"));
    assert(mount_fixture_lookup(&ns,"/session:0/A:0-cookie",&node)==-ENOENT);
    assert(!mount_fixture_lookup(&ns,"/session:0/B:1-cookie",&node));
    assert(!mount_fixture_remove(&ns,"/session:0/B:1-cookie",false));
    assert(mount_fixture_lookup(&ns,"/session:0/B:1-cookie",&node)==-ENOENT);
    vfs_path_put(&session);vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    assert(!storage_sync_all());finish_volume();
    puts("PASS actual POSIX paths: XDM colon parent/name, relative cwd, literal backslash, create/read/DAC/rename/unlink and native FAT/exFAT restrictions");
    return 0;
}
