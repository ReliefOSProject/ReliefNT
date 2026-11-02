/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <reliefnt/fsnotify_vfs.h>
#include <linux/inotify.h>
/* Real scalar-read staging, syscall entry, PTE/swap and pressure recovery.
 * The fixture models finite physical-page/heap availability and swap transport. */
#define PRESSURE_REGULAR_IO_FIXTURE 1
#define PRESSURE_MINCORE_FIXTURE_ENTRY pressure_mincore_unused_main
#include "memory_pressure_mincore_fix4_test.c"
#include "../../kernel/syscall/io/file_io.c"

static bool block_small_stage, arm_pressure;
static unsigned stage_shortages, reads, closes;
static size_t blocked_size;
static int provider_cookie;

void task_vector_cancel(struct task *task) { assert(task==current && !task->vector_io); }
void task_socket_stage_cancel(struct task *task) { assert(task==current); }
void task_transfer_cancel(struct task *task) { assert(task==current); }
void graphics_service_cancel_task_continuation(struct task *task) { assert(task==current); }
void task_pty_release_entry(struct task_pty_fd *entry) { assert(entry && !entry->used); }

static bool pressure_fixture_heap_admitted(size_t size)
{
    if (block_small_stage && size == blocked_size &&
        quota_enforced && live_pages >= pressure_baseline_pages) {
        ++stage_shortages;
        return false;
    }
    return true;
}

uint64_t time_uptime_us(void) { return 0; }
uint64_t time_work_counter_us(void) { return time_uptime_us(); }
/* This fixture has one simulated CPU and no APIC transport. */
void smp_send_reschedule(uint32_t cpu) { (void)cpu; }
int storage_disk_block_read(uint32_t d,int32_t p,uint64_t o,void *b,uint32_t n,uint32_t *r)
{ (void)d;(void)p;(void)o;(void)b;(void)n;(void)r;abort(); }
int storage_disk_block_write(uint32_t d,int32_t p,uint64_t o,const void *b,uint32_t n,uint32_t *r)
{ (void)d;(void)p;(void)o;(void)b;(void)n;(void)r;abort(); }
int storage_sync_disk(uint32_t d) { (void)d;abort(); }

static uint8_t file_byte(uint64_t position) { return (uint8_t)(position * 13u + 7u); }
static int fixture_lookup(void *context,const struct storage_node *parent,const char *name,
                          struct storage_node *out)
{ (void)context;(void)parent;(void)name;(void)out;return -ENOENT; }
static int fixture_retain(void *context,const struct storage_node *node,void **pin)
{ assert(context==&provider_cookie && node && pin);*pin=NULL;return 0; }
static void fixture_release(void *context,const struct storage_node *node,void *pin)
{ assert(context==&provider_cookie && node && !pin); }
static bool fixture_same(void *context,const struct storage_node *a,const struct storage_node *b)
{ assert(context==&provider_cookie);return a->first_cluster==b->first_cluster; }
static int fixture_open(void *context,const struct storage_node *node,uint32_t flags,void **out)
{ assert(context==&provider_cookie && node);(void)flags;*out=&provider_cookie;return 0; }
static int fixture_read(void *context,const struct storage_node *node,void *state,
                        uint64_t offset,void *buffer,uint32_t length,uint32_t *done,uint64_t flags)
{
    assert(context==&provider_cookie && node && state==&provider_cookie && writer_owned());
    assert(!reader_owned() && !user_page_fault_lock.state);
    (void)flags;
    ++reads;
    for(uint32_t i=0;i<length;++i)((uint8_t *)buffer)[i]=file_byte(offset+i);
    *done=length;
    if (arm_pressure) {
        assert(offset==0 && length==REGULAR_IO_CHUNK);
        arm_pressure=false;
        pressure_baseline_pages=live_pages;
        quota_enforced=true;
    }
    return 0;
}
static void fixture_close(void *context,const struct storage_node *node,void *state)
{ assert(context==&provider_cookie && node && state==&provider_cookie);++closes; }
static const struct vfs_backend_ops ops={
    .lookup=fixture_lookup,.retain=fixture_retain,.release=fixture_release,
    .same_node=fixture_same,.file_open=fixture_open,.file_read=fixture_read,
    .file_close=fixture_close
};

static int64_t invoke_read(struct trap_frame *frame,unsigned *attempts)
{
    do {
        ++pressure_fixture_tick;
        current->state=TASK_RUNNING;current->running_cpu=0;
        if(current->waiting_queue)kernel_wait_queue_remove(current->waiting_queue,current);
        frame->rip=0x400002;frame->rax=LINUX_SYS_READ;
        (void)syscall_dispatch_frame(frame);
        assert(!writer_owned() && !reader_owned() && !user_page_fault_lock.state);
        assert(++*attempts<1800);
    } while(frame->rax==LINUX_SYS_READ);
    return (int64_t)frame->rax;
}

static void map_pages(struct task *task,uint64_t base,unsigned count)
{
    for(unsigned i=0;i<count;++i) {
        uint64_t phys=mm_alloc_page();assert(phys);
        assert(address_space_map_user_page(sched_task_as(task),base+(uint64_t)i*4096,phys,
                                          RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    }
}
static void restore_page(struct task *task,uint64_t page)
{
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    if(paging_user_entry_kind(sched_task_as(task),page)==PAGING_USER_ENTRY_SWAP)
        assert(swap_resolve_fault(task,page,6)==1);
    kernel_execution_unlock_irqrestore(irq);
}

int main(void)
{
    nx_enabled=true;page_cache_init();
    struct swap_area *area=setup_area();
    struct task *task=new_task();
    const uint64_t output=0x38000000, lazy=0x39000000, readonly=0x39001000, victims=0x30000000;
    const uint64_t length=REGULAR_IO_CHUNK+512;
    task->vmas[0]=(struct task_vma){.used=1,.start=output,.end=output+9*4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    create_victims(task,victims,8);
    task->vmas[2]=(struct task_vma){.used=1,.start=lazy,.end=lazy+4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    task->vmas[3]=(struct task_vma){.used=1,.start=readonly,.end=readonly+4096,
        .prot=LINUX_PROT_READ,.flags=TASK_VMA_FLAG_PRIVATE|TASK_VMA_FLAG_ANON};
    map_pages(task,output,9);
    assert(address_space_prepare_user_range(sched_task_as(task),lazy,readonly+4096));
    prepare_task(task);

    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};
    struct task_file file={.used=1,.flags=LINUX_O_RDONLY,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=2}};
    struct vfs_mount *mount=NULL;
    assert(!vfs_mount_new(&ops,&provider_cookie,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file.node,&file.path_ref));vfs_mount_put(mount);
    assert(!vfs_file_open(&file.path_ref,LINUX_O_RDONLY,&file.provider_file));
    pressure_regular_fixture_file=&file;
    task->syscall_file=&file;task->syscall_fd=3;task->syscall_file_number=LINUX_SYS_READ;

    struct trap_frame frame={.rax=LINUX_SYS_READ,.rdi=3,.rsi=output,.rdx=length,
        .rip=0x400002,.rsp=RELIEFNT_USER_TOP-8192,.cs=0x23,.ss=0x1b,.rflags=1ULL<<9};
    blocked_size=sizeof(struct regular_io_stage)+512;
    block_small_stage=true;arm_pressure=true;
    unsigned attempts=0;
    assert(invoke_read(&frame,&attempts)==(int64_t)length);
    assert(attempts>1 && stage_shortages && gate_writes && reads==2);
    assert(file.offset==length && !file.io_owner && !task->regular_io.active && !task->waiting_queue);

    quota_enforced=false;block_small_stage=false;
    for(unsigned i=0;i<9;++i)restore_page(task,output+(uint64_t)i*4096);
    for(uint64_t i=0;i<length;++i) {
        uint64_t phys=address_space_user_page_phys(sched_task_as(task),output+(i&~4095ULL));
        assert(phys && ((uint8_t *)mm_phys_to_virt(phys))[i&4095u]==file_byte(i));
    }

    /* A valid untouched output page must also reach reclaim, never EFAULT. */
    assert(paging_user_entry_kind(sched_task_as(task),lazy)==PAGING_USER_ENTRY_EMPTY);
    pressure_baseline_pages=live_pages;quota_enforced=true;
    unsigned old_writes=gate_writes,old_reads=reads,old_denials=regular_fixture_page_denials;
    frame.rsi=lazy;frame.rdx=64;attempts=0;
    assert(invoke_read(&frame,&attempts)==64);
    assert(regular_fixture_page_denials>old_denials && gate_writes>old_writes &&
           reads==old_reads+1 && file.offset==length+64);
    quota_enforced=false;restore_page(task,lazy);
    uint64_t phys=address_space_user_page_phys(sched_task_as(task),lazy);assert(phys);
    for(unsigned i=0;i<64;++i)assert(((uint8_t *)mm_phys_to_virt(phys))[i]==file_byte(length+i));

    old_reads=reads;uint64_t old_offset=file.offset;
    frame.rsi=readonly;frame.rdx=1;attempts=0;
    assert(invoke_read(&frame,&attempts)==-EFAULT && attempts==1);
    frame.rsi=readonly+4096;attempts=0;
    assert(invoke_read(&frame,&attempts)==-EFAULT && attempts==1);
    assert(reads==old_reads && file.offset==old_offset && !file.io_owner && !task->waiting_queue);

    /* Cancel a blocked, unsubmitted stage through the real descriptor cleanup. */
    restore_page(task,output);
    pressure_baseline_pages=live_pages;quota_enforced=true;block_small_stage=true;
    pressure_regular_contend_writes=true;frame.rsi=output;frame.rdx=512;
    ++pressure_fixture_tick;task->state=TASK_RUNNING;task->running_cpu=0;
    frame.rip=0x400002;frame.rax=LINUX_SYS_READ;
    (void)syscall_dispatch_frame(&frame);
    assert(frame.rax==LINUX_SYS_READ && task->regular_io.active &&
           task->waiting_queue && file.io_owner==task->pid);
    uint64_t irq;kernel_execution_lock_irqsave(&irq);
    task_release_syscall_file(task);
    assert(!task->waiting_queue && !task->regular_io.active && !file.io_owner &&
           !task->syscall_file && !sched_pressure_cursor(task,true)->cache_attempt);
    kernel_execution_unlock_irqrestore(irq);
    pressure_regular_contend_writes=false;quota_enforced=false;block_small_stage=false;
    task->state=TASK_RUNNING;

    restore_victims(task,victims,8);
    for(unsigned i=0;i<9;++i)restore_page(task,output+(uint64_t)i*4096);
    restore_page(task,lazy);
    vfs_file_close(file.provider_file);vfs_path_put(&file.path_ref);
    destroy_task(task);assert(closes==1 && !live_pages && !swap_area_unpublish(area));
    swap_area_destroy(area);
    printf("PASS scalar read: stage and page shortages reclaim, accepted prefix preserved, bad outputs rejected, wait cancelled\n");
    return 0;
}
