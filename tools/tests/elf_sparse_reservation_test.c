#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include "exec_file_fixture.h"

#include "../../kernel/exec/elf.c"
#define align_down paging_align_down
#include "../../arch/x86_64/mm/paging.c"
#undef align_down

#define SPARSE_ELF_BASE 0x0000010000000000ULL
#define SPARSE_ELF_SIZE (1ULL << 40)
#define TEST_PT_BUDGET 16u

static unsigned live_pages;
static unsigned page_alloc_attempts;
static unsigned page_budget = UINT32_MAX;
static struct { uint64_t physical; unsigned references; } page_registry[2048];

uint64_t mm_alloc_page(void)
{
    ++page_alloc_attempts;
    if (!page_budget) return 0;
    if (page_budget != UINT32_MAX) --page_budget;
    void *page = aligned_alloc(4096, 4096);
    assert(page);
    memset(page, 0, 4096);
    unsigned slot;
    for (slot = 0; slot < 2048; ++slot)
        if (!page_registry[slot].references) break;
    assert(slot < 2048);
    page_registry[slot].physical = (uint64_t)(uintptr_t)page;
    page_registry[slot].references = 1;
    ++live_pages;
    return (uint64_t)(uintptr_t)page;
}

void *mm_phys_to_virt(uint64_t physical)
{
    return (void *)(uintptr_t)physical;
}

uint64_t mm_virt_to_phys(const void *address)
{
    return (uint64_t)(uintptr_t)address;
}

void mm_retain_page(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i) {
        if (page_registry[i].physical == physical && page_registry[i].references) {
            ++page_registry[i].references;
            return;
        }
    }
    abort();
}

void mm_free_page(uint64_t physical)
{
    for (unsigned i = 0; i < 2048; ++i) {
        if (page_registry[i].physical == physical && page_registry[i].references) {
            if (!--page_registry[i].references) {
                page_registry[i].physical = 0;
                assert(live_pages);
                --live_pages;
                free((void *)(uintptr_t)physical);
            }
            return;
        }
    }
    abort();
}

int page_cache_retain(uint64_t physical) { (void)physical; return -1; }
int page_cache_owns(uint64_t physical) { (void)physical; return 0; }
void page_cache_release(uint64_t physical) { (void)physical; abort(); }
void x86_64_invlpg(uint64_t address) { (void)address; }
void smp_flush_user_tlb(void) { }

uint32_t sched_task_vma_capacity(const struct task *task)
{
    (void)task;
    return SCHED_TASK_VMA_MAX;
}

struct task_vma *sched_task_vma_at(struct task *task, uint32_t index)
{
    assert(index < SCHED_TASK_VMA_MAX);
    return &sched_task_mm(task)->vmas[index];
}

void *kernel_malloc(size_t size){return calloc(1,size);}
void kernel_free(void *memory){free(memory);}
static int exec_fixture_read(void *context,const struct storage_node *node,uint64_t offset,
                               void *buffer,uint32_t length,uint32_t *read,uint64_t mount_flags)
{(void)context;(void)node;(void)offset;(void)buffer;(void)length;(void)read;(void)mount_flags;abort();}

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level;
    (void)format;
}

int main(void)
{
    struct {
        struct elf64_ehdr eh;
        struct elf64_phdr ph;
    } image = {0};
    struct elf_image_info info = {0};
    struct storage_node node = {
        .type = RELIEFOS_FS_TYPE_FILE,
        .size = sizeof(image),
        .first_cluster = 1,
        .volume_id = 1,
    };
    struct task task = {0};
    struct address_space child = {0};
    struct vfs_exec_source *source=exec_fixture_open(&node);

    nx_enabled = true;
    task.kind = TASK_KIND_USER;
    task.stack_top = RELIEFNT_USER_TOP - 4096;
    assert(address_space_create(sched_task_as(&task)));

    image.eh.e_phoff = sizeof(image.eh);
    image.eh.e_phentsize = sizeof(image.ph);
    image.eh.e_phnum = 1;
    image.ph = (struct elf64_phdr){
        .p_type = PT_LOAD,
        .p_flags = PF_W,
        .p_offset = 0,
        .p_vaddr = SPARSE_ELF_BASE,
        .p_filesz = 0,
        .p_memsz = SPARSE_ELF_SIZE,
        .p_align = 4096,
    };
    unsigned attempts_before_load = page_alloc_attempts;
    unsigned pages_before_load = live_pages;
    page_budget = TEST_PT_BUDGET;
    int map_result = elf64_map_one(&task, source, &image, &info, 0, "sparse-test");
    page_budget = UINT32_MAX;
    if (map_result != 0 || page_alloc_attempts != attempts_before_load ||
        live_pages != pages_before_load) {
        fprintf(stderr, "large lazy ELF mapped=%d attempts=%u pages=%u\n",
                map_result, page_alloc_attempts - attempts_before_load,
                live_pages - pages_before_load);
    }
    assert(map_result == 0);
    assert(page_alloc_attempts == attempts_before_load);
    assert(live_pages == pages_before_load);
    assert(task.address_space.vmas[0].used);
    assert(task.address_space.vmas[0].exec_source==source);
    vfs_exec_source_put(source); /* Only the lazy VMA retains the provider now. */
    assert(exec_fixture_pins==2 && exec_fixture_denials==1);
    assert(task.address_space.vmas[0].start == SPARSE_ELF_BASE);
    assert(task.address_space.vmas[0].end == SPARSE_ELF_BASE + SPARSE_ELF_SIZE);
    assert(!address_space_user_page_phys(sched_task_as(&task), SPARSE_ELF_BASE));

    unsigned attempts_before_reject = page_alloc_attempts;
    image.ph = (struct elf64_phdr){
        .p_type = PT_LOAD, .p_flags = PF_W, .p_offset = 0,
        .p_vaddr = RELIEFNT_KERNEL_HOLE_START, .p_filesz = 0,
        .p_memsz = 4096, .p_align = 4096,
    };
    assert(elf64_map_one(&task, source, &image, &info, 0, "kernel-hole") < 0);
    assert(page_alloc_attempts == attempts_before_reject);
    assert(!task.address_space.vmas[1].used);

    image.ph.p_vaddr = RELIEFNT_USER_TOP - 4096;
    image.ph.p_memsz = 8192;
    assert(elf64_map_one(&task, source, &image, &info, 0, "canonical-overflow") < 0);
    assert(page_alloc_attempts == attempts_before_reject);
    assert(!task.address_space.vmas[1].used);

    /* The lazy reservation stays empty until a high branch is materialized.
     * Mapping, COW fork/fault, and teardown all operate on the sparse branch. */
    const uint64_t resident_va = SPARSE_ELF_BASE + (1ULL << 39) + 4096;
    uint64_t backing = mm_alloc_page();
    *(uint8_t *)mm_phys_to_virt(backing) = 0x5a;
    assert(address_space_map_user_page(sched_task_as(&task), resident_va, backing,
        RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_phys(sched_task_as(&task), resident_va) == backing);
    assert(address_space_clone_cow(sched_task_as(&task), &child));
    assert(address_space_user_page_phys(&child, resident_va) == backing);
    assert(address_space_handle_cow_fault(&child, resident_va));
    uint64_t child_private = address_space_user_page_phys(&child, resident_va);
    assert(child_private && child_private != backing);
    *(uint8_t *)mm_phys_to_virt(child_private) = 0xa5;
    assert(*(uint8_t *)mm_phys_to_virt(backing) == 0x5a);

    address_space_destroy(&child);
    address_space_destroy(sched_task_as(&task));
    vfs_exec_source_put(task.address_space.vmas[0].exec_source);
    assert(!exec_fixture_pins && !exec_fixture_inode_refs && !exec_fixture_denials);
    assert(!live_pages);
    puts("PASS sparse 1 TiB ELF BSS: no eager page tables; high map, COW fork/fault and destruction");
}
