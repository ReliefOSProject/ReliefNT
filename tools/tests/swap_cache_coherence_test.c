/* SPDX-License-Identifier: Apache-2.0 */
/* Real ext caches, registry, slot backend and page cache over memory transports. */
#define RELIEFNT_FIXTURE_REAL_PAGE_CACHE
#define RELIEFNT_FIXTURE_REAL_DAC
#define main unused_swap_cache_fixture_main
#include "storage_mkdir_mount_test.c"
#undef main
#include "../../mm/user/fault.c"
#include "../../mm/swap/internal.h"
#include <reliefnt/swap_internal.h>

static uint64_t published_phys, injected_lba;
static int injected_error;
static bool injected_partial=true;

/** @brief Simulate a provider that may change one sector before returning failure.
 * @param context Fixture disk. @param first First device sector.
 * @param count Sector count. @param buffer Current input bytes.
 * @return Zero or injected terminal/provider errno.
 */
static int partial_provider_write(void *context,uint64_t first,uint32_t count,const void *buffer)
{
    struct test_memory_disk *disk=context;
    assert(storage_transport_lock.state && count && first+count<=disk->length/512);
    if (injected_error && first==injected_lba) {
        if (injected_partial) memcpy(disk->bytes+first*512,buffer,512);
        return injected_error;
    }
    return test_memory_write(context,first,count,buffer);
}

/** @brief Capture the physical frame installed by the actual file-fault path.
 * @param as Borrowed MM. @param va User virtual address.
 * @param phys Cache-owned frame. @param flags PTE permissions.
 * @return True after recording the frame.
 */
bool address_space_map_user_page(struct address_space *as,uint64_t va,uint64_t phys,uint64_t flags)
{ (void)as;(void)va;(void)flags;published_phys=phys;return true; }
/** @brief Initialize the actual page-cache lock. @param lock Empty lock. */
void kernel_spin_init(struct kernel_spinlock *lock) { lock->state=0; }
/** @brief Reject unexpected tmpfs frame retention. @param phys Unexpected frame. */
void mm_retain_page(uint64_t phys) { (void)phys;abort(); }
/** @brief Preserve native error values at the fault boundary.
 * @param status Provider result. @return Unchanged errno.
 */
int storage_errno(int status) { return status; }
/** @brief Encode one little-endian v1 header word.
 * @param out Four writable bytes. @param value Word to encode.
 */
static void put_word(uint8_t *out,uint32_t value)
{ out[0]=value;out[1]=value>>8;out[2]=value>>16;out[3]=value>>24; }
/** @brief Read an entire existing data block through the native ext cache.
 * @param volume Mounted instance. @param inode Its decoded file inode.
 * @param page Logical file page. @param out 4096 writable bytes.
 */
static void read_page(struct storage_volume *volume,struct ext4_inode_view *inode,
                      unsigned page,uint8_t *out)
{
    uint32_t done=0;
    assert(!storage_ext4_read_file_range(volume,12,inode,(uint64_t)page*4096,out,4096,&done));
    assert(done==4096);
}

/** @brief Exercise slot writes, error invalidation and post-release file faults.
 * @param argc Argument count. @param argv Fixture ext image path.
 * @return Zero when cached and post-swap data match the actual medium.
 */
int main(int argc,char **argv)
{
    assert(argc==2); FILE *file=fopen(argv[1],"rb");assert(file);
    assert(!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0);rewind(file);
    uint8_t *image=malloc((size_t)size),*other_image=malloc((size_t)size);assert(image && other_image);
    assert(fread(image,1,(size_t)size,file)==(size_t)size && !fclose(file));
    memcpy(other_image,image,(size_t)size);
    struct storage_volume *v=&g_volumes[0],*other=&g_volumes[1],*alias=&g_volumes[2];
    *v=(struct storage_volume){.volume_id=0,.ext_sector_count=(uint64_t)size/512};
    *other=(struct storage_volume){.volume_id=1,.ext_sector_count=(uint64_t)size/512};
    test_memory_bind(v,image,(uint64_t)size);test_memory_bind(other,other_image,(uint64_t)size);
    assert(!storage_ext4_mount(v) && !storage_ext4_mount(other));
    struct test_memory_disk *disk=test_memory_disk(v);
    *alias=(struct storage_volume){.volume_id=2,.ext_sector_count=(uint64_t)size/512,
                                   .kind=v->kind,.transport=v->transport};
    assert(!block_device_get(disk->registration.info.device,&alias->device_ref));
    assert(!storage_ext4_mount(alias));
    v->fs_ops=other->fs_ops=alias->fs_ops=&storage_ext_ops;
    v->ready=other->ready=alias->ready=true;g_active_volume=v;
    static const struct block_device_operations ops={.read=test_memory_read,.write=partial_provider_write};
    disk->registration.info.flags|=BLOCK_DEVICE_SWAP_SAFE;
    disk->registration.operations=&ops; /* Test-only provider setup before slot I/O. */
    struct ext4_inode_view inode,other_inode,alias_inode;
    assert(!storage_ext4_read_inode(v,12,&inode) && inode.size==3*4096);
    assert(!storage_ext4_read_inode(other,12,&other_inode));
    assert(!storage_ext4_read_inode(alias,12,&alias_inode));
    struct storage_ext4_map_result maps[3];
    for (unsigned i=0;i<3;++i)
        assert(!storage_ext4_map_block(v,12,&inode,i,false,&maps[i]) && !maps[i].hole && !maps[i].unwritten);
    uint8_t before[4096],other_before[4096],after[4096];
    read_page(v,&inode,1,before);read_page(other,&other_inode,1,other_before);
    read_page(alias,&alias_inode,1,after);
    assert(before[0]==0x5a && other_before[0]==0x5a);
    uint64_t lba=v->ext_start_lba+maps[1].physical*8;
    g_active_volume=v;ext2_cache_store(lba,8,before);
    g_active_volume=other;ext2_cache_store(lba,8,other_before);
    g_active_volume=alias;ext2_cache_store(lba,8,before);
    storage_read_cache=(struct storage_sector_cache){.volume=other,.first_lba=lba,.sector_count=8,.valid=1};
    uint8_t *pinned=NULL;
    assert(!storage_ext4_cache_get(v,maps[0].physical,&pinned,true) && pinned);
    struct storage_ext4_cache_entry *pinned_entry=ext4_cache_lookup(&ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK],v,maps[0].physical);
    assert(pinned_entry && pinned_entry->pinned);
    /* A failed command cannot discard same-device pinned or dirty entries. */
    struct swap_storage_token transaction={0};assert(!swap_storage_begin(&transaction));
    injected_lba=v->ext_start_lba+maps[0].physical*8;injected_error=-EIO;injected_partial=false;
    assert(swap_storage_transfer(&transaction,&v->device_ref,injected_lba,8,before,true)==-EIO);
    assert(pinned_entry->valid && pinned_entry->pinned);
    assert(!storage_ext4_cache_mark_dirty(v,maps[0].physical) && pinned_entry->dirty);
    assert(swap_storage_transfer(&transaction,&v->device_ref,injected_lba,8,before,true)==-EIO);
    assert(pinned_entry->valid && pinned_entry->dirty);
    storage_ext4_cache_finish(v,maps[0].physical,pinned);injected_error=0;injected_partial=true;
    swap_storage_end(&transaction);
    struct storage_node node;storage_ext4_node_from_inode(v,12,&inode,&node);
    struct storage_inode_ref *pin=NULL;assert(!storage_inode_get(&node,&pin) && pin);
    assert(!storage_inode_write_get(pin));assert(!storage_inode_swap_claim(pin));
    page_cache_init();assert(!page_cache_has_inode(&node));
    struct block_device_ref ref={0};assert(!block_device_get(disk->registration.info.device,&ref));
    struct swap_backend *backend=NULL;assert(!swap_backend_create(&backend,&ref,0,(uint64_t)size/512));
    for (unsigned i=0;i<3;++i)
        assert(!swap_backend_add_extent(backend,i*8,v->ext_start_lba+maps[i].physical*8,8));
    uint8_t header[4096]={0};put_word(header+1024,1);put_word(header+1028,2);memcpy(header+4086,"SWAPSPACE2",10);
    struct swap_area *area=NULL;assert(!swap_area_create(header,sizeof(header),3,1,&area));
    area->backend=backend;assert(!swap_area_publish(area));
    uint64_t slot=swap_slot_alloc();assert((uint32_t)slot==1);
    uint8_t *page=aligned_alloc(4096,4096);assert(page);memset(page,0x33,4096);
    test_fail_heap_after=0;test_fail_cache_alloc=1;
    assert(!swap_slot_submit_write(slot,(uintptr_t)page));
    test_fail_heap_after=-1;test_fail_cache_alloc=0;
    assert(image[lba*512]==0x33);
    read_page(v,&inode,1,after);
    for (unsigned i=0;i<4096;++i)assert(after[i]==0x33);
    read_page(alias,&alias_inode,1,after);
    for (unsigned i=0;i<4096;++i)assert(after[i]==0x33);
    assert(!page_cache_has_inode(&node)); /* No mapped frame admitted during the claim. */
    g_active_volume=v;assert(!ext2_cache_read(lba,8,after));
    g_active_volume=alias;assert(!ext2_cache_read(lba,8,after));
    g_active_volume=other;assert(ext2_cache_read(lba,8,after) && after[0]==0x5a);
    assert(storage_read_cache.valid && storage_read_cache.volume==other);
    assert(pinned_entry->valid && !pinned_entry->dirty && !pinned_entry->pinned);
    uint64_t hits=other->ext4.read_cache_hits;read_page(other,&other_inode,1,after);
    assert(after[0]==0x5a && other->ext4.read_cache_hits>hits);
    /* An invalidated/empty identity must not collide with unrelated cache. */
    struct storage_volume detached={0};g_active_volume=&detached;ext2_cache_store(lba,8,before);
    storage_read_cache=(struct storage_sector_cache){.volume=NULL,.first_lba=lba,.sector_count=8,.valid=1};
    ext2_cache_invalidate_range(NULL,NULL,lba,8);
    assert(storage_read_cache.valid && ext2_cache_read(lba,8,after));
    ext2_cache_invalidate_range(&detached,NULL,lba,8);
    assert(storage_read_cache.valid && !ext2_cache_read(lba,8,after));
    ext2_cache_reset(&detached);g_active_volume=v;
    read_page(v,&inode,2,before);assert(before[0]==0x5a);
    uint64_t second=swap_slot_alloc();assert((uint32_t)second==2);
    injected_lba=v->ext_start_lba+maps[2].physical*8;injected_error=-EIO;memset(page,0x66,4096);
    assert(swap_slot_submit_write(second,(uintptr_t)page)==-EIO);
    injected_error=0;read_page(v,&inode,2,after);
    for (unsigned i=0;i<4096;++i)assert(after[i]==(i<512 ? 0x66 : 0x5a));
    injected_error=-EAGAIN;memset(page,0x77,4096);
    assert(swap_slot_submit_write(second,(uintptr_t)page)==-EIO);
    injected_error=0;read_page(v,&inode,2,after);
    for (unsigned i=0;i<4096;++i)assert(after[i]==(i<512 ? 0x77 : 0x5a));
    assert(swap_area_unpublish(area)==-EBUSY);swap_slot_put(slot);swap_slot_put(second);
    assert(!swap_area_unpublish(area));storage_inode_swap_release(pin);storage_inode_write_put(pin);
    swap_area_destroy(area);swap_backend_destroy(backend);assert(!storage_inode_put(pin));free(page);
    const uint8_t patch[3]={0xa1,0xb2,0xc3};uint32_t done=0;
    assert(!storage_ext4_write_file_range(v,12,4096+17,patch,sizeof(patch),&done) && done==sizeof(patch));
    assert(!storage_ext4_read_inode(v,12,&inode));read_page(v,&inode,1,after);
    for (unsigned i=0;i<4096;++i)assert(after[i]==(i>=17 && i<20 ? patch[i-17] : 0x33));
    struct vfs_namespace ns={0};struct vfs_path root={0};struct vfs_lookup_result held={0};
    assert(!mount_fixture_begin(&ns,v,&root));assert(!vfs_kernel_lookup(&ns,"/payload",0,&held));
    struct task task={0};struct task_file file_mapping={.used=1,.path_ref=held.path};
    struct task_vma vma={.used=1,.prot=LINUX_PROT_READ,
        .flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_MMAP|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_SHARED_FILE,
        .start=0x5000000,.end=0x5001000,.file_offset=4096,.file_limit=inode.size,
        .file_node=node,.mapping_file=&file_mapping};
    published_phys=0;assert(!task_map_file_vma_page(&task,&vma,vma.start) && published_phys);
    const uint8_t *mapped=(const uint8_t *)(uintptr_t)published_phys;
    for (unsigned i=0;i<4096;++i)assert(mapped[i]==(i>=17 && i<20 ? patch[i-17] : 0x33));
    assert(page_cache_has_inode(&node));page_cache_release(published_phys);page_cache_invalidate_node(&node);
    assert(!page_cache_has_inode(&node));vfs_path_put(&held.path);vfs_path_put(&root);
    assert(!vfs_namespace_destroy(&ns));
    puts("swap slot ext-cache coherence, partial failure, device isolation and post-release fault: ok");
    return 0;
}
