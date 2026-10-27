/* Reproduce musl pthread_create's PROT_NONE stack followed by mprotect.
 * Link the real growable scheduler VMA store and exercise actual paging under
 * ASan so both retained pointers and lazy-page access are checked. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <reliefnt/sched.h>
#include "../../kernel/sched/internal.h"
#define RELIEFNT_TEST_REAL_EXEC_SOURCE
#include "exec_file_fixture.h"

static struct task *current;
static struct task *test_current_task(void) { return current; }
#define sched_current_task test_current_task
#include "../../arch/x86_64/mm/paging.c"
#include "modules/user_memory.h"
#undef sched_current_task

static unsigned pages, allocations;
static unsigned user_tlb_flush_calls;
static uint64_t user_tlb_flush_probe_page;
static struct task_file *drm_test_file = (struct task_file *)(uintptr_t)0x2000;
static int fail_after = -1;
void *kernel_malloc(size_t size)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) --fail_after;
    ++allocations;
    void *memory = malloc(size);
    assert(memory);
    return memory;
}
void kernel_free(void *memory) { free(memory); }
uint64_t mm_alloc_page(void)
{
    void *memory = aligned_alloc(4096, 4096);
    assert(memory);
    memset(memory, 0, 4096);
    ++pages;
    return (uintptr_t)memory;
}
void *mm_phys_to_virt(uint64_t physical) { return (void *)(uintptr_t)physical; }
uint64_t mm_virt_to_phys(const void *address) { return (uint64_t)(uintptr_t)address; }
void mm_free_page(uint64_t page) { assert(pages); --pages; free((void *)(uintptr_t)page); }
int page_cache_owns(uint64_t page) { (void)page; return 0; }
void page_cache_release(uint64_t page) { (void)page; abort(); }
void x86_64_invlpg(uint64_t page) { (void)page; }
void smp_flush_user_tlb(void)
{
    ++user_tlb_flush_calls;
    if (user_tlb_flush_probe_page)
        assert(!address_space_user_page_writable(
            sched_task_as(current), user_tlb_flush_probe_page));
}
void storage_inode_retain(struct storage_inode_ref *inode) { assert(!inode); }
static int exec_fixture_read(void *context,const struct storage_node *node,uint64_t offset,
                               void *buffer,uint32_t length,uint32_t *read,uint64_t mount_flags)
{(void)context;(void)node;(void)offset;(void)buffer;(void)length;(void)read;(void)mount_flags;abort();}
struct task_file *task_file_get(struct task_file *description)
{ assert(!description || description == drm_test_file); return description; }
void task_file_put(struct task_file *description)
{ assert(!description || description == drm_test_file); }
void sysv_shm_vma_release(struct task *task, struct task_vma *vma)
{ (void)task; (void)vma; }
void sysv_shm_vma_split(struct task_vma *vma) { (void)vma; }
/* mremap also links its anonymous/file alternatives. Those providers are
 * absent here and must never be entered by the actual device-remap test. */
void console_log(enum console_log_level level,const char *format,...)
{(void)level;(void)format;abort();}
uint64_t mm_free_memory_kib(void){abort();}
struct task_file *task_file_for_io(struct task *task,int fd)
{(void)task;(void)fd;abort();}
int file_can_read(const struct task_file *file){(void)file;abort();}
int file_can_write(const struct task_file *file){(void)file;abort();}
void storage_set_io_async_context(bool enabled){(void)enabled;abort();}
const struct framebuffer *framebuffer_get(void){abort();}
int task_shm_mmap_check(const struct task_file *file,uint32_t flags,uint32_t prot,uint32_t *maximum)
{(void)file;(void)flags;(void)prot;(void)maximum;abort();}
static const uint64_t base = RELIEFNT_USER_BASE + 0x100000;

static void setup(unsigned extra)
{
    current = calloc(1, sizeof(*current));
    assert(current);
    current->kind = TASK_KIND_USER;
    nx_enabled = true;
    assert(address_space_create(sched_task_as(current)));
    unsigned count = SCHED_TASK_VMA_MAX + extra;
    for (unsigned i = 0; i < count; ++i) {
        struct task_vma *vma = sched_task_vma_at(current, i);
        assert(vma);
        *vma = (struct task_vma){.used = 1, .start = base + (i + 1) * 0x10000,
            .end = base + (i + 1) * 0x10000 + 4096,
            .flags = TASK_VMA_FLAG_ANON, .prot = LINUX_PROT_NONE,
            .max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE | LINUX_PROT_EXEC};
    }
    struct task_vma *target = sched_task_vma_at(current, count - 1);
    target->start = base;
    target->end = base + 5 * 4096;
}

static void teardown(void)
{
    sched_task_vma_release(current);
    address_space_destroy(sched_task_as(current));
    assert(!pages);
    free(current);
    current = NULL;
}

static void test_split(unsigned extra, unsigned first, unsigned last)
{
    setup(extra);
    uint64_t start = base + first * 4096, end = base + last * 4096;
    /* Leave len unaligned: Linux/POSIX apply protection through the last page. */
    assert(syscall_mm_mprotect(start, end - start - 1,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        assert(vma);
        bool writable = i >= first && i < last;
        assert(vma->prot == (writable ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_NONE));
        assert(syscall_handle_private_anon_fault(current, address + 17, 6) == writable);
        assert(address_space_user_page_writable(sched_task_as(current), address) == writable);
    }
    uint64_t phys = address_space_user_page_phys(sched_task_as(current), start);
    assert(phys);
    *(unsigned char *)(uintptr_t)phys = 42;
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_NONE) == 0);
    assert(!address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(syscall_mm_mprotect(start, end - start, LINUX_PROT_READ) == 0);
    assert(address_space_user_page_readable(sched_task_as(current), start));
    assert(!address_space_user_page_writable(sched_task_as(current), start));
    assert(*(unsigned char *)(uintptr_t)phys == 42);
    teardown();
}

static void test_allocation_failure(unsigned extra)
{
    setup(extra);
    uint32_t index = SCHED_TASK_VMA_MAX + extra - 1;
    struct task_vma saved = *sched_task_vma_at(current, index);
    fail_after = 0;
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == -RELIEFOS_ENOMEM);
    fail_after = -1;
    assert(!memcmp(&saved, sched_task_vma_at(current, index), sizeof(saved)));
    for (uint32_t i = 0; i < sched_task_vma_capacity(current); ++i)
        assert(sched_task_vma_at(current, i)->used != 0xffffffffu);
    assert(!syscall_handle_private_anon_fault(current, base + 4096, 6));
    assert(syscall_mm_mprotect(base + 1, 4096, LINUX_PROT_READ) == -RELIEFOS_EINVAL);
    assert(syscall_mm_mprotect(base - 4096, 4096, LINUX_PROT_READ) == -RELIEFOS_ENOMEM);
    sched_task_vma_at(current, index)->max_prot = LINUX_PROT_READ;
    assert(syscall_mm_mprotect(base, 5 * 4096, LINUX_PROT_WRITE) == -RELIEFOS_EACCES);
    teardown();
}

static void test_append_with_spare_capacity(void)
{
    setup(1);
    struct task_vma *store = current->vma_extra;
    unsigned before = allocations;
    assert(sched_task_vma_at(current, SCHED_TASK_VMA_MAX + 1));
    assert(current->vma_extra == store && allocations == before);
    assert(current->vma_extra_count == 2);
    teardown();
}

static void test_adjacent_vmas(void)
{
    setup(4);
    unsigned count = SCHED_TASK_VMA_MAX + 4;
    struct task_vma *left = sched_task_vma_at(current, count - 1);
    struct task_vma *right = sched_task_vma_at(current, count - 2);
    assert(left && right);
    left->end = base + 3 * 4096;
    right->start = left->end;
    right->end = base + 5 * 4096;
    right->prot = LINUX_PROT_READ;
    /* Linux accepts one mprotect range spanning two adjacent VMAs. */
    assert(syscall_mm_mprotect(base + 4096, 3 * 4096,
                              LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
    for (unsigned i = 0; i < 5; ++i) {
        uint64_t address = base + i * 4096;
        struct task_vma *vma = task_vma_containing(current, address, address + 4096);
        uint32_t expected = i == 0 ? LINUX_PROT_NONE :
                            i < 4 ? LINUX_PROT_READ | LINUX_PROT_WRITE : LINUX_PROT_READ;
        assert(vma && vma->prot == expected);
        bool writable = i > 0 && i < 4;
        assert(syscall_handle_private_anon_fault(current, address, 6) == writable);
    }
    teardown();
}

static void device_test_release(void *value){assert(value);}
static uint64_t device_test_physical(void *value,uint64_t offset){return *(uint64_t *)value+offset;}
static void test_drm_mprotect_flushes_remote_translations(void)
{
    setup(1);
    uint32_t last = sched_task_vma_capacity(current) - 1;
    struct task_vma *vma = sched_task_vma_at(current, last);
    assert(vma && vma->used);
    uint64_t physical = mm_alloc_page();
    vma->start = base;
    vma->end = base + 4096;
    vma->flags = TASK_VMA_FLAG_DEVICE | TASK_VMA_FLAG_SHARED |
                 TASK_VMA_FLAG_MMAP;
    vma->prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    vma->max_prot = LINUX_PROT_READ | LINUX_PROT_WRITE;
    static const struct device_mapping_operations operations={.release=device_test_release,.physical=device_test_physical};
    struct device_mapping_info info={.length=4096,.max_prot=3,.can_split=true,.can_move=true};
    assert(!device_mapping_create(NULL,&operations,&physical,&info,&vma->device_mapping));
    vma->device_physical = physical;
    assert(address_space_map_user_page(sched_task_as(current), base, physical,
           RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED |
           RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_writable(sched_task_as(current), base));

    user_tlb_flush_calls = 0;
    user_tlb_flush_probe_page = base;
    assert(syscall_mm_mprotect(base, 4096, LINUX_PROT_READ) == 0);
    assert(user_tlb_flush_calls == 1);
    assert(!address_space_user_page_writable(sched_task_as(current), base));
    assert(sched_task_vma_at(current, last)->prot == LINUX_PROT_READ);

    assert(syscall_mm_munmap(base, 4096) == 0);
    user_tlb_flush_probe_page = 0;
    mm_free_page(physical);
    teardown();
}

static void test_elf_source_split_and_clone(void)
{
    setup(0);
    struct storage_node node={.type=RELIEFOS_FS_TYPE_FILE,.size=6*4096,.first_cluster=1};
    struct vfs_exec_source *source=exec_fixture_open(&node);
    struct task_vma *vma=sched_task_vma_at(current,SCHED_TASK_VMA_MAX-1);
    *vma=(struct task_vma){.used=1,.start=base,.end=base+6*4096,
        .prot=LINUX_PROT_READ|LINUX_PROT_WRITE,.max_prot=LINUX_PROT_READ|LINUX_PROT_WRITE,
        .flags=TASK_VMA_FLAG_FILE|TASK_VMA_FLAG_LAZY|TASK_VMA_FLAG_PRIVATE,
        .file_node=node,.file_limit=node.size,.exec_source=source};
    assert(!syscall_mm_mprotect(base+4096,3*4096,LINUX_PROT_READ));
    vma=task_vma_containing(current,base+4096,base+2*4096);
    assert(vma && vma->exec_source==source);
    struct task_vma copy=*vma;copy.exec_source=NULL;
    assert(!sched_task_vma_retain_file(&copy,vma));
    assert(!syscall_mm_munmap(base,6*4096));
    assert(exec_fixture_pins==2 && exec_fixture_inode_refs==1 && exec_fixture_denials==1);
    task_vma_clear(&copy);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
    teardown();
    puts("PASS ELF source survives mprotect splits, cloned VMA and original unmap; last mapping releases provider and writer denial");
}

struct movable_device_test {uint64_t physical[3];unsigned releases;};
static void movable_device_release(void *value)
{
    struct movable_device_test *state=value;++state->releases;
    for(unsigned i=0;i<3;++i)mm_free_page(state->physical[i]);
}
static uint64_t movable_device_physical(void *value,uint64_t offset)
{return ((struct movable_device_test *)value)->physical[offset/4096];}
static void test_device_remap_preserves_actual_pages_and_shared_lease(void)
{
    setup(1);current->stack_top=RELIEFNT_USER_TOP-4096;
    sched_task_limits(current)->as.rlim_cur=LINUX_RLIM_INFINITY;
    struct movable_device_test state={0};
    for(unsigned i=0;i<3;++i)state.physical[i]=mm_alloc_page();
    static const struct device_mapping_operations operations={
        .release=movable_device_release,.physical=movable_device_physical};
    struct device_mapping_info info={.length=3*4096,.max_prot=3,.can_split=true,.can_move=true};
    struct task_vma *vma=sched_task_vma_at(current,sched_task_vma_capacity(current)-1);
    *vma=(struct task_vma){.used=1,.start=base,.end=base+3*4096,.prot=3,.max_prot=3,
        .flags=TASK_VMA_FLAG_DEVICE|TASK_VMA_FLAG_SHARED|TASK_VMA_FLAG_MMAP,
        .device_physical=state.physical[0]};
    assert(!device_mapping_create(NULL,&operations,&state,&info,&vma->device_mapping));
    struct task_vma clone=*vma;clone.device_mapping=NULL;
    assert(!sched_task_vma_retain_device(&clone,vma));
    for(unsigned i=0;i<3;++i)assert(address_space_map_user_page(sched_task_as(current),base+i*4096,
        state.physical[i],RELIEFNT_PAGE_DEVICE|RELIEFNT_PAGE_SHARED|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    uint64_t target=base+0x1000000;
    assert(syscall_mm_mremap(base,3*4096,4*4096,LINUX_MREMAP_MAYMOVE,0)==-RELIEFOS_EFAULT);
    assert(syscall_mm_mremap(base,3*4096,3*4096,LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED,target+1)==-RELIEFOS_EINVAL);
    assert(address_space_user_page_phys(sched_task_as(current),base)==state.physical[0]);
    assert(syscall_mm_mremap(base,3*4096,2*4096,LINUX_MREMAP_MAYMOVE|LINUX_MREMAP_FIXED,target)==(int64_t)target);
    assert(!address_space_user_page_phys(sched_task_as(current),base));
    for(unsigned i=0;i<2;++i)assert(address_space_user_page_phys(sched_task_as(current),target+i*4096)==state.physical[i]);
    assert(!syscall_mm_mprotect(target+4096,4096,LINUX_PROT_READ));
    assert(!syscall_mm_munmap(target,4096));
    vma=task_vma_containing(current,target+4096,target+2*4096);
    assert(vma && vma->device_map_offset==4096 && vma->device_physical==state.physical[1]);
    assert(!syscall_mm_munmap(target+4096,4096));assert(!state.releases);
    task_vma_clear(&clone);assert(state.releases==1);
    teardown();
    puts("PASS generic device mremap resolves each actual PFN, preserves source on invalid move, forbids expansion and shares final backing through cloned/split VMAs");
}

int main(void)
{
    for (unsigned extra = 1; extra <= 17; ++extra) {
        test_split(extra, 1, 5); /* musl stack guard: prefix split */
        test_split(extra, 0, 4); /* suffix split */
        test_split(extra, 1, 4); /* both sides, including a growth between slots */
    }
    test_allocation_failure(15); /* one spare append before the allocation fails */
    test_allocation_failure(16); /* allocation fails on the first needed slot */
    test_append_with_spare_capacity();
    test_adjacent_vmas();
    test_drm_mprotect_flushes_remote_translations();
    test_device_remap_preserves_actual_pages_and_shared_lease();
    test_elf_source_split_and_clone();
    puts("PASS mprotect: VMA growth, guard pages, lazy writes, page rounding, revoke/restore and errno");
    assert(allocations);
    return 0;
}
