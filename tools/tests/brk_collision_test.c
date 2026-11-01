/* A Linux brk extension must never replace a VMA already in the process. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <linux/mman.h>
#include <linux/errno.h>
#include <linux/mount.h>
#include <reliefnt/mount.h>
#include <reliefnt/audio.h>
#include <reliefnt/console.h>
#include <reliefnt/drm.h>
#include <reliefnt/syscall.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/heap.h>
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall_internal.h>
#include "sysv_shm_fixture_stubs.h"
#include "execution_suspend_fixture.h"

int64_t syscall_mm_brk(uint64_t requested);
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset);
int64_t syscall_mm_munmap(uint64_t addr, uint64_t len);
int64_t syscall_legacy_memory(uint64_t number, uint64_t a0, uint64_t a1,
                              uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
void mm_context_destroy(struct address_space *as);
/** @brief Reject memfd creation in this memory-dispatch fixture.
 * @param name User name address. @param flags Flags. @return Never returns.
 */
int64_t syscall_memfd_create(uint64_t name, uint32_t flags)
{ (void)name; (void)flags; abort(); }

static struct task *current_task;
static struct task_file framebuffer_file,second_file;
static int fail_file_pin;
static struct framebuffer framebuffer_device;
static const uint64_t framebuffer_alias = 0xffffff8000200000ULL;
static const uint64_t framebuffer_physical = 0x42000000ULL;
static unsigned eager_range_prepares;
static unsigned page_alloc_attempts;
static unsigned mmap_provider_pins;
static uint64_t mmap_provider_flags,mmap_provider_size=16384;
static bool mmap_numeric_replaced;
static bool mmap_async_requested=true;
static int mmap_provider_error;
static uint64_t last_device_mapping_phys,last_device_mapping_flags;
static int file_writable = 1;
struct task *sched_current_task(void) { return current_task; }

/** @brief Reject typed DEVICE policy selection in this FB-only fixture.
 * @param claim Unused owner. @param physical Unused page. @param flags Unused
 * output. @return Never returns.
 */
bool paging_cache_claim_select(struct paging_cache_claim *claim,uint64_t physical,uint64_t *flags)
{ (void)claim;(void)physical;(void)flags;abort(); }
/** @brief Reject typed DEVICE policy coalescing in this FB-only fixture.
 * @param first Unused owner. @param second Unused owner. @return Never returns.
 */
bool paging_cache_claim_merge(struct paging_cache_claim *first,struct paging_cache_claim *second)
{ (void)first;(void)second;abort(); }
/** @brief Reject typed DEVICE policy release in this FB-only fixture.
 * @param claim Unused owner. This function never returns.
 */
void paging_cache_claim_release(struct paging_cache_claim *claim)
{ (void)claim;abort(); }

uint32_t sched_task_vma_capacity(const struct task *task)
{
    (void)task;
    return SCHED_TASK_VMA_MAX;
}

struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    return index < SCHED_TASK_VMA_MAX ? &sched_task_mm(task)->vmas[index] : NULL;
}

uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t address)
{
    (void)as;
    (void)address;
    return 0;
}

bool address_space_user_page_is_device(const struct address_space *as, uint64_t address)
{ (void)as; (void)address; return false; }
/** @brief Reject unexercised protection in the dispatch fixture.
 * @param as MM. @param address Page. @param flags Protection. @return Never.
 */
bool address_space_protect_user_page(struct address_space *as, uint64_t address, uint64_t flags)
{ (void)as; (void)address; (void)flags; abort(); }
/** @brief Reject unexercised user-output checks. @param ptr Address.
 * @param len Bytes. @return Never.
 */
bool user_range_writable(uint64_t ptr, uint64_t len)
{ (void)ptr; (void)len; abort(); }
/** @brief Reject unexercised backing residency. @param node Backing.
 * @param offset File offset. @return Never.
 */
bool storage_tmpfs_page_resident(const struct storage_node *node, uint64_t offset)
{ (void)node; (void)offset; abort(); }
/** @brief Reject unexercised cache lookup. @param node Backing.
 * @param offset File offset. @param phys Frame output. @return Never.
 */
int page_cache_lookup(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; abort(); }
/** @brief Reject unexercised locked file slices. @param dst Copy.
 * @param src Original. @return Never.
 */
int sched_task_vma_retain_file(struct task_vma *dst, const struct task_vma *src)
{ (void)dst; (void)src; abort(); }
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t address)
{ (void)as; (void)address; return 0; }
bool address_space_prepare_user_range(struct address_space *as, uint64_t start, uint64_t end)
{ (void)as; (void)start; (void)end; ++eager_range_prepares; return true; }
bool address_space_map_user_page(struct address_space *as, uint64_t address, uint64_t phys, uint64_t flags)
{ (void)as; (void)address; last_device_mapping_phys = phys;last_device_mapping_flags=flags;return true; }
bool address_space_user_range_free(const struct address_space *as, uint64_t start, uint64_t end)
{ (void)as; (void)start; (void)end; return true; }
bool address_space_user_page_next(const struct address_space *as, uint64_t start,
                                  uint64_t end, uint64_t *page, uint64_t *phys,
                                  uint64_t *flags)
{
    (void)as; (void)start; (void)end;
    if (page) *page = 0;
    if (phys) *phys = 0;
    if (flags) *flags = 0;
    return false;
}
/** @brief Report no resident or swapped leaves in this VMA-only fixture.
 * @param as Borrowed fixture address space. @param start First address.
 * @param end Exclusive end. @param page Optional page output.
 * @param phys Optional physical output. @param flags Optional flags output.
 * @return False; this fixture does not model page-table leaves.
 */
bool address_space_user_entry_next(const struct address_space *as, uint64_t start,
                                   uint64_t end, uint64_t *page, uint64_t *phys,
                                   uint64_t *flags)
{ return address_space_user_page_next(as, start, end, page, phys, flags); }
/** @brief Classify the VMA-only fixture's unmodeled leaves as empty.
 * @param as Borrowed fixture address space. @param va User byte address.
 * @return EMPTY because this fixture models no page-table leaves.
 */
enum paging_user_entry_kind paging_user_entry_kind(const struct address_space *as,
                                                   uint64_t va)
{ (void)as; (void)va; return PAGING_USER_ENTRY_EMPTY; }
/** @brief Reject an unreachable swap-leaf installation in this fixture.
 * @param as Borrowed address space. @param va User address. @param flags Leaf flags.
 * @return Never; the fixture reports no swap entries to the remap walker.
 */
bool address_space_map_user_swap(struct address_space *as, uint64_t va, uint64_t flags)
{ (void)as; (void)va; (void)flags; abort(); }
void smp_flush_user_tlb(void) {}
int page_cache_owns(uint64_t phys) { (void)phys; return 0; }
void page_cache_release(uint64_t phys) { (void)phys; }
void mm_free_page(uint64_t phys) { (void)phys; }
uint64_t mm_alloc_page(void) { ++page_alloc_attempts; return 0; }
/** @brief Model an exhausted VMA-only fixture page pool for reclaim too.
 * @return Zero; neither ordinary nor emergency pages exist in this model.
 */
uint64_t mm_alloc_emergency_page(void) { return mm_alloc_page(); }
uint64_t mm_free_memory_kib(void) { return UINT64_MAX; }
int storage_inode_put(struct storage_inode_ref *reference) { (void)reference; return 0; }
void storage_inode_retain(struct storage_inode_ref *reference) { (void)reference; }
int storage_inode_get(const struct storage_node *node, struct storage_inode_ref **out)
{ (void)node; *out = NULL; return 0; }
int storage_inode_refresh(struct storage_node *node) { (void)node;fputs("unexpected bare mmap metadata refresh\n",stderr);abort(); }
int storage_node_mount_flags(const struct storage_node *node, uint64_t *flags)
{ (void)node;(void)flags;fputs("unexpected bare mmap mount policy\n",stderr);abort(); }
void storage_set_io_async_context(bool requested){mmap_async_requested=requested;}
struct task_file *task_file_for_fd(struct task *task, int fd)
{ (void)task; return fd == 7 ? (mmap_numeric_replaced ? &second_file : &framebuffer_file) : fd == 8 ? &second_file : NULL; }
struct task_file *task_file_for_io(struct task *task,int fd)
{return task->syscall_file && task->syscall_fd==fd ? task->syscall_file : task_file_for_fd(task,fd);}
int file_can_read(const struct task_file *file) { return file == &framebuffer_file || file == &second_file; }
int file_can_write(const struct task_file *file) { return (file == &framebuffer_file || file == &second_file) && file_writable; }
int task_shm_mmap_check(const struct task_file *file, uint32_t flags, uint32_t prot, uint32_t *maximum)
{ (void)file; (void)flags; (void)prot; (void)maximum; return -1; }
const struct framebuffer *framebuffer_get(void) { return &framebuffer_device; }
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    return value >= framebuffer_alias && value - framebuffer_alias < 4096
               ? framebuffer_physical + value - framebuffer_alias : 0;
}
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    return phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE &&
           len <= RELIEFNT_KERNEL_DIRECT_MAP_SIZE - phys;
}
bool paging_kernel_direct_cache_flags(uint64_t physical,uint64_t *flags)
{
    *flags=physical>=framebuffer_physical && physical-framebuffer_physical<8192 ?
        RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD : 0;
    return true;
}
void console_printf(const char *fmt, ...) { (void)fmt; }

void console_log(enum console_log_level level, const char *fmt, ...)
{
    (void)level; (void)fmt;
}
void *kernel_malloc(size_t size) { return __builtin_malloc(size); }
void kernel_free(void *ptr) { __builtin_free(ptr); }
/** @brief Enter fixture registry exclusion. @param lock Lock. @param flags Saved state output. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
/** @brief Leave fixture registry exclusion. @param lock Lock. @param flags Saved state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
/* This admission fixture never populates a user PTE or enters a file fault. */
/** @brief Reject unexpected frame access. @param phys Frame. @return Never returns. */
void *mm_phys_to_virt(uint64_t phys) { (void)phys; abort(); }
/** @brief Reject unexpected physical pin. @param phys Frame. */
void mm_retain_page(uint64_t phys) { (void)phys; abort(); }
/** @brief Reject unexpected PTE inspection. @param as MM. @param page Address.
 * @return Never returns.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
/** @brief Reject unexpected COW fault. @param as MM. @param page Address.
 * @return Never returns.
 */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
/** @brief The brk fixture never maps a shared file-cache leaf. */
bool address_space_shared_write_fault(struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
/** @brief Reject unexpected stack fault. @param as MM. @param page Address.
 * @return Never returns.
 */
bool address_space_map_user_stack_page(struct address_space *as, uint64_t page)
{ (void)as; (void)page; abort(); }
/** @brief Reject unexpected cache I/O. @param node File. @param offset Offset.
 * @param loader Loader. @param context Loader state. @param phys Output. @return Never returns.
 */
int page_cache_load_fault(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{ (void)node; (void)offset; (void)loader; (void)context; (void)phys; abort(); }
/** @brief The brk fixture never performs ordinary file-cache I/O. */
int page_cache_load(const struct storage_node *node, uint64_t offset,
                    page_cache_loader loader, void *context, uint64_t *phys)
{ (void)node; (void)offset; (void)loader; (void)context; (void)phys; abort(); }
/** @brief Reject an unexpected shared-file grant. @param page Frame. */
void page_cache_mark_shared_writable(uint64_t page) { (void)page; abort(); }
/** @brief Reject unexpected tmpfs I/O. @param node File. @param offset Offset.
 * @param phys Output. @return Never returns.
 */
int storage_tmpfs_get_page(const struct storage_node *node, uint64_t offset, uint64_t *phys)
{ (void)node; (void)offset; (void)phys; abort(); }
/** @brief Reject unexpected storage errors. @param error Status. @return Never returns. */
int storage_errno(int error) { (void)error; abort(); }
/** @brief Reject unexpected loader entry. @param flags Saved state output. */
void userland_loader_lock(uint64_t *flags) { (void)flags; abort(); }
/** @brief Reject unexpected loader exit. @param flags Saved state. */
void userland_loader_unlock(uint64_t flags) { (void)flags; abort(); }
struct task_file *task_file_get(struct task_file *source)
{if(fail_file_pin)return NULL;assert(source && source->references);++source->references;return source;}
void task_file_put(struct task_file *description)
{if(description){assert(description->references);if(!--description->references)vfs_path_put(&description->path_ref);}}
static int mmap_provider_lookup(void *context,const struct storage_node *parent,const char *name,struct storage_node *out)
{(void)context;(void)parent;(void)name;(void)out;return -ENOENT;}
static int mmap_provider_retain(void *context,const struct storage_node *node,void **pin)
{assert(context==&mmap_provider_pins && node);++mmap_provider_pins;*pin=NULL;return 0;}
static void mmap_provider_release(void *context,const struct storage_node *node,void *pin)
{assert(context==&mmap_provider_pins && node && !pin && mmap_provider_pins);--mmap_provider_pins;}
static bool mmap_provider_same(void *context,const struct storage_node *left,const struct storage_node *right)
{(void)context;return left->type==right->type && left->first_cluster==right->first_cluster;}
static int mmap_provider_stat(void *context,const struct vfs_path *path,struct linux_stat_abi *out)
{assert(context==&mmap_provider_pins && !mmap_async_requested && vfs_path_node(path)->type==RELIEFOS_FS_TYPE_FILE);if(mmap_provider_error)return mmap_provider_error;*out=(struct linux_stat_abi){.st_size=(int64_t)mmap_provider_size,.st_mode=0100600};return 0;}
static uint64_t mmap_provider_policy(void *context)
{assert(context==&mmap_provider_pins);return mmap_provider_flags;}
static void bind_mmap_provider(struct task_file *file)
{
    static const struct vfs_backend_ops ops={.lookup=mmap_provider_lookup,.retain=mmap_provider_retain,
        .release=mmap_provider_release,.same_node=mmap_provider_same,.getattr=mmap_provider_stat,.mount_flags=mmap_provider_policy};
    struct storage_node root={.type=RELIEFOS_FS_TYPE_DIR};struct vfs_mount *mount=NULL;
    assert(!file->path_ref.mount && !vfs_mount_new(&ops,&mmap_provider_pins,NULL,&root,&mount));
    assert(!vfs_path_anonymous(mount,&file->node,&file->path_ref));vfs_mount_put(mount);
}

/** @brief Route real checked-population and remap quota errors through legacy dispatch. */
static void test_memory_legacy_final_errors(void)
{
    struct task owner = {0};
    owner.kind = TASK_KIND_USER;
    owner.stack_top = RELIEFNT_USER_TOP - 4096;
    owner.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    owner.limits.memlock.rlim_cur = 4096;
    /* The production checked walk requires a live root identity; this fixture
     * stubs paging, so supply a distinct synthetic CR3 for the private MM. */
    sched_task_as(&owner)->cr3 = 0x12345000;
    uint64_t address = 0x62000000;
    owner.vmas[0] = (struct task_vma){.used = 1, .start = address, .end = address + 4096,
        .prot = LINUX_PROT_READ | LINUX_PROT_WRITE,
        .flags = TASK_VMA_FLAG_ANON | TASK_VMA_FLAG_PRIVATE};
    current_task = &owner;
    unsigned initial_attempts = page_alloc_attempts;
    unsigned attempts = initial_attempts;
    assert(syscall_legacy_memory(LINUX_SYS_MLOCK, address, 4096, 0, 0, 0, 0) == -EAGAIN);
    assert(page_alloc_attempts == attempts + 1);
    attempts = page_alloc_attempts;
    assert(syscall_legacy_memory(LINUX_SYS_MLOCK2, address, 4096, 0, 0, 0, 0) == -EAGAIN);
    assert(page_alloc_attempts == attempts + 1);
    owner.vmas[0].flags |= TASK_VMA_FLAG_LOCKED;
    assert(syscall_legacy_memory(LINUX_SYS_MREMAP, address, 4096, 8192, 0, 0, 0) == -EAGAIN);
    mm_context_destroy(sched_task_as(&owner));
    page_alloc_attempts = initial_attempts;
    current_task = NULL;
    puts("PASS normalized memory route preserves real mlock allocation and mremap quota EAGAIN");
}

static void test_regular_mapping_description(void)
{
    struct task owner={0};owner.kind=TASK_KIND_USER;
    owner.stack_top=RELIEFNT_USER_TOP-4096;
    owner.limits.as.rlim_cur=owner.limits.as.rlim_max=LINUX_RLIM_INFINITY;
    current_task=&owner;
    framebuffer_file=(struct task_file){.used=1,.references=1,.flags=LINUX_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=50,.size=16384}};
    bind_mmap_provider(&framebuffer_file);
    const uint64_t base=0x50000000,page=4096;
    unsigned initial_pins=mmap_provider_pins;
    mmap_provider_error=-EAGAIN;
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,
        LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==-EAGAIN);
    assert(syscall_mm_mmap_dispatch(base,page,LINUX_PROT_READ,
        LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==KERNEL_MMAP_FILE_IO_RETRY);
    assert(syscall_legacy_memory(LINUX_SYS_MMAP,base,page,LINUX_PROT_READ,
        LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==KERNEL_MMAP_FILE_IO_RETRY);
    assert(framebuffer_file.references==1 && mmap_provider_pins==initial_pins);
    mmap_provider_error=0;
    assert(syscall_mm_mmap(base,3*page,LINUX_PROT_READ,
        LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==(int64_t)base);
    task_file_put(&framebuffer_file); /* Numeric close at the OFD boundary. */
    assert(framebuffer_file.references==1);
    assert(!syscall_mm_munmap(base+page,page));assert(framebuffer_file.references==2);
    assert(!syscall_mm_munmap(base,page));assert(framebuffer_file.references==1);
    assert(!syscall_mm_munmap(base+2*page,page));assert(!framebuffer_file.references);
    framebuffer_file.references=1;bind_mmap_provider(&framebuffer_file);second_file=framebuffer_file;
    second_file.path_ref=(struct vfs_path){0};vfs_path_get(&framebuffer_file.path_ref,&second_file.path_ref);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==(int64_t)base);
    assert(syscall_mm_mmap(base+page,page,LINUX_PROT_READ,LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,8,page)==(int64_t)(base+page));
    task_file_put(&framebuffer_file);task_file_put(&second_file);
    assert(framebuffer_file.references==1 && second_file.references==1);
    assert(!syscall_mm_munmap(base,page));assert(!framebuffer_file.references && second_file.references==1);
    assert(!syscall_mm_munmap(base+page,page));assert(!second_file.references);
    framebuffer_file.references=1;bind_mmap_provider(&framebuffer_file);fail_file_pin=1;
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_PRIVATE|LINUX_MAP_FIXED_NOREPLACE,7,0)==-ENOMEM);
    fail_file_pin=0;assert(framebuffer_file.references==1);
    task_file_put(&framebuffer_file);assert(!mmap_provider_pins);
    for(unsigned i=0;i<SCHED_TASK_VMA_MAX;++i)assert(!owner.address_space.vmas[i].used);
    puts("PASS regular mmap keeps its OFD after close and partial unmap; different OFDs do not merge and pin failure publishes no VMA");
}

/* Mount policy applies to every regular provider, including those without
 * the old EXT2/TMPFS node flags. Metadata comes from the held inode provider. */
static struct task_vma *mmap_result_vma(struct task *task,uint64_t address)
{
    for(uint32_t i=0;i<sched_task_vma_capacity(task);++i) {
        struct task_vma *vma=sched_task_vma_at(task,i);
        if(vma && vma->used && vma->start<=address && address<vma->end)return vma;
    }
    return NULL;
}
static void test_regular_mapping_provider(void)
{
    struct task owner={.kind=TASK_KIND_USER};owner.stack_top=RELIEFNT_USER_TOP-4096;
    owner.limits.as.rlim_cur=owner.limits.as.rlim_max=LINUX_RLIM_INFINITY;current_task=&owner;
    framebuffer_file=(struct task_file){.used=1,.references=1,.flags=LINUX_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=50,.size=1}};
    bind_mmap_provider(&framebuffer_file);const uint64_t base=0x52000000,page=4096;
    uint64_t fixed=LINUX_MAP_FIXED_NOREPLACE;mmap_provider_size=3*page;
    assert(!vfs_mount_set_flags(framebuffer_file.path_ref.mount,MS_NOEXEC));
    assert(syscall_mm_mmap(base,page,LINUX_PROT_EXEC,LINUX_MAP_PRIVATE|fixed,7,0)==-EPERM);
    mmap_provider_flags=MS_RDONLY;
    assert(!vfs_mount_set_flags(framebuffer_file.path_ref.mount,0));
    assert(syscall_mm_mmap(base,page,LINUX_PROT_WRITE,LINUX_MAP_SHARED|fixed,7,0)==-EROFS);
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_SHARED|fixed,7,0)==(int64_t)base);
    struct task_vma *vma=mmap_result_vma(&owner,base);assert(vma && !(vma->max_prot&LINUX_PROT_WRITE));
    assert(vma->file_node.size==3*page);assert(!syscall_mm_munmap(base,page));
    assert(syscall_mm_mmap(base,page,LINUX_PROT_WRITE,LINUX_MAP_PRIVATE|fixed,7,0)==(int64_t)base);
    assert(!syscall_mm_munmap(base,page));mmap_provider_flags=0;
    /* The actual mmap consumer must honor common admission even after the
     * numeric table entry names a different inode. */
    second_file=(struct task_file){.used=1,.references=1,.flags=LINUX_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.first_cluster=60,.size=1}};
    bind_mmap_provider(&second_file);owner.syscall_file=task_file_get(&framebuffer_file);owner.syscall_fd=7;
    task_file_put(&framebuffer_file);mmap_numeric_replaced=true;
    assert(syscall_mm_mmap(base,page,LINUX_PROT_READ,LINUX_MAP_PRIVATE|fixed,7,0)==(int64_t)base);
    vma=mmap_result_vma(&owner,base);
    assert(vma && vma->file_node.first_cluster==50 && vma->mapping_file==&framebuffer_file);
    task_file_put(owner.syscall_file);owner.syscall_file=NULL;
    assert(!syscall_mm_munmap(base,page) && !framebuffer_file.references);
    task_file_put(&second_file);assert(!mmap_provider_pins);
    mmap_numeric_replaced=false;mmap_provider_size=16384;
    puts("PASS regular mmap uses actual provider metadata, all-filesystem RO/noexec policy and original admitted OFD");
}

int main(void)
{
    test_memory_legacy_final_errors();
    test_regular_mapping_provider();
    test_regular_mapping_description();
    const uint64_t page = 4096;
    const uint64_t start = RELIEFNT_USER_HEAP_BASE + 0x20000;
    struct task task = {0};
    task.kind = TASK_KIND_USER;
    task.stack_top = RELIEFNT_USER_TOP - page;
    task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    task.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    task.program_break_base = task.program_break = start;
    task.address_space.vmas[0] = (struct task_vma){
        .used = 1, .start = start + page, .end = start + 2 * page,
        .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_READ,
    };
    current_task = &task;

    /* Linux permits an explicit anonymous W|X reservation for JIT runtimes.
     * Admission remains lazy and unknown protection bits remain invalid. */
    const uint64_t jit_start = 0x0000000040000000ULL;
    assert(syscall_mm_mmap(jit_start, page,
        LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0) == (int64_t)jit_start);
    assert(!eager_range_prepares && !page_alloc_attempts);
    assert(syscall_mm_munmap(jit_start, page) == 0);
    assert(syscall_mm_mmap(jit_start, page, 0x80,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0) == -RELIEFOS_EINVAL);

    framebuffer_file = (struct task_file){.used=1,.references=1,.flags=LINUX_O_RDWR,
        .node={.type=RELIEFOS_FS_TYPE_FILE,.size=page}};
    bind_mmap_provider(&framebuffer_file);
    assert(!vfs_mount_set_flags(framebuffer_file.path_ref.mount, MS_NOEXEC));
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_PRIVATE | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EPERM);
    assert(!vfs_mount_set_flags(framebuffer_file.path_ref.mount, MS_RDONLY));
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EROFS);
    assert(!vfs_mount_set_flags(framebuffer_file.path_ref.mount, 0));
    file_writable = 0;
    assert(syscall_mm_mmap(jit_start, page, LINUX_PROT_WRITE | LINUX_PROT_EXEC,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, 0) == -RELIEFOS_EACCES);
    file_writable = 1;
    task_file_put(&framebuffer_file);
    assert(!mmap_provider_pins);

    /* Linux brk(2) returns the old break when an existing VMA blocks growth. */
    int64_t result = syscall_mm_brk(start + 2 * page);
    if (result != (int64_t)start) {
        fprintf(stderr, "brk collision returned %#llx, expected %#llx\n",
                (unsigned long long)result, (unsigned long long)start);
    }
    assert(result == (int64_t)start);
    assert(task.program_break == start);
    assert(task.address_space.vmas[0].used);
    assert(task.address_space.vmas[0].start == start + page);
    assert(task.address_space.vmas[0].end == start + 2 * page);
    assert(task.address_space.vmas[0].prot == LINUX_PROT_READ);

    /* A sparse VMA may reserve a 1 TiB span without allocating page tables
     * or walking each page on either publication or munmap. */
    const uint64_t sparse_start = 0x0000010000000000ULL;
    const uint64_t sparse_length = 1ULL << 40;
    struct task sparse = {0};
    sparse.kind = TASK_KIND_USER;
    sparse.stack_top = RELIEFNT_USER_TOP - page;
    sparse.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    sparse.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    current_task = &sparse;
    unsigned prepares_before_sparse = eager_range_prepares;
    unsigned page_allocs_before_sparse = page_alloc_attempts;
    int64_t sparse_mapped = syscall_mm_mmap(sparse_start, sparse_length,
        LINUX_PROT_READ | LINUX_PROT_WRITE,
        LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE,
        UINT64_MAX, 0);
    assert(sparse_mapped == (int64_t)sparse_start);
    assert(eager_range_prepares == prepares_before_sparse);
    assert(page_alloc_attempts == page_allocs_before_sparse);
    assert(sparse.address_space.vmas[0].used &&
           sparse.address_space.vmas[0].start == sparse_start &&
           sparse.address_space.vmas[0].end == sparse_start + sparse_length);
    assert(syscall_mm_munmap(sparse_start, sparse_length) == 0);
    assert(!sparse.address_space.vmas[0].used);
    assert(eager_range_prepares == prepares_before_sparse);
    assert(page_alloc_attempts == page_allocs_before_sparse);
    puts("PASS sparse 1 TiB mmap reservation: lazy page tables and bounded munmap traversal");

    /* Framebuffer CPU pointers are high direct-map aliases; an fbdev mmap
     * must install the backing PFN, never the alias bits, in the user PTE. */
    struct task fb_task = {0};
    fb_task.kind = TASK_KIND_USER;
    fb_task.stack_top = RELIEFNT_USER_TOP - page;
    fb_task.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    fb_task.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    current_task = &fb_task;
    memset(&framebuffer_file, 0, sizeof(framebuffer_file));
    framebuffer_file.flags = TASK_FILE_FLAG_DEV_NODE;
    framebuffer_file.node.first_cluster = STORAGE_DEV_KIND_FB0;
    memset(&framebuffer_device, 0, sizeof(framebuffer_device));
    framebuffer_device.available = true;
    framebuffer_device.pixels = (uint32_t *)(uintptr_t)framebuffer_alias;
    framebuffer_device.pitch = 4096;
    framebuffer_device.height = 2;
    framebuffer_device.max_bytes = 8192;
    const uint64_t fb_vaddr = 0x0000000030000000ULL;
    last_device_mapping_phys = 0;
    int64_t fb_mapped = syscall_mm_mmap(fb_vaddr, page,
        LINUX_PROT_READ | LINUX_PROT_WRITE,
        LINUX_MAP_SHARED | LINUX_MAP_FIXED_NOREPLACE, 7, page);
    if (fb_mapped != (int64_t)fb_vaddr)
        fprintf(stderr, "fbdev mmap returned %lld\n", (long long)fb_mapped);
    assert(fb_mapped == (int64_t)fb_vaddr);
    assert(last_device_mapping_phys == framebuffer_physical + page);
    assert((last_device_mapping_flags&(RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD))==
           (RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD));
    assert(last_device_mapping_phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE);
    puts("PASS fbdev mmap converts the high CPU alias to a physical PFN");

    /* CLONE_VM threads must observe the same process break, just as they
     * observe the same mappings. The local copy models the task state made
     * by clone before both tasks point at the promoted shared mm. */
    struct task process = {0};
    struct task thread;
    struct task_address_space_state shared_mm;
    process.kind = TASK_KIND_USER;
    process.stack_top = RELIEFNT_USER_TOP - page;
    process.limits.as.rlim_cur = LINUX_RLIM_INFINITY;
    process.limits.as.rlim_max = LINUX_RLIM_INFINITY;
    process.program_break_base = process.program_break = start;
    thread = process;
    shared_mm = process.address_space;
    process.shared_mm = thread.shared_mm = &shared_mm;

    current_task = &process;
    result = syscall_mm_brk(start + page);
    if (result != (int64_t)(start + page)) {
        fprintf(stderr, "shared brk growth returned %#llx, current=%#llx base=%#llx\n",
                (unsigned long long)result,
                (unsigned long long)process.program_break,
                (unsigned long long)process.program_break_base);
    }
    assert(result == (int64_t)(start + page));
    current_task = &thread;
    assert(syscall_mm_brk(0) == (int64_t)(start + page));
    puts("PASS brk: collision protection and CLONE_VM shared break");
    return 0;
}
