/* Exercise the real ext native operation and page cache with a deterministic
 * synchronous provider boundary, including provider and metadata failures. */
#define main cache_fixture_main
#include "memory_reclaim_test.c"
#undef main
#include "../../fs/ext_common/native.c"

static uint64_t disk_size;
static uint64_t error_committed_size;
static uint32_t inode_read_generation;
static int operation_error, inode_error, destroy_error, operations, invalidations, inode_reads;
static uint64_t probe_phys;
static unsigned char probe_byte;

int storage_ext4_check_node(struct storage_volume *volume,const struct storage_node *item,
                            struct ext4_inode_view *in)
{
    (void)volume; (void)item; *in=(struct ext4_inode_view){.mode=0100000,.size=disk_size}; return 0;
}
/** @brief Model provider progress and a separately reported operation error.
 * @param volume Borrowed fixture volume. @param ino Expected inode number.
 * @param mode Requested mode. @param offset First byte. @param len Byte count.
 * @param observe Synchronous zero-range observer. @param context Observer state.
 * @return Zero or injected provider errno.
 */
int storage_ext4_fallocate_observed(struct storage_volume *volume,uint64_t ino,uint32_t mode,uint64_t offset,uint64_t len,
                                    storage_ext4_zero_observer observe,void *context)
{
    (void)volume; assert(ino==node.first_cluster && irq_flags==(1ULL<<9)); ++operations;
    if (probe_phys) assert(*(unsigned char *)(uintptr_t)probe_phys==probe_byte);
    if (operation_error) { if (error_committed_size) disk_size=error_committed_size; return operation_error; }
    if (mode & (2u | 16u)) observe(context,offset,len);
    if (!(mode&1u) && disk_size<offset+len) disk_size=offset+len;
    return 0;
}
/** @brief Read modeled metadata or return an independently injected error.
 * @param volume Borrowed fixture volume. @param ino Expected inode number.
 * @param in Required inode output. @return Zero or injected metadata errno.
 */
int storage_ext4_read_inode(struct storage_volume *volume,uint64_t ino,struct ext4_inode_view *in)
{
    (void)volume; assert(ino==node.first_cluster); ++inode_reads; if (inode_error) return inode_error;
    *in=(struct ext4_inode_view){.mode=0100000,.size=disk_size,.generation=inode_read_generation}; return 0;
}
int storage_ext4_destroy_inode(struct storage_volume *volume,uint64_t ino)
{ (void)volume; assert(ino==node.first_cluster); return destroy_error; }
void storage_cache_invalidate(void) { ++invalidations; }
/** @brief Supply the stat adapter's synthetic fixture device number.
 * @param volume Borrowed fixture volume. @return A stable synthetic device.
 */
uint64_t storage_volume_device_number(const struct storage_volume *volume)
{ (void)volume; return 0; }
/** @brief Decode the stat adapter's optional little-endian device number.
 * @param bytes Four borrowed on-disk bytes. @return Decoded integer.
 */
uint32_t ext4_get_le32(const uint8_t *bytes)
{ return (uint32_t)bytes[0] | (uint32_t)bytes[1]<<8 | (uint32_t)bytes[2]<<16 | (uint32_t)bytes[3]<<24; }

/** @brief A successful size-growing preallocation preserves old dirty aliases.
 * @return None.
 */
static void test_growth(void)
{
    struct storage_volume volume={0}; node.size=disk_size=4096;
    uint64_t old=load(0); page_cache_mark_shared_writable(old);
    *(unsigned char *)(uintptr_t)old=0x73; page_cache_release(old);
    struct storage_node previous=node;
    probe_phys=old; probe_byte=0x73;
    assert(!ext_native_fallocate(&volume,&node,0,7000,2000));
    assert(node.size==9000 && operations==1 && invalidations==1);
    uint64_t alias=0; assert(!page_cache_lookup(&node,0,&alias) && alias==old);
    assert(*(unsigned char *)(uintptr_t)alias==0x73); page_cache_release(alias);
    assert(!page_cache_lookup(&previous,0,&alias) && alias==old); page_cache_release(alias);
}
/** @brief Zero or punch commits only the requested bytes of resident pages.
 * @param mode Native punch or zero flag. @param fail_kind Inject operation or later metadata failure.
 * @return None.
 */
static void test_range(uint32_t mode,int fail_kind)
{
    struct storage_volume volume={0}; node.size=disk_size=3*4096;
    uint64_t frames[3];
    for (unsigned i=0;i<3;++i) {
        frames[i]=load(i); page_cache_mark_shared_writable(frames[i]);
        memset((void *)(uintptr_t)frames[i],0x61+(int)i,4096);
        if (i!=1) page_cache_release(frames[i]);
    }
    probe_phys=frames[0]; probe_byte=0x61;
    if (fail_kind==1) operation_error=-5;
    if (fail_kind==2) inode_error=-5;
    int rc=ext_native_fallocate(&volume,&node,mode,123,7000);
    assert(rc==(fail_kind ? -5 : 0)); assert(operations==1);
    for (unsigned i=0;i<3;++i) {
        unsigned char *p=(void *)(uintptr_t)frames[i];
        for (unsigned j=0;j<4096;++j) {
            unsigned pos=i*4096+j;
            unsigned char want=fail_kind==1 ? (unsigned char)(0x61+i) :
                (pos>=123 && pos<7123 ? 0 : (unsigned char)(0x61+i));
            assert(p[j]==want);
        }
    }
    page_cache_release(frames[1]);
}
/** @brief Terminal deletion hides the old key while an active frame stays pinned.
 * @return None.
 */
static void test_terminal(void)
{
    struct storage_volume volume={0}; node.size=disk_size=4096;
    uint64_t old=load(0); page_cache_mark_shared_writable(old);
    *(unsigned char *)(uintptr_t)old=0x7c;
    assert(!ext_native_put_inode(&volume,&node,true));
    uint64_t alias=0; assert(page_cache_lookup(&node,0,&alias));
    assert(page_cache_owns(old) && *(unsigned char *)(uintptr_t)old==0x7c);
    uint64_t replacement=load(0); assert(replacement!=old);
    assert(*(unsigned char *)(uintptr_t)replacement==0);
    page_cache_release(old); assert(!page_cache_owns(old));
    check_page(0,replacement,0); page_cache_release(replacement);
    page_cache_invalidate_node(&node); assert(!atomic_load(&live_pages));
}
/** @brief A failed terminal destroy leaves the old key and frame accessible.
 * @return None.
 */
static void test_terminal_failure(void)
{
    struct storage_volume volume={0}; node.size=disk_size=4096;
    uint64_t old=load(0); page_cache_mark_shared_writable(old);
    *(unsigned char *)(uintptr_t)old=0x7c; page_cache_release(old);
    destroy_error=-5; assert(ext_native_put_inode(&volume,&node,true)==-5);
    check_page(0,old,0x7c);
}
/** @brief Pinned size snapshots share writes and a committed range update.
 * @return None.
 */
static void test_size_aliases(void)
{
    struct storage_volume volume={0}; node.size=disk_size=8192;
    uint64_t old=load(1);
    node.size=disk_size=12288;
    uint64_t newer=load(1); assert(old==newer);
    page_cache_mark_shared_writable(old); memset((void *)(uintptr_t)old,0x77,4096);
    page_cache_mark_shared_writable(newer); ((unsigned char *)(uintptr_t)newer)[100]=0x88;
    assert(!ext_native_fallocate(&volume,&node,16,4096+10,50));
    unsigned char *a=(void *)(uintptr_t)old, *b=(void *)(uintptr_t)newer;
    for (unsigned i=0;i<4096;++i) {
        assert(a[i]==(i>=10 && i<60 ? 0 : i==100 ? 0x88 : 0x77));
        assert(b[i]==a[i]);
    }
    page_cache_release(old); page_cache_release(newer);
}
/** @brief Recover only a readable committed size after a provider error.
 * @param fail_read Fail the subsequent inode read when true.
 * @param committed Simulate a provider size commit before its error.
 * @param mode Plain allocation or ZERO_RANGE without KEEP_SIZE.
 * @return None; provider errno and all previously valid dirty bytes survive.
 */
static void test_error_growth(bool fail_read,bool committed,uint32_t mode)
{
    struct storage_volume volume={0}; node.size=disk_size=2000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[3]=0x73;
    page_cache_resize_node(&node,2000,1000); node.size=disk_size=1000;
    ((uint8_t *)(uintptr_t)frame)[1500]=0x7c;
    operation_error=-5; error_committed_size=committed ? 2000 : 0; inode_error=fail_read ? -12 : 0;
    assert(ext_native_fallocate(&volume,&node,mode,1000,1000)==-5 && inode_reads==1);
    assert(invalidations==(fail_read ? 0 : 1));
    assert(((uint8_t *)(uintptr_t)frame)[3]==0x73);
    if (fail_read || !committed) {
        assert(node.size==1000 && ((uint8_t *)(uintptr_t)frame)[1500]==0x7c);
    } else {
        assert(node.size==2000);
        struct request request={.offset=0}; uint64_t alias=0;
        assert(!page_cache_load(&node,0,loader,&request,&alias) && alias==frame);
        assert(((uint8_t *)(uintptr_t)alias)[3]==0x73 && !((uint8_t *)(uintptr_t)alias)[1500]);
        page_cache_release(alias);
    }
    page_cache_release(frame);
}
/** @brief A successful growth remains reconciled if its later inode read fails.
 * @return None; returns metadata errno without undoing the provider's growth.
 */
static void test_growth_stat_failure(void)
{
    struct storage_volume volume={0}; node.size=disk_size=2000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[3]=0x73;
    page_cache_resize_node(&node,2000,1000); node.size=disk_size=1000;
    ((uint8_t *)(uintptr_t)frame)[1500]=0x7c; inode_error=-12;
    assert(ext_native_fallocate(&volume,&node,0,1000,1000)==-12);
    assert(node.size==1000 && disk_size==2000 && inode_reads==1 && invalidations==1);
    struct storage_node fresh=node; fresh.size=2000;
    struct request request={.offset=0}; uint64_t alias=0;
    assert(!page_cache_load(&fresh,0,loader,&request,&alias) && alias==frame);
    assert(((uint8_t *)(uintptr_t)alias)[3]==0x73 && !((uint8_t *)(uintptr_t)alias)[1500]);
    page_cache_release(alias); page_cache_release(frame);
}
/** @brief A growing operation's inconsistent smaller reread cannot truncate data.
 * @return None; original error and previously valid dirty bytes are retained.
 */
static void test_error_growth_smaller(void)
{
    struct storage_volume volume={0}; node.size=disk_size=1000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[700]=0x6c;
    operation_error=-5; error_committed_size=500;
    assert(ext_native_fallocate(&volume,&node,0,1000,1000)==-5 && inode_reads==1);
    assert(node.size==1000 && !invalidations && ((uint8_t *)(uintptr_t)frame)[700]==0x6c);
    page_cache_release(frame);
}
/** @brief A reread of another inode generation cannot replace the live key's size.
 * @return None; original errno and old resident bytes remain available.
 */
static void test_error_growth_generation(void)
{
    struct storage_volume volume={0}; node.size=disk_size=1000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[700]=0x6c;
    operation_error=-5; error_committed_size=2000; inode_read_generation=1;
    assert(ext_native_fallocate(&volume,&node,0,1000,1000)==-5 && inode_reads==1);
    assert(node.size==1000 && !invalidations && ((uint8_t *)(uintptr_t)frame)[700]==0x6c);
    page_cache_release(frame);
}
/** @brief A later successful reread repairs a cache cap left by an earlier error.
 * @return None; the known inode size is published even without new growth.
 */
static void test_error_growth_known_size(void)
{
    struct storage_volume volume={0}; node.size=disk_size=1000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[3]=0x73;
    page_cache_resize_node(&node,2000,1000); ((uint8_t *)(uintptr_t)frame)[1500]=0x7c;
    node.size=disk_size=2000; operation_error=-5;
    assert(ext_native_fallocate(&volume,&node,0,2000,1000)==-5 && inode_reads==1 && invalidations==1);
    struct request request={.offset=0}; uint64_t alias=0;
    assert(!page_cache_load(&node,0,loader,&request,&alias) && alias==frame);
    assert(((uint8_t *)(uintptr_t)alias)[3]==0x73 && !((uint8_t *)(uintptr_t)alias)[1500]);
    page_cache_release(alias); page_cache_release(frame);
}
/** @brief Independent checked stat repairs an earlier unreadable growth error.
 * @return None; no second allocation operation is performed.
 */
static void test_error_growth_independent_stat(void)
{
    struct storage_volume volume={0}; node.size=disk_size=2000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[3]=0x73;
    page_cache_resize_node(&node,2000,1000); node.size=disk_size=1000;
    ((uint8_t *)(uintptr_t)frame)[1500]=0x7c;
    operation_error=-5; error_committed_size=2000; inode_error=-12;
    assert(ext_native_fallocate(&volume,&node,0,1000,1000)==-5 && inode_reads==1);
    assert(node.size==1000 && disk_size==2000 && operations==1);
    inode_error=0; struct linux_stat_abi status;
    assert(!ext_native_stat(&volume,&node,&status) && status.st_size==2000);
    struct storage_node fresh=node; fresh.size=(uint64_t)status.st_size;
    struct request request={.offset=0}; uint64_t alias=0; unsigned before=atomic_load(&loads);
    assert(!page_cache_load(&fresh,0,loader,&request,&alias) && alias==frame);
    assert(atomic_load(&loads)==before+1 && operations==1);
    assert(((uint8_t *)(uintptr_t)alias)[3]==0x73 && !((uint8_t *)(uintptr_t)alias)[1500]);
    page_cache_release(alias); page_cache_release(frame);
}
/** @brief Trusted current stat cannot let a stale pre-shrink VMA regrow data.
 * @return None; current-size observation clears the newly invalid tail.
 */
static void test_independent_stat_shrink(void)
{
    struct storage_volume volume={0}; node.size=disk_size=2000;
    uint64_t frame=load(0); page_cache_mark_shared_writable(frame);
    ((uint8_t *)(uintptr_t)frame)[3]=0x73; ((uint8_t *)(uintptr_t)frame)[1500]=0x7c;
    disk_size=1000; struct linux_stat_abi status;
    assert(!ext_native_stat(&volume,&node,&status) && status.st_size==1000);
    assert(((uint8_t *)(uintptr_t)frame)[3]==0x73 && !((uint8_t *)(uintptr_t)frame)[1500]);
    struct request request={.offset=0}; uint64_t alias=0; unsigned before=atomic_load(&loads);
    assert(!page_cache_load(&node,0,loader,&request,&alias) && alias==frame);
    assert(atomic_load(&loads)==before && !((uint8_t *)(uintptr_t)alias)[1500]);
    page_cache_release(alias); page_cache_release(frame);
}
/** @brief Run one isolated native adapter/cache behavior case.
 * @param argc Argument count. @param argv Selected scenario.
 * @return Zero when all scenario assertions hold.
 */
int main(int argc,char **argv)
{
    assert(argc==2); page_cache_init(); node.flags=STORAGE_NODE_FLAG_EXT2;
    if (!strcmp(argv[1],"growth")) test_growth();
    else if (!strcmp(argv[1],"zero")) test_range(16,0);
    else if (!strcmp(argv[1],"punch")) test_range(3,0);
    else if (!strcmp(argv[1],"failure")) test_range(16,1);
    else if (!strcmp(argv[1],"stat-failure")) test_range(16,2);
    else if (!strcmp(argv[1],"terminal")) test_terminal();
    else if (!strcmp(argv[1],"terminal-fail")) test_terminal_failure();
    else if (!strcmp(argv[1],"size-aliases")) test_size_aliases();
    else if (!strcmp(argv[1],"error-growth")) test_error_growth(false,true,0);
    else if (!strcmp(argv[1],"error-growth-zero")) test_error_growth(false,true,16);
    else if (!strcmp(argv[1],"error-growth-no-commit")) test_error_growth(false,false,0);
    else if (!strcmp(argv[1],"error-growth-read-fail")) test_error_growth(true,true,0);
    else if (!strcmp(argv[1],"growth-stat-fail")) test_growth_stat_failure();
    else if (!strcmp(argv[1],"error-growth-smaller")) test_error_growth_smaller();
    else if (!strcmp(argv[1],"error-growth-generation")) test_error_growth_generation();
    else if (!strcmp(argv[1],"error-growth-known-size")) test_error_growth_known_size();
    else if (!strcmp(argv[1],"error-growth-independent-stat")) test_error_growth_independent_stat();
    else if (!strcmp(argv[1],"independent-stat-shrink")) test_independent_stat_shrink();
    else abort();
    printf("PASS ext native cache mutation: %s\n",argv[1]); return 0;
}
