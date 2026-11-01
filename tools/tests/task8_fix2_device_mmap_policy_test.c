/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Compare actual DEVICE user-leaf cache bits with a live PCI BAR.
 * The host models CPU/TLB, physical pages and device callbacks.
 */
#define TASK8_FIX2_DEVICE_MMAP 1
#define main task8_fix2_previous_mm_main
#include "mprotect_vma_growth_test.c"
#undef main
#include "../../arch/x86_64/platform/pci.c"
#include <reliefnt/chrdev.h>

static uint64_t bar=0xc000000000ULL;
static unsigned released;
/** @brief Model this fixture's BAR as non-RAM. @param physical Start.
 * @param bytes Length. @return False for all tested BAR pages.
 */
bool mm_phys_is_ram(uint64_t physical,uint64_t bytes)
{ (void)physical;(void)bytes;return false; }
/** @brief Retire the fixture backing. @param context Stable BAR address. */
static void release_backing(void *context) { assert(context==&bar);++released; }
/** @brief Resolve a page of the fixture backing. @param context BAR owner.
 * @param offset Aligned offset. @return Original physical page.
 */
static uint64_t resolve_backing(void *context,uint64_t offset)
{ assert(context==&bar);return bar+offset; }
static const struct device_mapping_operations mapping_ops={
    .release=release_backing,.physical=resolve_backing};
/** @brief Admit a typed mapping through the actual character adapter.
 * @param task Current task. @param file Original OFD. @param offset Zero.
 * @param length Two pages. @param prot Access. @param flags Map flags.
 * @param out Empty owned result. @return Device mapping status.
 */
static int mmap_backing(struct task *task,struct task_file *file,uint64_t offset,
                        uint64_t length,uint32_t prot,uint64_t flags,
                        struct device_mapping **out)
{
    assert(task==current && file==fix2_device_file && !offset && length==8192);
    (void)prot;(void)flags;
    struct device_mapping_info info={.length=length,.max_prot=3,.can_split=true,.can_move=true};
    return device_mapping_create(NULL,&mapping_ops,&bar,&info,out);
}
static const struct character_file_operations file_ops={.mmap=mmap_backing};
static const struct character_device_registration registration={
    .major=221,.first_minor=0,.count=1,.operations=&file_ops};

/** @brief Check that actual mmap user PTEs inherit the live PCI UC policy.
 * @return Zero after removing all fixture-owned mappings and registrations.
 */
int main(void)
{
    kernel_pml4[511]=(uintptr_t)kernel_shared_pdpt|3;mmio_dynamic_tables_ready=true;
    setup(1);clear_setup_vmas();current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct task_file file={.used=1,.flags=LINUX_O_RDWR};fix2_device_file=&file;
    assert(!character_device_register(&registration));
    assert(!character_file_open((uint64_t)221<<8,current,&file,&file.character_file));
    void *lease=pci_map_mmio_lease(bar,8192);assert(lease);
    uint64_t user=base+0x1000000,uc=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD;
    assert(syscall_mm_mmap(user,8192,3,LINUX_MAP_SHARED|LINUX_MAP_FIXED,3,0)==(int64_t)user);
    for(unsigned i=0;i<2;++i)
        assert((address_space_user_page_flags(sched_task_as(current),user+i*4096)&
                (RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PAT))==uc);
    assert(!syscall_mm_munmap(user,8192) && released==1);
    pci_unmap_mmio(lease,8192);character_file_close(file.character_file,&file);
    assert(!character_device_unregister(&registration));teardown();
    puts("PASS actual DEVICE mmap user leaves match the live PCI UC BAR");return 0;
}
