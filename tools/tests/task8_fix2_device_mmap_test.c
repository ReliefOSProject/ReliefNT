/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Actual character mmap and PCI cache ownership integration.
 * Page tables, VMA operations, PCI lease and device backing are production
 * code; CPU/TLB, MMIO, module and physical memory are host models.
 */
#define TASK8_FIX2_DEVICE_MMAP 1
#define main task8_fix2_inherited_mm_main
#include "mprotect_vma_growth_test.c"
#undef main
#include "../../arch/x86_64/platform/pci.c"
#include "../../kernel/syscall/io/file_io.c"
#include <reliefnt/chrdev.h>

static uint64_t device_base=0xc000000000ULL, expected_cache;
static unsigned provider_releases, provider_calls;
#ifdef TASK8_FIX4_CALLBACK
static void fix4_assert_callback(void);
#endif
#ifdef TASK8_FIX3_REUSED_BACKING
static struct device_mapping *reused_backing;
static uint64_t fix3_page_stride=4096;
#endif
/** @brief Model checked user input in the fixture's explicit mapped interval.
 * @param address User address. @param length Required bytes. @return True if
 * the fixture's user-output admission also accepts the interval.
 */
bool user_range_ok(uint64_t address,uint64_t length)
{ return user_range_writable(address,length); }
/** @brief Trap a RAM refcount query in this DEVICE-only stage.
 * @param physical Unused page. @return Never returns.
 */
uint32_t mm_page_refcount(uint64_t physical) { (void)physical;abort(); }
#ifdef CONFIG_SWAP
/** @brief Model successful preparation of the already-resident DEVICE page.
 * @param task Current task. @param address Admitted byte. @param length Bytes.
 * @param writing Whether the page is an output. @return Zero.
 */
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
#ifdef TASK8_FIX5_FRONT
    (void)address;(void)length;(void)writing;assert(task==current);return 0;
#else
    assert(task==current && address==fix2_device_user_start && length==64 && writing);return 0;
#endif
}
/** @brief Reject unexpected swap faults in the DEVICE-only fixture.
 * @param task Unused task. @param address Unused address. @param error Unused
 * fault bits. @return Never returns.
 */
int swap_resolve_fault(struct task *task,uint64_t address,uint64_t error)
{ (void)task;(void)address;(void)error;abort(); }
/** @brief Reject an unexpected swap-slot retain. @param token Unused slot.
 * @return Never returns.
 */
int swap_slot_get(uint64_t token) { (void)token;abort(); }
/** @brief Reject an unexpected swap-slot release. @param token Unused slot. */
void swap_slot_put(uint64_t token) { (void)token;abort(); }
/** @brief Reject an unexpected file-inode swap query.
 * @param reference Unused inode. @return Never returns.
 */
bool storage_inode_swap_active(const struct storage_inode_ref *reference)
{ (void)reference;abort(); }
#endif
/** @brief Model physical topology for the dedicated non-RAM BAR fixture.
 * @param physical Candidate page. @param bytes Candidate length.
 * @return False for this fixture's device aperture.
 */
bool mm_phys_is_ram(uint64_t physical,uint64_t bytes)
{ (void)physical;(void)bytes;return false; }
/** @brief Verify the policy remains held through the provider's final callback.
 * @param context Retained backing identity.
 */
static void fixture_device_release(void *context)
{
    assert(context==&device_base);
#ifdef TASK8_FIX4_CALLBACK
    fix4_assert_callback();
#endif
    struct paging_cache_claim probe={0};uint64_t flags=UINT64_MAX;
    assert(paging_cache_claim_select(&probe,device_base,&flags) && flags==expected_cache);
    paging_cache_claim_release(&probe);++provider_releases;
}
/** @brief Resolve the retained device's page by mapping offset.
 * @param context Retained backing identity. @param offset Page-aligned offset.
 * @return Fixture BAR physical page.
 */
static uint64_t fixture_device_physical(void *context,uint64_t offset)
{
    assert(context==&device_base);
#ifdef TASK8_FIX3_REUSED_BACKING
    return device_base+(offset/4096)*fix3_page_stride;
#else
    return device_base+offset;
#endif
}
static const struct device_mapping_operations backing_ops={
    .release=fixture_device_release,.physical=fixture_device_physical};
/** @brief Return a typed backing through the actual character mmap adapter.
 * @param task Original task. @param file Original OFD. @param offset Zero.
 * @param length Requested bytes. @param prot Requested access. @param flags Map flags.
 * @param out Empty mapping output. @return Zero or the backing allocation error.
 */
static int fixture_mmap(struct task *task,struct task_file *file,uint64_t offset,
                        uint64_t length,uint32_t prot,uint64_t flags,
                        struct device_mapping **out)
{
    assert(task==current && file==fix2_device_file && !offset);
#ifdef TASK8_FIX3_REUSED_BACKING
    if(reused_backing) {
        assert(length==4096 || length==8192);
        ++provider_calls;
        int rc=device_mapping_get(reused_backing);
        if(rc)return rc;
        *out=reused_backing;
        return 0;
    }
#endif
#if defined(TASK8_FIX5_FRONT)
    assert(length==256ULL*4096 || length==32768ULL*4096);
#elif defined(TASK8_FIX4_SCALE)
    assert(length==32768ULL*4096);
#elif defined(TASK8_FIX4_CALLBACK)
    assert(length==8192 || length==16384);
#else
    assert(length==8192);
#endif
    (void)prot;(void)flags;++provider_calls;
    #ifdef TASK8_FIX5_FRONT
    uint64_t backing_length=length==256ULL*4096 ? 32768ULL*4096 : 65536ULL*4096;
#else
    uint64_t backing_length=length;
#endif
    struct device_mapping_info info={.length=backing_length,.max_prot=3,.can_split=true,.can_move=true};
    return device_mapping_create(NULL,&backing_ops,&device_base,&info,out);
}
static const struct character_file_operations char_ops={.mmap=fixture_mmap};
static const struct character_device_registration registration={
    .major=221,.first_minor=0,.count=1,.operations=&char_ops};

/** @brief Count live intrusive physical-policy owners in the fixture.
 * @return Number of independently owned claims.
 */
static unsigned policy_count(void)
{ unsigned n=0;for(struct paging_cache_claim *p=cache_claims;p;p=p->next)++n;return n; }

/** @brief Exercise actual mmap, fork, unmap, retained stage and rollback.
 * @return Zero after the character provider and all cache claims retire.
 */
#ifndef TASK8_FIX3_REUSED_BACKING
int main(void)
{
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;
    mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();
    current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    const uint64_t user=base+0x1000000,uc=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    fix2_device_user_start=user;fix2_device_user_end=user+8192;
    const uint64_t mask=uc|RELIEFNT_PAGE_PAT;
    void *pci=pci_map_mmio_lease(device_base,8192);assert(pci);expected_cache=uc;
    const uint64_t mapflags=LINUX_MAP_SHARED|LINUX_MAP_FIXED;
    unsigned initial_policies=policy_count();
    int failures=0;
    for(int n=0;;++n) {
        fail_after=n;
        int64_t ret=syscall_mm_mmap(user,8192,LINUX_PROT_READ|LINUX_PROT_WRITE,mapflags,3,0);
        fail_after=-1;
        if(ret==(int64_t)user)break;
        assert(ret==-RELIEFOS_ENOMEM && n<12);
        assert(!address_space_user_page_phys(sched_task_as(current),user));
        assert(policy_count()==initial_policies);++failures;
    }
    assert(failures>=3 && provider_calls>=4);
    struct task_vma *vma=task_vma_containing(current,user,user+8192);assert(vma && vma->device_mapping);
    for(unsigned i=0;i<2;++i) {
        uint64_t leaf=address_space_user_page_flags(sched_task_as(current),user+i*4096);
        assert((leaf&mask)==uc && (leaf&RELIEFNT_PAGE_WRITABLE));
    }
    struct address_space child={0};assert(address_space_clone_cow(sched_task_as(current),&child));
    struct task_vma child_vma=*vma;child_vma.device_mapping=NULL;
    assert(!sched_task_vma_retain_device(&child_vma,vma));
    assert(!regular_stage_begin(current,user,64,false));
    struct regular_io_stage *stage=regular_stage_find(current);
    assert(stage && stage->pages==1 && stage->page[0].device);
    assert(((*kernel_shared_leaf((uintptr_t)stage->page[0].device_page.alias,false))&mask)==uc);
    pci_unmap_mmio(pci,8192);
    character_file_close(file.character_file,&file);file.character_file=NULL;
    unsigned prior=provider_releases;
    assert(!syscall_mm_munmap(user,8192));
    assert(provider_releases==prior && !paging_ioremap_device_page(device_base,0));
    task_regular_io_stage_cancel(current);task_regular_io_stage_cancel(current);
    assert(!regular_stage_find(current));
    assert(provider_releases==prior && !paging_ioremap_device_page(device_base,0));
    address_space_destroy(&child);task_vma_clear(&child_vma);
    assert(provider_releases==prior+1 && !policy_count());
    void *wb=paging_ioremap_device_page(device_base,0);assert(wb);
    assert(paging_iounmap(wb,4096));

    /* A new user-first mapping fixes WB; a later conflicting UC PCI claim is
     * rejected without changing the already installed user leaf. */
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    device_base+=0x100000;expected_cache=0;
    assert(syscall_mm_mmap(user,8192,3,mapflags,3,0)==(int64_t)user);
    assert(!(address_space_user_page_flags(sched_task_as(current),user)&mask));
    assert(!pci_map_mmio_lease(device_base,8192));
    assert(!(address_space_user_page_flags(sched_task_as(current),user)&mask));
    assert(!syscall_mm_munmap(user,8192));
    pci=pci_map_mmio_lease(device_base,8192);assert(pci);pci_unmap_mmio(pci,8192);
    character_file_close(file.character_file,&file);file.character_file=NULL;
    assert(!character_device_unregister(&registration));fix2_device_file=NULL;
    teardown();assert(!cache_claims);
    puts("PASS actual character mmap, PCI policy, fork/stage lifetime and OOM rollback");
    return 0;
}
#endif
