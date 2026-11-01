/* Real ext native fallocate, provider and page cache against a memory disk. */
#define RELIEFNT_FIXTURE_REAL_PAGE_CACHE
#define RELIEFNT_FIXTURE_WRAP_NATIVE_INODE_READ
#define main storage_fixture_unused_main
#include "storage_mkdir_mount_test.c"
#undef main
#include "../../mm/user/fault.c"

static bool fail_native_read_once;
static uint64_t published_phys;
/** @brief Fail only the adapter's next direct read, leaving real provider reads intact.
 * @param volume Borrowed ext instance. @param ino Native inode number.
 * @param out Required inode output. @return Zero or an injected/native errno.
 */
int fixture_native_inode_read(struct storage_volume *volume,uint64_t ino,struct ext4_inode_view *out)
{
    if (fail_native_read_once) { fail_native_read_once=false; return -ENOMEM; }
    return storage_ext4_read_inode(volume,ino,out);
}
/** @brief Capture a real file fault's page-table publication.
 * @param as Borrowed address space. @param va Virtual page address.
 * @param phys Owned physical frame. @param flags Mapping permissions.
 * @return True after recording the published frame.
 */
bool address_space_map_user_page(struct address_space *as,uint64_t va,uint64_t phys,uint64_t flags)
{ (void)as; (void)va; (void)flags; published_phys=phys; return true; }
/** @brief Translate a fixture provider error without changing it.
 * @param ret Provider result. @return The unchanged result.
 */
int storage_errno(int ret) { return ret; }

/** @brief Initialize the fixture cache lock.
 * @param lock Exclusively owned lock.
 */
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state=0; }
/** @brief Reject unexpected tmpfs retention in the ext-only fixture.
 * @param phys Unused tmpfs frame.
 */
void mm_retain_page(uint64_t phys) { (void)phys; abort(); }

/** @brief Borrowed real-provider page context with controlled loader failure. */
struct ext_page_request { struct storage_volume *volume; struct ext4_inode_view *inode; uint64_t offset; bool fail; };
/** @brief Load one complete page from the real ext provider.
 * @param phys Cache-owned destination. @param opaque Borrowed provider and offset.
 * @return Zero or provider error.
 */
static int ext_page_loader(uint64_t phys,void *opaque)
{
    struct ext_page_request *request=opaque; uint32_t done=0;
    memset((void *)(uintptr_t)phys,0,4096);
    if (request->fail) return -EIO;
    uint32_t expected=request->inode->size-request->offset < 4096 ?
                      (uint32_t)(request->inode->size-request->offset) : 4096;
    int rc=storage_ext4_read_file_range(request->volume,12,request->inode,request->offset,
                                        (void *)(uintptr_t)phys,4096,&done);
    return rc ? rc : done==expected ? 0 : -5;
}
/** @brief A late fallocate failure must expose its committed file-size growth.
 * @param volume Mounted real ext instance. @param node Borrowed initial file key.
 * @param inode Caller-owned inode view. @param request Real page loader state.
 * @param first Pinned resident page. @param second Second pinned page.
 * @param dirty Preserve a modified byte inside the old EOF when true.
 * @param fail_loader Inject an intermediate suffix-loader failure when true.
 * @return None; verifies the real provider, shared page and returned errno.
 */
static void test_partial_growth(struct storage_volume *volume,struct storage_node *node,
                                struct ext4_inode_view *inode,struct ext_page_request *request,
                                uint64_t first,uint64_t second,bool dirty,bool fail_loader)
{
    if (dirty) { page_cache_mark_shared_writable(first); ((uint8_t *)(uintptr_t)first)[3]=0x73; }
    assert(!storage_truncate_held_node(node,1000));
    ((uint8_t *)(uintptr_t)first)[1500]=0x7c; /* Prior out-of-EOF padding. */
    struct test_memory_disk *disk=test_memory_disk(volume); disk->write_failure_lba=0;
    int rc=ext_native_fallocate(volume,node,0,0,20000);
    disk->write_failure_lba=UINT64_MAX;
    assert(rc==-EIO);
    assert(!storage_ext4_read_inode(volume,12,inode) && inode->size==20000);
    uint8_t backing=0xff; uint32_t read=0;
    assert(!storage_ext4_read_file_range(volume,12,inode,1500,&backing,1,&read) && read==1 && !backing);
    struct storage_node fresh; storage_ext4_node_from_inode(volume,12,inode,&fresh);
    request->inode=inode; request->offset=0; uint64_t alias=0;
    if (fail_loader) {
        request->fail=true;
        assert(page_cache_load(&fresh,0,ext_page_loader,request,&alias)<0 && !alias);
        assert(((uint8_t *)(uintptr_t)first)[3]==(dirty ? 0x73 : 0x5a) &&
               ((uint8_t *)(uintptr_t)first)[1500]==0x7c);
        request->fail=false;
    }
    assert(!page_cache_load(&fresh,0,ext_page_loader,request,&alias) && alias==first);
    assert(((uint8_t *)(uintptr_t)alias)[3]==(dirty ? 0x73 : 0x5a) &&
           ((uint8_t *)(uintptr_t)alias)[1500]==backing);
    assert(node->size==inode->size);
    page_cache_release(alias); page_cache_release(first); page_cache_release(second);
    printf("PASS ext partially committed growth: %s%s\n",dirty ? "dirty" : "clean",fail_loader ? " loader-failure" : "");
}
/** @brief A fresh native stat or lookup recovers growth after the reread fails.
 * @param volume Mounted real ext instance. @param node Initial inode key.
 * @param inode Caller-owned inode view. @param first First pinned cache frame.
 * @param second Second pinned cache frame. @param lookup Use native lookup when true;
 * otherwise use the same retained-path stat performed for a new mmap.
 * @return None; exercises real ext provider, trusted metadata and file fault.
 */
static void test_independent_observation(struct storage_volume *volume,struct storage_node *node,
                                         struct ext4_inode_view *inode,uint64_t first,uint64_t second,bool lookup)
{
    struct vfs_namespace ns={0}; struct vfs_path root={0}; struct vfs_lookup_result held={0};
    assert(!mount_fixture_begin(&ns,volume,&root));
    if (!lookup) assert(!vfs_kernel_lookup(&ns,"/payload",0,&held));
    page_cache_mark_shared_writable(first); ((uint8_t *)(uintptr_t)first)[3]=0x73;
    assert(!storage_truncate_held_node(node,1000));
    ((uint8_t *)(uintptr_t)first)[1500]=0x7c;
    struct test_memory_disk *disk=test_memory_disk(volume); disk->write_failure_lba=0;
    fail_native_read_once=true; assert(ext_native_fallocate(volume,node,0,0,20000)==-EIO);
    assert(!fail_native_read_once && node->size==1000);
    disk->write_failure_lba=UINT64_MAX;
    assert(!storage_ext4_read_inode(volume,12,inode) && inode->size==20000);
    uint8_t backing=0xff; uint32_t read=0;
    assert(!storage_ext4_read_file_range(volume,12,inode,1500,&backing,1,&read) && read==1 && !backing);
    struct storage_node bad=*node;
    bad.inode_generation=inode->generation==UINT32_MAX ? 1 : inode->generation+1;
    struct linux_stat_abi rejected;
    assert(ext_native_stat(volume,&bad,&rejected)==-116);
    struct storage_node untrusted=*node; untrusted.size=inode->size; uint64_t stale=0;
    assert(!page_cache_lookup(&untrusted,0,&stale) && stale==first);
    assert(((uint8_t *)(uintptr_t)stale)[1500]==0x7c); page_cache_release(stale);
    struct storage_node fresh;
    if (lookup) {
        assert(!ext_native_lookup(volume,vfs_path_node(&root),"payload",&fresh) && fresh.size==20000);
    } else {
        struct linux_stat_abi status;
        assert(!vfs_path_stat(&held.path,&status) && status.st_size==20000);
        fresh=*vfs_path_node(&held.path); fresh.size=(uint64_t)status.st_size;
    }
    uint64_t found=0; assert(page_cache_lookup(&fresh,0,&found)<0);
    if (lookup) assert(!vfs_kernel_lookup(&ns,"/payload",0,&held));
    struct task task={0}; struct task_file file={.used=1,.path_ref=held.path};
    struct task_vma vma={.used=1,.prot=LINUX_PROT_READ,
        .flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_MMAP|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_SHARED_FILE,
        .start=0x5000000,.end=0x5001000,.file_limit=fresh.size,.file_node=fresh,.mapping_file=&file};
    published_phys=0; assert(!task_map_file_vma_page(&task,&vma,vma.start) && published_phys==first);
    assert(((uint8_t *)(uintptr_t)first)[3]==0x73 && ((uint8_t *)(uintptr_t)first)[1500]==backing);
    page_cache_release(published_phys); page_cache_release(first); page_cache_release(second);
    vfs_path_put(&held.path); vfs_path_put(&root); assert(!vfs_namespace_destroy(&ns));
    printf("PASS ext independent %s and actual file fault after failed reread\n",lookup ? "lookup" : "stat");
}
/** @brief Exercise real ext provider progress, size transitions and cache failure.
 * @param argc Argument count. @param argv Image and selected scenario.
 * @return Zero after actual provider, disk and page-cache assertions.
 */
int main(int argc,char **argv)
{
    assert(argc==3); FILE *file=fopen(argv[1],"rb"); assert(file);
    assert(!fseek(file,0,SEEK_END)); long size=ftell(file); assert(size>0); rewind(file);
    struct storage_volume *volume=&g_volumes[0];
    *volume=(struct storage_volume){.volume_id=0,.ext_sector_count=size/512};
    uint8_t *image=malloc((size_t)size); assert(image && fread(image,1,(size_t)size,file)==(size_t)size);
    assert(!fclose(file)); test_memory_bind(volume,image,(uint64_t)size);
    assert(!storage_ext4_mount(volume)); volume->fs_ops=&storage_ext_ops; volume->ready=true;
    g_active_volume=volume; page_cache_init();
    struct ext4_inode_view inode; assert(!storage_ext4_read_inode(volume,12,&inode) && inode.size==8192);
    struct storage_node node; storage_ext4_node_from_inode(volume,12,&inode,&node);
    struct storage_ext4_map_result map0,map1;
    assert(!storage_ext4_map_block(volume,12,&inode,0,false,&map0) && !map0.hole && !map0.unwritten);
    assert(!storage_ext4_map_block(volume,12,&inode,1,false,&map1) && !map1.hole && !map1.unwritten);
    struct ext_page_request request={.volume=volume,.inode=&inode}; uint64_t first,second;
    assert(!page_cache_load(&node,0,ext_page_loader,&request,&first)); request.offset=4096;
    assert(!page_cache_load(&node,4096,ext_page_loader,&request,&second));
    bool dirty=strstr(argv[2],"dirty")!=NULL;
    if(dirty) {
        page_cache_mark_shared_writable(first); page_cache_mark_shared_writable(second);
        ((uint8_t *)(uintptr_t)first)[3]=0x73; ((uint8_t *)(uintptr_t)second)[1500]=0x74;
    }
    struct test_memory_disk *disk=test_memory_disk(volume);
    if(!strcmp(argv[2],"growth-dirty-independent-stat") || !strcmp(argv[2],"growth-dirty-independent-lookup")) {
        test_independent_observation(volume,&node,&inode,first,second,strstr(argv[2],"lookup")!=NULL);
        return 0;
    }
    if(!strncmp(argv[2],"growth-",7)) {
        test_partial_growth(volume,&node,&inode,&request,first,second,dirty,strstr(argv[2],"loader")!=NULL);
        return 0;
    }
    if(!strcmp(argv[2],"resize")) {
        page_cache_mark_shared_writable(first); ((uint8_t *)(uintptr_t)first)[3]=0x73;
        assert(!storage_truncate_held_node(&node,1000));
        assert(((uint8_t *)(uintptr_t)first)[3]==0x73 && !((uint8_t *)(uintptr_t)first)[1500]);
        uint8_t byte=0x6a; uint32_t written=0;
        assert(!storage_write_held_node(&node,1500,&byte,1,&written) && written==1);
        assert(!storage_ext4_read_inode(volume,12,&inode) && inode.size==1501);
        node.size=inode.size; request.offset=0; uint64_t alias=0;
        assert(!page_cache_load(&node,0,ext_page_loader,&request,&alias) && alias==first);
        assert(((uint8_t *)(uintptr_t)alias)[3]==0x73 && ((uint8_t *)(uintptr_t)alias)[1500]==0x6a);
        page_cache_release(alias); page_cache_release(first); page_cache_release(second);
        puts("PASS ext same-page shrink and regrowth"); return 0;
    }
    if(!strcmp(argv[2],"aligned")) {
        assert(!ext_native_fallocate(volume,&node,3,0,4096));
        assert(!((uint8_t *)(uintptr_t)first)[0] && !((uint8_t *)(uintptr_t)first)[4095]);
        assert(((uint8_t *)(uintptr_t)second)[0]==0x5a);
        page_cache_mark_shared_writable(first); ((uint8_t *)(uintptr_t)first)[24]=0x6b;
        assert(!ext_native_fallocate(volume,&node,16,20,50));
        assert(!((uint8_t *)(uintptr_t)first)[24]);
        page_cache_release(first); page_cache_release(second);
        puts("PASS ext completed aligned punch and hole zero"); return 0;
    }
    disk->write_failure_lba=volume->ext_start_lba+map1.physical*8;
    uint32_t mode=strstr(argv[2],"zero") ? 16 : 3;
    assert(ext_native_fallocate(volume,&node,mode,20,5000)==-EIO);
    assert(image[map0.physical*4096+20]==0 && image[map1.physical*4096]==0x5a);
    uint8_t *a=(uint8_t *)(uintptr_t)first,*b=(uint8_t *)(uintptr_t)second;
    assert(a[3]==(dirty ? 0x73 : 0x5a) && a[19]==0x5a && a[20]==0 && a[4095]==0);
    assert(b[0]==0x5a && b[923]==0x5a && b[1500]==(dirty ? 0x74 : 0x5a));
    page_cache_release(first); page_cache_release(second);
    if(!dirty) {
        assert(page_cache_reclaim(2)==2); disk->write_failure_lba=UINT64_MAX;
        request.offset=0; assert(!page_cache_load(&node,0,ext_page_loader,&request,&first));
        assert(((uint8_t *)(uintptr_t)first)[20]==0); page_cache_release(first);
    }
    printf("PASS ext partial mutation: %s\n",argv[2]); return 0;
}
