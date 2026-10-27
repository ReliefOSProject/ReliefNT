#define main unused_ext_directory_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main

/* Direct native parent/inode preparation preserves the precise FAT slot
 * layout of this codec regression; common VFS lookup verifies visibility. */
static void create_fat_member(const struct storage_node *parent,const char *name,const void *data,uint32_t length)
{
    struct storage_node node;assert(!storage_cluster_new_file(&g_volumes[0],parent,name,&node));
    uint32_t done=0;assert(!storage_write_held_node(&node,0,data,length,&done) && done==length);
}

/** @brief Grow real FAT directories with an LFN that cannot fit the old tail. */
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0);
    rewind(file);
    g_active_volume = &g_volumes[0];
    *g_active_volume = (struct storage_volume){.volume_id = 0, .esp_sector_count = size / 512};
    uint8_t *image=malloc(size);assert(image);
    assert(fread(image,1,size,file)==(size_t)size && !fclose(file));
    test_memory_bind(g_active_volume,image,(uint64_t)size);

    assert(fat32_mount() == 0 && g_storage.cluster_bytes == 512);
    g_storage.fs_ops=&storage_fat_ops;
    g_storage.ready = true; storage_cache_invalidate();
    struct vfs_namespace ns={0};struct vfs_path root={0};struct storage_node mods;
    assert(!mount_fixture_begin(&ns,&g_volumes[0],&root));
    assert(!fat32_mkdir_child(vfs_path_node(&root),"mods",&mods));
    for (unsigned i = 0; i < 12; ++i) {
        char path[80]; snprintf(path, sizeof(path), "s%07u", i);
        create_fat_member(&mods,path,"x",1);
    }
    create_fat_member(&mods,"gfxterm_menu.mod","module",6);
    struct storage_node node;
    int ret = mount_fixture_lookup(&ns,"/mods/gfxterm_menu.mod", &node);
    fprintf(stderr, "LFN after tail expansion lookup=%d\n", ret);
    assert(ret == 0 && node.size == 6);
    for (unsigned i = 0; i < 300; ++i) {
        char path[100]; snprintf(path, sizeof(path), "long-module-name-%04u.mod", i);
        create_fat_member(&mods,path,"abc",3);
        assert(!storage_backend_lookup(&g_volumes[0],&mods,path,&node) && node.size==3);
        char absolute[128];snprintf(absolute,sizeof(absolute),"/mods/%s",path);
        assert(!mount_fixture_lookup(&ns,absolute,&node) && node.size==3);
    }
    vfs_path_put(&root);assert(!vfs_namespace_destroy(&ns));
    file = fopen(argv[1], "wb"); assert(file);
    assert(fwrite(test_memory_bytes(&g_storage), 1, size, file) == (size_t)size); assert(!fclose(file));
    test_memory_release(&g_storage);
    puts("PASS FAT directory growth: short/LFN entries remain visible across incomplete cluster tails");
    return 0;
}
