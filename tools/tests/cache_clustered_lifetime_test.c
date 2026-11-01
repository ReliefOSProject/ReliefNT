/* Real FAT/exFAT namespace, inode cache, VFS and fault/cache units over a
 * private image. Page-table publication is modeled at the hardware boundary. */
#define RELIEFNT_FIXTURE_REAL_PAGE_CACHE
#define main storage_fixture_unused_main
#include "storage_mkdir_mount_test.c"
#undef main
#include "../../mm/user/fault.c"

static uint64_t published_phys, published_flags;
/** @brief Initialize the host fixture's exclusively owned cache lock.
 * @param lock Lock supplied by production cache initialization.
 */
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state=0; }
/** @brief Reject unexpected tmpfs retention in the clustered-only fixture.
 * @param phys Unused tmpfs frame.
 */
void mm_retain_page(uint64_t phys) { (void)phys; abort(); }
/** @brief Capture a production file-fault publication.
 * @param as Borrowed fixture address space. @param va User page.
 * @param phys Owned backing frame. @param flags Granted page permissions.
 * @return True after capturing the physical frame and flags.
 */
bool address_space_map_user_page(struct address_space *as,uint64_t va,uint64_t phys,uint64_t flags)
{ (void)as; (void)va; published_phys=phys; published_flags=flags; return true; }
/** @brief Translate fixture provider errno unchanged.
 * @param ret Provider result. @return The same errno.
 */
int storage_errno(int ret) { return ret; }

/** @brief Open and fault an actual shared writable file mapping.
 * @param ns Mounted namespace. @param name Absolute file name.
 * @param result Owned lookup path, held until the simulated unmap.
 * @param snapshot Required original file identity output.
 * @return Retained cache frame published by the production fault path.
 */
static uint64_t map_shared(struct vfs_namespace *ns,const char *name,
                           struct vfs_lookup_result *result,struct storage_node *snapshot)
{
    assert(!vfs_kernel_lookup(ns,name,0,result));
    *snapshot=*vfs_path_node(&result->path);
    struct task task={0};
    struct task_file file={.used=1,.path_ref=result->path};
    struct task_vma vma={.used=1,.prot=LINUX_PROT_READ|LINUX_PROT_WRITE,
        .flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_MMAP|TASK_VMA_FLAG_SHARED|
               TASK_VMA_FLAG_SHARED_FILE,.start=0x5000000,.end=0x5001000,
        .file_limit=snapshot->size,.file_node=*snapshot,.mapping_file=&file};
    assert(!task_map_file_vma_page(&task,&vma,vma.start));
    assert((published_flags&(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITE_NOTIFY))==
           (RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITE_NOTIFY));
    return published_phys;
}
/** @brief Create a real native member with one full page of file data.
 * @param parent Actual root directory. @param name Native component.
 * @return Stamped identity of the newly created regular file.
 */
static struct storage_node create_member(const struct storage_node *parent,const char *name)
{
    struct storage_node node; unsigned char bytes[4096]={0}; uint32_t wrote=0;
    assert(!storage_cluster_new_file(&g_volumes[0],parent,name,&node));
    assert(!storage_write_held_node(&node,0,bytes,sizeof(bytes),&wrote) && wrote==sizeof(bytes));
    return node;
}
/** @brief Add unused clustered metadata entries to force the real 128-entry eviction.
 * @param flags FAT or exFAT identity flag. @param start Unique first synthetic slot.
 * @param count Number of distinct unused inode records to intern.
 */
static void fill_inode_cache(uint32_t flags,unsigned start,unsigned count)
{
    for(unsigned i=0;i<count;++i) {
        struct storage_node item={.type=RELIEFOS_FS_TYPE_FILE,.flags=flags,.volume_id=0,
                                  .first_cluster=30000+i,.entry_id=((uint64_t)0xabc<<32)|(start+i)};
        assert(!storage_inode_cache_node(&item));
    }
}
/** @brief Preserve dirty mapping identity across actual native rename.
 * @param ns Mounted namespace. @param root Actual root metadata.
 */
static void test_rename(struct vfs_namespace *ns,const struct storage_node *root)
{
    (void)create_member(root,"old-file.bin");
    struct vfs_lookup_result old={0},newer={0}; struct storage_node a,b;
    uint64_t first=map_shared(ns,"/old-file.bin",&old,&a);
    ((unsigned char *)(uintptr_t)first)[3]=0x73;
    assert(!mount_fixture_rename(ns,"/old-file.bin","/moved-identity-long-file.bin"));
    uint64_t second=map_shared(ns,"/moved-identity-long-file.bin",&newer,&b);
    assert(a.entry_id!=b.entry_id && a.inode_generation==b.inode_generation);
    assert(first==second && ((unsigned char *)(uintptr_t)second)[3]==0x73);
    page_cache_release(first); page_cache_release(second);
    vfs_path_put(&old.path); vfs_path_put(&newer.path);
}
/** @brief Preserve a mapped inode incarnation through clustered metadata eviction.
 * @param ns Mounted namespace. @param root Actual root metadata.
 */
static void test_eviction(struct vfs_namespace *ns,const struct storage_node *root)
{
    fill_inode_cache(root->flags & (STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT),0,128);
    (void)create_member(root,"eviction.bin");
    struct vfs_lookup_result old={0},newer={0}; struct storage_node a,b;
    uint64_t first=map_shared(ns,"/eviction.bin",&old,&a);
    ((unsigned char *)(uintptr_t)first)[7]=0x6b;
    page_cache_release(first); vfs_path_put(&old.path);
    fill_inode_cache(root->flags & (STORAGE_NODE_FLAG_FAT32|STORAGE_NODE_FLAG_EXFAT),1000,1);
    assert(!storage_backend_lookup(&g_volumes[0],root,"eviction.bin",&b));
    assert(a.inode_generation==b.inode_generation);
    uint64_t second=map_shared(ns,"/eviction.bin",&newer,&b);
    assert(second==first && ((unsigned char *)(uintptr_t)second)[7]==0x6b);
    page_cache_release(second); vfs_path_put(&newer.path);
}
/** @brief Final deletion retires dirty pages while a held mapping stays alive.
 * @param ns Mounted namespace. @param root Actual root metadata.
 * @param held Keep the mapping through the unlink when true.
 */
static void test_delete(struct vfs_namespace *ns,const struct storage_node *root,bool held)
{
    (void)create_member(root,"delete.bin");
    struct vfs_lookup_result old={0},newer={0}; struct storage_node a,b;
    uint64_t first=map_shared(ns,"/delete.bin",&old,&a);
    ((unsigned char *)(uintptr_t)first)[5]=0x44;
    if(!held) { page_cache_release(first); vfs_path_put(&old.path); }
    int rc=(root->flags&STORAGE_NODE_FLAG_FAT32) ? fat32_remove_child(root,"delete.bin",false) :
                                                  exfat_remove_child(root,"delete.bin",false);
    assert(!rc);
    if(held) assert(page_cache_owns(first) && ((unsigned char *)(uintptr_t)first)[5]==0x44);
    else assert(!page_cache_owns(first));
    (void)create_member(root,"delete.bin");
    uint64_t second=map_shared(ns,"/delete.bin",&newer,&b);
    assert(a.inode_generation!=b.inode_generation && (!held || second!=first) &&
           ((unsigned char *)(uintptr_t)second)[5]==0);
    if(held) { page_cache_release(first); vfs_path_put(&old.path); assert(!page_cache_owns(first)); }
    page_cache_release(second); vfs_path_put(&newer.path);
}
/** @brief Replacement retires only the replaced inode, after its last hold.
 * @param ns Mounted namespace. @param root Root metadata. @param held Keep the
 * replaced inode's mapping pinned until after publication.
 */
static void test_replace(struct vfs_namespace *ns,const struct storage_node *root,bool held)
{
    (void)create_member(root,"source.bin"); (void)create_member(root,"target.bin");
    struct vfs_lookup_result source={0},target={0},result={0}; struct storage_node a,b,c;
    uint64_t src=map_shared(ns,"/source.bin",&source,&a),old=map_shared(ns,"/target.bin",&target,&b);
    ((uint8_t *)(uintptr_t)src)[4]=0x68; ((uint8_t *)(uintptr_t)old)[4]=0x49;
    if(!held) { page_cache_release(old); vfs_path_put(&target.path); }
    assert(!mount_fixture_rename(ns,"/source.bin","/target.bin"));
    if(held) assert(page_cache_owns(old) && ((uint8_t *)(uintptr_t)old)[4]==0x49);
    else assert(!page_cache_owns(old));
    uint64_t now=map_shared(ns,"/target.bin",&result,&c);
    assert(now==src && c.inode_generation==a.inode_generation && c.inode_generation!=b.inode_generation);
    assert(((uint8_t *)(uintptr_t)now)[4]==0x68);
    if(held) { page_cache_release(old); vfs_path_put(&target.path); assert(!page_cache_owns(old)); }
    page_cache_release(src); page_cache_release(now); vfs_path_put(&source.path); vfs_path_put(&result.path);
}
/** @brief Real clustered growth refreshes a dirty mapped partial-page suffix.
 * @param ns Mounted namespace. @param root Actual root metadata.
 */
static void test_growth(struct vfs_namespace *ns,const struct storage_node *root)
{
    struct storage_node created=create_member(root,"growth.bin");
    assert(!storage_truncate_held_node(&created,1000));
    struct vfs_lookup_result old={0},newer={0}; struct storage_node a,b;
    uint64_t first=map_shared(ns,"/growth.bin",&old,&a);
    assert(a.size==1000); ((uint8_t *)(uintptr_t)first)[3]=0x73;
    uint8_t byte=0x6a; uint32_t written=0;
    assert(!storage_write_held_node(&a,1500,&byte,1,&written) && written==1);
    uint64_t second=map_shared(ns,"/growth.bin",&newer,&b);
    assert(b.size==1501 && first==second && ((uint8_t *)(uintptr_t)second)[3]==0x73 &&
           ((uint8_t *)(uintptr_t)second)[1500]==0x6a);
    page_cache_release(first); page_cache_release(second);
    vfs_path_put(&old.path); vfs_path_put(&newer.path);
}
/** @brief Initialize a synthetic page for the volume-retirement boundary.
 * @param phys Owned frame. @param opaque Unused context. @return Zero.
 */
static int purge_loader(uint64_t phys,void *opaque)
{ (void)opaque; memset((void *)(uintptr_t)phys,0,4096); return 0; }
/** @brief Volume purge hides cache keys while preserving an outstanding frame.
 * @param flags FAT or exFAT type. @return None.
 */
static void test_purge(uint32_t flags)
{
    struct storage_node item={.type=RELIEFOS_FS_TYPE_FILE,.flags=flags,.volume_id=0,
        .first_cluster=321,.entry_id=1234,.size=4096};
    assert(!storage_inode_cache_node(&item));
    uint64_t held=0,found=0;
    assert(!page_cache_load(&item,0,purge_loader,NULL,&held));
    page_cache_mark_shared_writable(held); ((uint8_t *)(uintptr_t)held)[3]=0x73;
    assert(!storage_inode_purge_volume(0));
    assert(page_cache_lookup(&item,0,&found) && page_cache_owns(held));
    struct storage_node replacement=item; replacement.inode_generation=0;
    assert(!storage_inode_cache_node(&replacement) && replacement.inode_generation!=item.inode_generation);
    assert(!page_cache_load(&replacement,0,purge_loader,NULL,&found) && found!=held);
    assert(((uint8_t *)(uintptr_t)held)[3]==0x73 && !((uint8_t *)(uintptr_t)found)[3]);
    page_cache_release(held); assert(!page_cache_owns(held)); page_cache_release(found);
}
/** @brief A managed mount keeps held data, then retires it at final teardown.
 * @param ns Live namespace. @param root Actual root. @param root_path Owned root path.
 */
static void test_teardown(struct vfs_namespace *ns,const struct storage_node *root,struct vfs_path *root_path)
{
    (void)create_member(root,"teardown.bin");
    struct vfs_lookup_result held={0}; struct storage_node item;
    uint64_t frame=map_shared(ns,"/teardown.bin",&held,&item);
    ((uint8_t *)(uintptr_t)frame)[4]=0x59; page_cache_release(frame);
    assert(vfs_namespace_destroy(ns)==-EBUSY);
    uint64_t found=0; assert(!page_cache_lookup(&item,0,&found) && found==frame);
    assert(((uint8_t *)(uintptr_t)found)[4]==0x59); page_cache_release(found);
    vfs_path_put(&held.path); vfs_path_put(root_path);
    assert(!vfs_namespace_destroy(ns)); assert(!page_cache_owns(frame));
}
/** @brief Simulate a failed filesystem flush at managed mount retirement.
 * @param volume Borrowed volume. @return Injected I/O error.
 */
static int fail_teardown_sync(struct storage_volume *volume)
{ (void)volume; return -EIO; }
/** @brief Failed final sync keeps identity and dirty data until retry succeeds.
 * @param ns Live namespace. @param root Actual root. @param root_path Owned root path.
 */
static void test_teardown_failure(struct vfs_namespace *ns,const struct storage_node *root,struct vfs_path *root_path)
{
    (void)create_member(root,"retry.bin");
    struct vfs_lookup_result held={0}; struct storage_node item,again;
    uint64_t frame=map_shared(ns,"/retry.bin",&held,&item);
    ((uint8_t *)(uintptr_t)frame)[4]=0x59; page_cache_release(frame);
    const struct storage_filesystem_ops *original=g_storage.fs_ops;
    struct storage_filesystem_ops failing=*original; failing.sync=fail_teardown_sync;
    g_storage.fs_ops=&failing; vfs_path_put(&held.path); vfs_path_put(root_path);
    assert(!vfs_namespace_destroy(ns));
    assert(!storage_backend_lookup(&g_volumes[0],root,"retry.bin",&again));
    assert(again.inode_generation==item.inode_generation);
    uint64_t found=0; assert(!page_cache_lookup(&again,0,&found) && found==frame);
    assert(((uint8_t *)(uintptr_t)found)[4]==0x59); page_cache_release(found);
    g_storage.fs_ops=original; assert(vfs_reclaim_mounts(1)==1 && !page_cache_owns(frame));
}
/** @brief Run one clustered filesystem lifecycle scenario on a private image.
 * @param argc Argument count. @param argv Image, filesystem and scenario.
 * @return Zero after all production-path assertions hold.
 */
int main(int argc,char **argv)
{
    assert(argc==4); FILE *file=fopen(argv[1],"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long size=ftell(file); assert(size>0); rewind(file);
    g_active_volume=&g_volumes[0]; *g_active_volume=(struct storage_volume){.volume_id=0,
        .esp_sector_count=size/512,.exfat_sector_count=size/512};
    uint8_t *image=malloc((size_t)size); assert(image && fread(image,1,(size_t)size,file)==(size_t)size);
    assert(!fclose(file)); test_memory_bind(g_active_volume,image,(uint64_t)size);
    if(!strcmp(argv[2],"fat")) { assert(!fat32_mount()); g_storage.fs_ops=&storage_fat_ops; }
    else { assert(!strcmp(argv[2],"exfat") && !exfat_mount()); g_storage.fs_ops=&storage_exfat_ops; }
    g_storage.ready=true; storage_cache_invalidate(); page_cache_init();
    if(!strcmp(argv[3],"purge")) { test_purge(!strcmp(argv[2],"fat") ? STORAGE_NODE_FLAG_FAT32 : STORAGE_NODE_FLAG_EXFAT); return 0; }
    if(!strncmp(argv[3],"teardown",8)) g_storage.vfs_managed=true;
    struct vfs_namespace ns={0}; struct vfs_path root_path={0};
    assert(!mount_fixture_begin(&ns,&g_volumes[0],&root_path));
    struct storage_node root=*vfs_path_node(&root_path);
    if(!strcmp(argv[3],"rename")) test_rename(&ns,&root);
    else if(!strcmp(argv[3],"eviction")) test_eviction(&ns,&root);
    else if(!strcmp(argv[3],"delete")) test_delete(&ns,&root,false);
    else if(!strcmp(argv[3],"held-delete")) test_delete(&ns,&root,true);
    else if(!strcmp(argv[3],"replace")) test_replace(&ns,&root,false);
    else if(!strcmp(argv[3],"held-replace")) test_replace(&ns,&root,true);
    else if(!strcmp(argv[3],"teardown")) test_teardown(&ns,&root,&root_path);
    else if(!strcmp(argv[3],"teardown-fail")) test_teardown_failure(&ns,&root,&root_path);
    else if(!strcmp(argv[3],"growth")) test_growth(&ns,&root);
    else abort();
    printf("PASS clustered file cache: %s %s\n",argv[2],argv[3]); return 0;
}
