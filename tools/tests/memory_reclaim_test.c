#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/page_cache.h>

/* Weak declarations let the pre-feature binary exercise old behavior and
 * report a missing API as a test failure rather than a linker error. */
extern uint64_t page_cache_reclaim(uint64_t) __attribute__((weak));
extern uint64_t page_cache_reclaimable_pages(void) __attribute__((weak));
extern void page_cache_mark_shared_writable(uint64_t) __attribute__((weak));

#define SLOTS 4096u
static atomic_uint live_pages, live_metadata, allocations, loads;
static atomic_uint page_limit = UINT32_MAX;
static atomic_bool fail_metadata;
static _Thread_local uint64_t irq_flags = 1ULL << 9;
static _Thread_local bool expect_disabled, probe_release;
static pthread_barrier_t entered, proceed;
static struct storage_node node = {.type=RELIEFOS_FS_TYPE_FILE,
    .flags=STORAGE_NODE_FLAG_FAT32, .first_cluster=11, .volume_id=1,
    .size=(SLOTS + 4ULL) * 4096ULL};
struct request { uint64_t offset, phys; bool fail, wait; int result; };

uint32_t smp_current_cpu(void) { return 0; }
void smp_membarrier_poll(void) { }
uint64_t kernel_irq_save(void) { uint64_t old=irq_flags; irq_flags=0; return old; }
void kernel_irq_restore(uint64_t flags) { irq_flags=flags; }
void console_log(enum console_log_level level,const char *format,...)
{ (void)level; (void)format; }
uint64_t mm_alloc_page(void)
{
    assert(irq_flags == (expect_disabled ? 0 : (1ULL << 9)));
    atomic_fetch_add(&allocations, 1);
    unsigned live=atomic_load(&live_pages);
    while (live < atomic_load(&page_limit)) {
        if (atomic_compare_exchange_weak(&live_pages, &live, live+1)) {
            void *p=aligned_alloc(4096,4096); assert(p); return (uintptr_t)p;
        }
    }
    return 0;
}
void mm_free_page(uint64_t phys)
{
    assert(irq_flags == (expect_disabled ? 0 : (1ULL << 9)));
    if (probe_release) {
        probe_release=false; (void)page_cache_reclaimable_pages(); probe_release=true;
    }
    assert(phys && atomic_fetch_sub(&live_pages,1)); free((void *)(uintptr_t)phys);
}
void *mm_phys_to_virt(uint64_t phys)
{ assert(irq_flags == (1ULL << 9)); return (void *)(uintptr_t)phys; }
void *kernel_malloc(size_t size)
{
    assert(irq_flags == (1ULL << 9));
    if (atomic_load(&fail_metadata)) return NULL;
    void *p=calloc(1,size); assert(p); atomic_fetch_add(&live_metadata,1); return p;
}
void kernel_free(void *p)
{
    assert(irq_flags == (1ULL << 9));
    if (p) { assert(atomic_fetch_sub(&live_metadata,1)); free(p); }
}
/** @brief Fill an authoritative disk-file page, with controlled loader failure.
 * @param phys Owned destination page. @param opaque Request with fault controls.
 * @return Zero or a simulated backing failure.
 */
static int loader(uint64_t phys, void *opaque)
{
    struct request *req=opaque;
    assert(irq_flags == (1ULL << 9)); atomic_fetch_add(&loads,1);
    memset((void *)(uintptr_t)phys,(int)(req->offset/4096%251),4096);
    if (req->wait) {
        int rc=pthread_barrier_wait(&entered); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
        rc=pthread_barrier_wait(&proceed); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    }
    return req->fail ? -1 : 0;
}
/** @brief Load a numbered file page through the production cache.
 * @param index File-page index. @return Retained physical page.
 */
static uint64_t load(unsigned index)
{
    struct request req={.offset=(uint64_t)index*4096};
    assert(!page_cache_load(&node,req.offset,loader,&req,&req.phys)); return req.phys;
}
/** @brief Look up a page and verify its retained identity and contents.
 * @param index File-page index. @param expected Expected frame.
 * @param byte Expected first byte. @return None.
 */
static void check_page(unsigned index,uint64_t expected,unsigned char byte)
{
    uint64_t found=0; assert(!page_cache_lookup(&node,(uint64_t)index*4096,&found));
    assert(found==expected && *(unsigned char *)(uintptr_t)found==byte);
    page_cache_release(found);
}
/** @brief Prove PMM exhaustion can commit a clean reclaim before retrying.
 * @param fail_loader Whether the new load subsequently fails. @return None.
 */
static void test_exhaustion(bool fail_loader)
{
    uint64_t old=load(0); page_cache_release(old);
    atomic_store(&page_limit,1);
    struct request req={.offset=4096,.fail=fail_loader};
    int result=page_cache_load(&node,req.offset,loader,&req,&req.phys);
    if (fail_loader) assert(result<0 && !req.phys && !atomic_load(&live_pages));
    else assert(!result && req.phys && *(unsigned char *)(uintptr_t)req.phys==1);
    uint64_t found=0; assert(page_cache_lookup(&node,0,&found));
    if (!fail_loader) { page_cache_release(req.phys); page_cache_invalidate_node(&node); }
    uint64_t reloaded=load(0);
    assert(*(unsigned char *)(uintptr_t)reloaded==0);
    page_cache_release(reloaded); page_cache_invalidate_node(&node);
    assert(!atomic_load(&live_pages));
}
/** @brief A cache hit remains valid without a loader; a miss needs one.
 * @return None; both successful references and failure conserve ownership.
 */
static void test_hit_without_loader(void)
{
    uint64_t phys=load(0), alias=0;
    assert(!page_cache_load(&node,0,NULL,NULL,&alias) && alias==phys);
    page_cache_release(alias); page_cache_release(phys);
    alias=0; assert(page_cache_load(&node,4096,NULL,NULL,&alias)<0 && !alias);
    assert(page_cache_reclaim(1)==1 && !atomic_load(&live_pages));
}
/** @brief Reclaim works with IRQs already disabled and returns outside cache_lock.
 * @return None; provider and metadata allocation counters remain unchanged.
 */
static void test_irq_reclaim(void)
{
    uint64_t first=load(0), second=load(1);
    page_cache_release(first); page_cache_release(second);
    unsigned reads=atomic_load(&loads), metadata=atomic_load(&live_metadata);
    irq_flags=0; expect_disabled=true; probe_release=true;
    assert(page_cache_reclaim(1)==1 && irq_flags==0);
    assert(page_cache_reclaimable_pages()==1 && atomic_load(&loads)==reads &&
           atomic_load(&live_metadata)==metadata);
    probe_release=false; expect_disabled=false; irq_flags=1ULL<<9;
    assert(page_cache_reclaim(2)==1 && !atomic_load(&live_pages));
}
/** @brief Assert an eligible page count and physical reclaim budget.
 * @return None; pinned, dirty and unknown backing are excluded.
 */
static void test_budget(void)
{
    assert(page_cache_reclaim && page_cache_reclaimable_pages && page_cache_mark_shared_writable);
    uint64_t a=load(0), b=load(1), c=load(2);
    page_cache_release(a); page_cache_release(b);
    page_cache_mark_shared_writable(b); *(unsigned char *)(uintptr_t)b=0xab;
    struct storage_node unknown=node; unknown.first_cluster=12; unknown.flags=STORAGE_NODE_FLAG_TMPFS;
    struct request req={.offset=0};
    assert(!page_cache_load(&unknown,0,loader,&req,&req.phys)); page_cache_release(req.phys);
    struct storage_node flagless=unknown; flagless.first_cluster=13; flagless.flags=0;
    struct storage_node mixed=unknown; mixed.first_cluster=14;
    mixed.flags=STORAGE_NODE_FLAG_FAT32 | STORAGE_NODE_FLAG_EXFAT_NOFAT;
    struct storage_node beyond=node; beyond.first_cluster=15; beyond.size=4096;
    struct request req2={.offset=0}, req3={.offset=0}, req4={.offset=4096};
    assert(!page_cache_load(&flagless,0,loader,&req2,&req2.phys)); page_cache_release(req2.phys);
    assert(!page_cache_load(&mixed,0,loader,&req3,&req3.phys)); page_cache_release(req3.phys);
    assert(!page_cache_load(&beyond,4096,loader,&req4,&req4.phys)); page_cache_release(req4.phys);
    assert(page_cache_reclaimable_pages()==1);
    assert(page_cache_reclaim(0)==0 && page_cache_reclaimable_pages()==1);
    assert(page_cache_reclaim(1)==1 && page_cache_reclaimable_pages()==0);
    assert(page_cache_reclaim(20)==0);
    check_page(1,b,0xab); check_page(2,c,2);
    page_cache_release(c); assert(page_cache_reclaimable_pages()==1);
    assert(page_cache_reclaim(4)==1); assert(atomic_load(&live_pages)==5);
    page_cache_invalidate_node(&node); page_cache_invalidate_node(&unknown);
    page_cache_invalidate_node(&flagless); page_cache_invalidate_node(&mixed);
    page_cache_invalidate_node(&beyond);
    check_page(1,b,0xab);
    uint64_t alias=0; assert(!page_cache_lookup(&unknown,0,&alias) && alias==req.phys);
    page_cache_release(alias);
    assert(!page_cache_lookup(&flagless,0,&alias) && alias==req2.phys); page_cache_release(alias);
    assert(!page_cache_lookup(&mixed,0,&alias) && alias==req3.phys); page_cache_release(alias);
    assert(!page_cache_lookup(&beyond,4096,&alias) && alias==req4.phys); page_cache_release(alias);
}
/** @brief Fill fixed cache slots with individually pinned disk pages.
 * @return Array of retained physical pages, owned by the fixture.
 */
static uint64_t *fill(void)
{
    uint64_t *pages=calloc(SLOTS,sizeof(*pages)); assert(pages);
    for (unsigned i=0;i<SLOTS;++i) pages[i]=load(i);
    return pages;
}
/** @brief Reject metadata exhaustion without loading or losing pinned frames.
 * @return None.
 */
static void test_metadata_failure(void)
{
    uint64_t *pages=fill(); unsigned before=atomic_load(&live_pages), calls=atomic_load(&loads);
    atomic_store(&fail_metadata,true);
    struct request req={.offset=(uint64_t)SLOTS*4096};
    assert(page_cache_load(&node,req.offset,loader,&req,&req.phys)==PAGE_CACHE_FULL);
    assert(!req.phys && atomic_load(&loads)==calls && atomic_load(&live_pages)==before);
    check_page(0,pages[0],0); atomic_store(&fail_metadata,false);
    for (unsigned i=0;i<SLOTS;++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node); assert(!atomic_load(&live_pages)); free(pages);
}
/** @brief Preserve a dirty fixed victim during a cache miss.
 * @return None.
 */
static void test_dirty_replacement(void)
{
    assert(page_cache_mark_shared_writable && page_cache_reclaim);
    uint64_t *pages=fill(); page_cache_mark_shared_writable(pages[0]);
    *(unsigned char *)(uintptr_t)pages[0]=0xad; page_cache_release(pages[0]);
    page_cache_release(pages[1]);
    uint64_t newer=load(SLOTS); check_page(0,pages[0],0xad);
    uint64_t missing=0; assert(page_cache_lookup(&node,4096,&missing));
    page_cache_release(newer);
    page_cache_invalidate_node(&node); assert(page_cache_reclaim(UINT64_MAX)==0);
    check_page(0,pages[0],0xad);
    for (unsigned i=2;i<SLOTS;++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node); free(pages);
}
/** @brief A dirty overflow page stays resident on final and extra release.
 * @return None.
 */
static void test_dirty_overflow(void)
{
    assert(page_cache_mark_shared_writable && page_cache_reclaim);
    uint64_t *pages=fill();
    uint64_t extra=load(SLOTS); assert(atomic_load(&live_metadata)>0);
    page_cache_mark_shared_writable(extra); *(unsigned char *)(uintptr_t)extra=0xbe;
    page_cache_release(extra); page_cache_release(extra);
    assert(page_cache_reclaim(UINT64_MAX)==0); check_page(SLOTS,extra,0xbe);
    for (unsigned i=0;i<SLOTS;++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node); check_page(SLOTS,extra,0xbe); free(pages);
}
/** @brief Terminal forget retires pinned fixed and overflow entries independently.
 * @return None; frame and metadata ownership are balanced after the last put.
 */
static void test_retired_overflow(void)
{
    uint64_t *pages=fill(), old=load(SLOTS), alias=0;
    page_cache_mark_shared_writable(old); *(unsigned char *)(uintptr_t)old=0xce;
    page_cache_forget_node(&node);
    assert(page_cache_lookup(&node,0,&alias));
    assert(page_cache_lookup(&node,(uint64_t)SLOTS*4096,&alias));
    assert(page_cache_owns(old) && *(unsigned char *)(uintptr_t)old==0xce);
    uint64_t replacement=load(SLOTS); assert(replacement!=old);
    page_cache_release(old); assert(!page_cache_owns(old));
    page_cache_release(replacement);
    for (unsigned i=0;i<SLOTS;++i) page_cache_release(pages[i]);
    assert(!atomic_load(&live_pages) && !atomic_load(&live_metadata)); free(pages);
}
/** @brief Loader failure must not publish or overwrite a selected old victim.
 * @return None.
 */
static void test_loader_failure(void)
{
    uint64_t *pages=fill(); page_cache_release(pages[0]);
    unsigned before=atomic_load(&live_pages);
    struct request req={.offset=(uint64_t)SLOTS*4096,.fail=true};
    assert(page_cache_load(&node,req.offset,loader,&req,&req.phys)<0);
    assert(!req.phys && atomic_load(&live_pages)==before); check_page(0,pages[0],0);
    for (unsigned i=1;i<SLOTS;++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node); assert(!atomic_load(&live_pages)); free(pages);
}
/** @brief Run a replacement concurrently while its provider is stopped.
 * @param opaque Request with barrier controls. @return NULL.
 */
static void *load_thread(void *opaque)
{
    struct request *req=opaque;
    req->result=page_cache_load(&node,req->offset,loader,req,&req->phys); return NULL;
}
/** @brief Hold a lookup pin across a concurrent pressure and invalidation window.
 * @param opaque Expected frame pointer. @return NULL after releasing the pin.
 */
static void *pin_thread(void *opaque)
{
    uint64_t expected=*(uint64_t *)opaque, pin=0;
    assert(!page_cache_lookup(&node,0,&pin) && pin==expected);
    int rc=pthread_barrier_wait(&entered); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    rc=pthread_barrier_wait(&proceed); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(*(unsigned char *)(uintptr_t)pin==0); page_cache_release(pin); return NULL;
}
/** @brief A concurrent pin excludes pressure and live invalidation atomically.
 * @return None; the released clean frame becomes eligible afterward.
 */
static void test_pressure_pin(void)
{
    uint64_t phys=load(0); page_cache_release(phys);
    assert(!pthread_barrier_init(&entered,NULL,2)); assert(!pthread_barrier_init(&proceed,NULL,2));
    pthread_t thread; assert(!pthread_create(&thread,NULL,pin_thread,&phys));
    int rc=pthread_barrier_wait(&entered); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(page_cache_reclaimable_pages()==0 && page_cache_reclaim(1)==0);
    page_cache_invalidate_node(&node); check_page(0,phys,0);
    rc=pthread_barrier_wait(&proceed); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(thread,NULL));
    assert(page_cache_reclaimable_pages()==1 && page_cache_reclaim(1)==1);
    assert(!atomic_load(&live_pages));
    assert(!pthread_barrier_destroy(&entered)); assert(!pthread_barrier_destroy(&proceed));
}
/** @brief A pin acquired during a load excludes that victim at publication.
 * @return None.
 */
static void test_concurrent_pin(bool fail_growth)
{
    uint64_t *pages=fill(); page_cache_release(pages[0]);
    assert(!pthread_barrier_init(&entered,NULL,2)); assert(!pthread_barrier_init(&proceed,NULL,2));
    struct request req={.offset=(uint64_t)SLOTS*4096,.wait=true}; pthread_t t;
    assert(!pthread_create(&t,NULL,load_thread,&req));
    int rc=pthread_barrier_wait(&entered); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    uint64_t pin=0; assert(!page_cache_lookup(&node,0,&pin) && pin==pages[0]);
    page_cache_invalidate_node(&node);
    atomic_store(&fail_metadata,fail_growth);
    rc=pthread_barrier_wait(&proceed); assert(!rc || rc==PTHREAD_BARRIER_SERIAL_THREAD);
    assert(!pthread_join(t,NULL));
    if (fail_growth) assert(req.result==PAGE_CACHE_FULL && !req.phys);
    else assert(!req.result && req.phys);
    check_page(0,pin,0); page_cache_release(pin); if (req.phys) page_cache_release(req.phys);
    for (unsigned i=1;i<SLOTS;++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node); assert(!atomic_load(&live_pages));
    assert(!pthread_barrier_destroy(&entered)); assert(!pthread_barrier_destroy(&proceed)); free(pages);
}
/** @brief A clustered generation survives mutable fields but separates incarnations.
 * @return None; all temporary frame references are released.
 */
static void test_cluster_identity(void)
{
    node.flags=STORAGE_NODE_FLAG_EXFAT|STORAGE_NODE_FLAG_EXFAT_NOFAT;
    node.inode_generation=57; node.mount_generation=4; node.entry_id=11;
    uint64_t first=load(0),alias=0; page_cache_mark_shared_writable(first);
    ((unsigned char *)(uintptr_t)first)[8]=0x62;
    struct storage_node changed=node; changed.flags=STORAGE_NODE_FLAG_EXFAT;
    changed.first_cluster=99; changed.entry_id=44; changed.size+=4096;
    assert(page_cache_has_inode(&changed));
    assert(!page_cache_lookup(&changed,0,&alias) && alias==first);
    assert(((unsigned char *)(uintptr_t)alias)[8]==0x62); page_cache_release(alias);
    struct storage_node other=changed; other.inode_generation++;
    assert(page_cache_lookup(&other,0,&alias)); other=changed; other.flags=STORAGE_NODE_FLAG_FAT32;
    assert(page_cache_lookup(&other,0,&alias)); other=changed; other.mount_generation++;
    assert(page_cache_lookup(&other,0,&alias)); other=changed; other.inode_generation=0;
    assert(page_cache_lookup(&other,0,&alias));
    page_cache_forget_node(&changed); page_cache_release(first); assert(!atomic_load(&live_pages));
}
/** @brief Load a changed backing page or simulate failure after writing scratch.
 * @param phys Scratch frame. @param opaque Boolean failure control.
 * @return Zero or a simulated provider error.
 */
static int growth_loader(uint64_t phys,void *opaque)
{ memset((void *)(uintptr_t)phys,0x65,4096); return *(bool *)opaque ? -1 : 0; }
/** @brief Failed partial-page refresh preserves frame, dirty bytes and validity.
 * @return None; covers provider failure, PMM exhaustion and later success.
 */
static void test_growth_failure(void)
{
    node.size=1000; struct storage_node smaller=node;
    uint64_t first=load(0),alias=0; page_cache_mark_shared_writable(first);
    ((uint8_t *)(uintptr_t)first)[3]=0x71; node.size=2000;
    bool fail=true;
    assert(page_cache_load(&node,0,growth_loader,&fail,&alias)<0 && !alias);
    assert(((uint8_t *)(uintptr_t)first)[3]==0x71 && !((uint8_t *)(uintptr_t)first)[1500]);
    assert(page_cache_lookup(&node,0,&alias)<0 && !page_cache_lookup(&smaller,0,&alias) && alias==first);
    page_cache_release(alias); alias=0;
    fail=false; atomic_store(&page_limit,1);
    assert(page_cache_load(&node,0,growth_loader,&fail,&alias)<0 && !alias);
    assert(((uint8_t *)(uintptr_t)first)[3]==0x71 && !((uint8_t *)(uintptr_t)first)[1500]);
    atomic_store(&page_limit,UINT32_MAX);
    assert(!page_cache_load(&node,0,growth_loader,&fail,&alias) && alias==first);
    assert(((uint8_t *)(uintptr_t)alias)[3]==0x71 && ((uint8_t *)(uintptr_t)alias)[1500]==0x65);
    page_cache_release(alias); alias=0;
    page_cache_resize_node(&node,2000,1000);
    assert(!((uint8_t *)(uintptr_t)first)[1500] && ((uint8_t *)(uintptr_t)first)[3]==0x71);
    /* A stale old-size snapshot cannot falsely regrow past authoritative EOF. */
    assert(!page_cache_load(&node,0,growth_loader,&fail,&alias) && alias==first);
    assert(!((uint8_t *)(uintptr_t)alias)[1500]); page_cache_release(alias); alias=0;
    page_cache_resize_node(&node,1000,2000);
    assert(page_cache_lookup(&node,0,&alias));
    assert(!page_cache_load(&node,0,growth_loader,&fail,&alias) && alias==first);
    assert(((uint8_t *)(uintptr_t)alias)[3]==0x71 && ((uint8_t *)(uintptr_t)alias)[1500]==0x65);
    page_cache_release(alias); page_cache_forget_node(&node); page_cache_release(first);
    assert(!atomic_load(&live_pages));
}
int main(int argc,char **argv)
{
    assert(argc==2); page_cache_init();
    if (!strcmp(argv[1],"exhaustion")) test_exhaustion(false);
    else if (!strcmp(argv[1],"exhaustion-fail")) test_exhaustion(true);
    else if (!strcmp(argv[1],"hit-no-loader")) test_hit_without_loader();
    else if (!strcmp(argv[1],"irq")) test_irq_reclaim();
    else if (!strcmp(argv[1],"budget")) test_budget();
    else if (!strcmp(argv[1],"metadata")) test_metadata_failure();
    else if (!strcmp(argv[1],"dirty")) test_dirty_replacement();
    else if (!strcmp(argv[1],"overflow")) test_dirty_overflow();
    else if (!strcmp(argv[1],"retired-overflow")) test_retired_overflow();
    else if (!strcmp(argv[1],"loader")) test_loader_failure();
    else if (!strcmp(argv[1],"pin")) test_concurrent_pin(false);
    else if (!strcmp(argv[1],"pin-fail")) test_concurrent_pin(true);
    else if (!strcmp(argv[1],"pressure-pin")) test_pressure_pin();
    else if (!strcmp(argv[1],"cluster-identity")) test_cluster_identity();
    else if (!strcmp(argv[1],"growth-failure")) test_growth_failure();
    else abort();
    printf("PASS cache reclaim: %s\n",argv[1]); return 0;
}
