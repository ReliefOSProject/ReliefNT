/* Real swapoff, MM, slot, PTE and reclaim paths; host RAM and transport modeled. */
#define TASK11_SWAPOFF_RECOVERY 1
#define SWAPOFF_FRONTDOOR_FIXTURE_ENTRY previous_swapoff_frontdoor_main
#define mm_alloc_page fixture_alloc_page
#define swap_storage_transfer fixture_storage_transfer
#define kernel_execution_suspend_irqrestore fixture_execution_suspend
#include "memory_pressure_swapoff_frontdoor_test.c"
#undef mm_alloc_page
#undef swap_storage_transfer
#undef kernel_execution_suspend_irqrestore
#include <reliefnt/memory_pressure.h>

static unsigned quota = 64;
static uint64_t cache_epoch;
static bool foreign_busy, recovery_io_failure, change_caller;
static bool drain_change, fatal_before, fatal_after, cancelled_boundary;
static unsigned cache_scans, io_after_cancel, scans_after_cancel;
int __real_swap_drain_step(struct swap_area *,struct swap_drain_cursor *,uint32_t,uint32_t *);

/* The real drain accepts any budget from 2 to 32. Use its smallest legal
 * budget to reach a drain checkpoint after the first materialized leaf. */
int __wrap_swap_drain_step(struct swap_area *area,struct swap_drain_cursor *cursor,
                           uint32_t budget,uint32_t *work)
{
    return __real_swap_drain_step(area,cursor,drain_change ? 2 : budget,work);
}

uint64_t mm_alloc_page(void)
{
    if (live_host_pages >= quota) {
        if (fatal_before && !cancelled_boundary) {
            task.pending_signals|=1ULL<<8;cancelled_boundary=true;
        }
        return 0;
    }
    return fixture_alloc_page();
}
uint64_t mm_total_memory_kib(void) { return 8192 * 4; }
uint64_t mm_emergency_reserve_pages(void) { return 64; }
uint64_t mm_free_memory_kib(void)
{ return 4 * (64 + (quota > live_host_pages ? quota - live_host_pages : 0)); }
bool swap_storage_busy(void) { return active_token != NULL || foreign_busy; }
int swap_storage_transfer(const struct swap_storage_token *token,const struct block_device_ref *ref,
                          uint64_t first,uint32_t count,void *bytes,bool write)
{
    if (cancelled_boundary) ++io_after_cancel;
    if (write && recovery_io_failure && live_host_pages>=quota) return -EIO;
    return fixture_storage_transfer(token,ref,first,count,bytes,write);
}
int kernel_execution_suspend_irqrestore(struct kernel_execution_suspend_token *token)
{
    int rc=fixture_execution_suspend(token);
    if (!rc && (change_caller || drain_change) && live_host_pages>=quota) {
        task.pid=4;if (drain_change) cancelled_boundary=true;
    }
    if (!rc && fatal_after && cache_scans && !cancelled_boundary) {
        task.pending_signals|=1ULL<<8;cancelled_boundary=true;
    }
    return rc;
}
uint32_t page_cache_pressure_scan_count(uint32_t budget, uint32_t *inspected,
                                        bool *complete, uint32_t *candidates)
{
    assert(budget);
    ++cache_scans;if (cancelled_boundary) ++scans_after_cancel;
    *inspected = 1; *complete = true; *candidates = 0; ++cache_epoch;
    return 0;
}
uint64_t page_cache_pressure_epoch(void) { return cache_epoch; }
struct pressure_cursor *sched_pressure_cursor(struct task *owner, bool allow_oom)
{ (void)owner; (void)allow_oom; abort(); }
int memory_oom_advance(uint32_t budget) { (void)budget; abort(); }
void memory_pressure_notify(void) { }

/** @brief Exercise failed-drain recovery and verify both original page bytes.
 * @param argc Argument count. @param argv Scenario name. @return Zero on pass.
 */
int main(int argc,char **argv)
{
    assert(argc==2);
    bool normal=!strcmp(argv[1],"success");
    bool blocked=!strcmp(argv[1],"foreign");
    bool io_error=!strcmp(argv[1],"io-error");
    bool cancelled=!strcmp(argv[1],"caller-change");
    drain_change=!strcmp(argv[1],"drain-caller-change");
    fatal_before=!strcmp(argv[1],"fatal-before-recovery");
    fatal_after=!strcmp(argv[1],"fatal-after-checkpoint");
    assert(normal || blocked || io_error || cancelled || drain_change || fatal_before || fatal_after ||
           !strcmp(argv[1],"partial"));
    files[0].node = (struct storage_node){.type=RELIEFOS_FS_TYPE_FILE,
        .flags=STORAGE_NODE_FLAG_EXT2,.first_cluster=1};
    files[0].physical=64; header(64);
    assert(!syscall_mm_swapon((uintptr_t)"one",0));
    task.state=TASK_RUNNING; nx_enabled=true;
    const uint64_t base=RELIEFNT_USER_BASE+0x60000;
    task.vmas[0]=(struct task_vma){.used=1,.start=base,.end=base+8192,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,
        .flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    assert(address_space_create(sched_task_as(&task)));
    for(unsigned j=0;j<2;++j) {
        uint64_t phys=mm_alloc_page();assert(phys);
        for(unsigned i=0;i<4096;++i)
            ((uint8_t *)mm_phys_to_virt(phys))[i]=(uint8_t)(i*17u+j*59u);
        assert(address_space_map_user_page(sched_task_as(&task),base+j*4096,phys,
                                           RELIEFNT_PAGE_WRITABLE));
        assert(swap_try_evict(sched_task_as(&task),base+j*4096)==1);
    }
    quota=live_host_pages+(normal ? 2u : 1u);
    foreign_busy=blocked; recovery_io_failure=io_error; change_caller=cancelled;
    int rc=syscall_mm_swapoff((uintptr_t)"one");
    if (normal) {
        assert(!rc && !claims && !refs && !console_warnings);
    } else {
        assert(rc==(drain_change ? -ESRCH : -ENOMEM) && files[0].claimed && claims && refs);
        if (drain_change || fatal_before || fatal_after) {
            assert(cancelled_boundary);
            if (io_after_cancel || scans_after_cancel) {
                fprintf(stderr,"FAIL: recovery continued after cancellation: scans=%u io=%u\n",
                        scans_after_cancel,io_after_cancel);
                return 1;
            }
        }
        if (!blocked && !io_error && !cancelled &&
            !drain_change && !fatal_before && !fatal_after &&
            mm_free_memory_kib()/4-mm_emergency_reserve_pages() != 1) {
            fputs("FAIL: failed live swapoff consumed the last ordinary frame\n",stderr);
            return 1;
        }
        if (!drain_change && !fatal_before && !fatal_after)
            assert(console_warnings==(blocked || io_error || cancelled ? 1u : 0u));
        if (!blocked && !io_error && !cancelled && !drain_change && !fatal_before && !fatal_after) {
            uint64_t probe=mm_alloc_page();assert(probe);mm_free_page(probe);
        } else {
            assert(paging_user_entry_kind(sched_task_as(&task),base)==PAGING_USER_ENTRY_RESIDENT);
            assert(paging_user_entry_kind(sched_task_as(&task),base+4096)==PAGING_USER_ENTRY_SWAP);
        }
        foreign_busy=recovery_io_failure=change_caller=drain_change=fatal_before=fatal_after=false;
        cancelled_boundary=false;task.pid=3;task.pending_signals=0;
        quota=64;
        assert(!syscall_mm_swapoff((uintptr_t)"one"));
    }
    for(unsigned j=0;j<2;++j) {
        uint64_t phys=address_space_user_page_phys(sched_task_as(&task),base+j*4096);
        assert(phys);
        for(unsigned i=0;i<4096;++i)
            assert(((uint8_t *)mm_phys_to_virt(phys))[i]==(uint8_t)(i*17u+j*59u));
    }
    address_space_destroy(sched_task_as(&task));assert(!live_host_pages);empty();
    printf("PASS %s: syscall status, ownership and both page contents\n",argv[1]);
    return 0;
}
