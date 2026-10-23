#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int allocation_fail_after = -1;
static unsigned allocation_attempts;

static int test_page_alloc_allowed(void)
{
    ++allocation_attempts;
    if (allocation_fail_after < 0) return 1;
    return allocation_fail_after-- > 0;
}

#ifndef RELIEFNT_TEST_PAGE_ALLOC_GATE
#define RELIEFNT_TEST_PAGE_ALLOC_GATE() test_page_alloc_allowed()
#endif

/* Exercise the real page-table implementation; only physical allocation and
 * privileged CPU operations are replaced by host fixtures. */
#include "../../arch/x86_64/mm/paging.c"

static struct { uint64_t address; unsigned refs; void *host; } pages[1024];

uint64_t mm_alloc_page(void)
{
#ifdef RELIEFNT_TEST_PAGE_ALLOC_GATE
    if (!RELIEFNT_TEST_PAGE_ALLOC_GATE()) return 0;
#endif
    for (unsigned i = 0; i < 1024; ++i) if (!pages[i].refs) {
        void *p = aligned_alloc(4096, 4096);
        assert(p);
        memset(p, 0, 4096);
        pages[i].address = (uint64_t)(i + 1u) * 4096u;
        pages[i].refs = 1;
        pages[i].host = p;
        return pages[i].address;
    }
    abort();
}

void *mm_phys_to_virt(uint64_t p)
{
    uintptr_t early_start = (uintptr_t)&mmio_early_pt_tables[0][0];
    uint64_t early_bytes = sizeof(mmio_early_pt_tables);
    if (p >= early_start && p - early_start < early_bytes)
        return (uint8_t *)mmio_early_pt_tables + (p - early_start);
    for (unsigned i = 0; i < 1024; ++i)
        if (pages[i].refs && p >= pages[i].address && p - pages[i].address < 4096)
            return (uint8_t *)pages[i].host + (p - pages[i].address);
    return NULL;
}

uint64_t mm_virt_to_phys(const void *address)
{
    for (unsigned i = 0; i < 1024; ++i) {
        uintptr_t p = (uintptr_t)address;
        uintptr_t start = (uintptr_t)pages[i].host;
        if (pages[i].refs && p >= start && p - start < 4096)
            return pages[i].address + (p - start);
    }
    return 0;
}

void mm_retain_page(uint64_t p)
{
    for (unsigned i = 0; i < 1024; ++i) if (pages[i].address == p && pages[i].refs) {
        ++pages[i].refs;
        return;
    }
    abort();
}

void mm_free_page(uint64_t p)
{
    for (unsigned i = 0; i < 1024; ++i) if (pages[i].address == p && pages[i].refs) {
        if (!--pages[i].refs) free(pages[i].host);
        return;
    }
    abort();
}

int page_cache_retain(uint64_t p) { (void)p; return -1; }
int page_cache_owns(uint64_t p) { (void)p; return 0; }
void page_cache_release(uint64_t p) { (void)p; abort(); }
void x86_64_load_cr3(uint64_t p) { (void)p; }
void smp_flush_user_tlb(void) { }
void x86_64_invlpg(uint64_t p) { (void)p; }
#ifndef RELIEFNT_PAGING_FIXTURE_EXTERNAL_SPINLOCKS
void kernel_spin_lock(struct kernel_spinlock *lock) { (void)lock; }
void kernel_spin_unlock(struct kernel_spinlock *lock) { (void)lock; }
#endif

/** @brief Verify fork COW isolates writes by both parent and child.
 * @return None. Assertions fail when a present parent leaf is not write-protected.
 */
static void test_parent_cow_write_isolation(void)
{
    struct address_space parent = {0}, child = {0};
    const uint64_t va = RELIEFNT_USER_BASE;
    uint64_t shared_phys, protnone_phys;

    assert(address_space_create(&parent));
    shared_phys = mm_alloc_page();
    protnone_phys = mm_alloc_page();
    assert(shared_phys && protnone_phys);
    *(uint8_t *)mm_phys_to_virt(shared_phys) = 0x42;
    *(uint8_t *)mm_phys_to_virt(protnone_phys) = 0x17;
    assert(address_space_map_user_page(&parent, va, shared_phys,
                                       RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_map_user_page(&parent, va + PAGE_SIZE, protnone_phys,
                                       RELIEFNT_PAGE_PROTNONE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_clone_cow(&parent, &child));

    uint64_t parent_flags = address_space_user_page_flags(&parent, va);
    uint64_t child_flags = address_space_user_page_flags(&child, va);
    assert(!(parent_flags & RELIEFNT_PAGE_WRITABLE) &&
           (parent_flags & RELIEFNT_PAGE_COW));
    assert(!(child_flags & RELIEFNT_PAGE_WRITABLE) &&
           (child_flags & RELIEFNT_PAGE_COW));
    assert(address_space_user_page_phys(&parent, va) == shared_phys);
    assert(address_space_user_page_phys(&child, va) == shared_phys);

    uint64_t protnone_va = va + PAGE_SIZE;
    assert(address_space_user_page_phys(&parent, protnone_va) == protnone_phys);
    assert(address_space_user_page_phys(&child, protnone_va) == protnone_phys);
    assert((address_space_user_page_flags(&parent, protnone_va) &
            RELIEFNT_PAGE_PROTNONE) != 0);
    assert((address_space_user_page_flags(&child, protnone_va) &
            RELIEFNT_PAGE_PROTNONE) != 0);
    assert(!address_space_user_page_readable(&parent, protnone_va));
    assert(!address_space_user_page_readable(&child, protnone_va));

    assert(address_space_handle_cow_fault(&parent, va));
    uint64_t parent_private = address_space_user_page_phys(&parent, va);
    assert(parent_private != shared_phys);
    *(uint8_t *)mm_phys_to_virt(parent_private) = 0x91;
    assert(*(uint8_t *)mm_phys_to_virt(shared_phys) == 0x42);

    assert(address_space_handle_cow_fault(&child, va));
    uint64_t child_private = address_space_user_page_phys(&child, va);
    assert(child_private != shared_phys);
    *(uint8_t *)mm_phys_to_virt(child_private) = 0x2a;
    assert(*(uint8_t *)mm_phys_to_virt(parent_private) == 0x91);
    assert(*(uint8_t *)mm_phys_to_virt(child_private) == 0x2a);

    address_space_destroy(&child);
    address_space_destroy(&parent);
}

static void test_early_mmio_split_uses_reserved_tables(void)
{
    const uint64_t phys = 0x40403000ULL;
    const uint64_t exhausted_phys = 0x40603000ULL;
    const uint64_t dynamic_phys = 0x40803000ULL;
    uint64_t *pte;
    kernel_pd[1][2] = 0x40400000ULL | RELIEFNT_PAGE_PRESENT |
                      RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG;
    allocation_fail_after = 0;
    unsigned before = allocation_attempts;
    assert(paging_mmio_uncached(phys, PAGE_SIZE));
    allocation_fail_after = -1;
    assert(allocation_attempts == before);
    assert(!(kernel_pd[1][2] & PAGE_SIZE_FLAG));
    pte = mmio_existing_leaf(phys);
    assert(pte && (*pte & RELIEFNT_PHYS_ADDR_MASK) == phys);
    assert((*pte & (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD |
                    RELIEFNT_PAGE_NOEXEC)) ==
           (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD |
            RELIEFNT_PAGE_NOEXEC));

    uint32_t early_used = mmio_early_pt_table_count;
    kernel_pd[1][3] = 0x40600000ULL | RELIEFNT_PAGE_PRESENT |
                      RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG;
    mmio_early_pt_table_count = MMIO_EARLY_PT_TABLE_COUNT;
    allocation_fail_after = 0;
    before = allocation_attempts;
    assert(!paging_mmio_uncached(exhausted_phys, PAGE_SIZE));
    allocation_fail_after = -1;
    assert(allocation_attempts == before);
    assert(kernel_pd[1][3] == (0x40600000ULL | RELIEFNT_PAGE_PRESENT |
                               RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG));
    mmio_early_pt_table_count = early_used;

    kernel_pd[1][4] = 0x40800000ULL | RELIEFNT_PAGE_PRESENT |
                      RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG;
    paging_mmio_allocator_ready();
    before = allocation_attempts;
    assert(paging_mmio_uncached(dynamic_phys, PAGE_SIZE));
    assert(allocation_attempts == before + 1);
    assert(!(kernel_pd[1][4] & PAGE_SIZE_FLAG));
    uint64_t dynamic_table = kernel_pd[1][4] & RELIEFNT_PHYS_ADDR_MASK;
    mm_free_page(dynamic_table);
    kernel_pd[1][4] = 0x40800000ULL | RELIEFNT_PAGE_PRESENT |
                      RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG;
}

int main(void)
{
    struct address_space parent = {0}, child = {0};
    const uint64_t va = RELIEFNT_USER_BASE;
    const uint64_t two_meg_boundary = RELIEFNT_USER_BASE + 0x200000ULL;
    const uint64_t far_va = 0x0000010000000000ULL;
    const uint64_t last_user_page = RELIEFNT_USER_TOP - 4096ULL;
    uint64_t hole_pdpt_phys, hole_pd_phys, hole_pt_phys;
    uint64_t *hole_pdpt, *hole_pd, *hole_pt;
    nx_enabled = true;

    /* Keep only the mapped supervisor kernel hole from the bootstrap's low
     * identity hierarchy. Ordinary low user addresses must not inherit its
     * present supervisor identity leaves, while kernel code at 128 MiB must
     * stay reachable after switching to the user CR3. */
    hole_pdpt_phys = mm_alloc_page();
    hole_pd_phys = mm_alloc_page();
    hole_pt_phys = mm_alloc_page();
    hole_pdpt = mm_phys_to_virt(hole_pdpt_phys);
    hole_pd = mm_phys_to_virt(hole_pd_phys);
    hole_pt = mm_phys_to_virt(hole_pt_phys);
    assert(hole_pdpt && hole_pd && hole_pt);
    hole_pdpt[0] = hole_pd_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    hole_pd[64] = hole_pt_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    hole_pt[0] = 0x08000000ULL | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    kernel_pml4[0] = 0x9003ULL; /* Full bootstrap low identity map. */
    kernel_pml4[1] = 0xa003ULL; /* Other low-half kernel mappings are omitted. */
    kernel_pml4[511] = 0x2003ULL;
    kernel_hole_pml4e = hole_pdpt_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    assert(address_space_create(&parent));
    assert(parent.pml4[0] == kernel_hole_pml4e);
    assert(parent.pml4[0] != kernel_pml4[0]);
    assert(!(parent.pml4[0] & RELIEFNT_PAGE_USER));
    assert(parent.pml4[1] == 0);
    assert(parent.pml4[511] == kernel_pml4[511]);
    assert(!(hole_pdpt[0] & RELIEFNT_PAGE_USER));
    assert(!(hole_pd[64] & RELIEFNT_PAGE_USER));
    assert(!(hole_pt[0] & RELIEFNT_PAGE_USER));

    /* A high CPU alias is not a physical frame number. Mapping it into a
     * user PTE would copy its canonical/reserved bits into the hardware PFN. */
    assert(!address_space_map_user_page(&parent, RELIEFNT_USER_BASE,
        0xffffff8000420000ULL, RELIEFNT_PAGE_NOEXEC));
    assert(!address_space_user_page_phys(&parent, RELIEFNT_USER_BASE));

    /* A single prepared mapping range may cross a 2 MiB PDE boundary. */
    assert(address_space_prepare_user_range(&parent, two_meg_boundary - 4096,
                                            two_meg_boundary + 4096));
    uint64_t boundary_page = mm_alloc_page();
    assert(address_space_map_user_page(&parent, two_meg_boundary - 4096,
                                       boundary_page,
                                       RELIEFNT_PAGE_NOEXEC));
    uint64_t boundary_page2 = mm_alloc_page();
    assert(address_space_map_user_page(&parent, two_meg_boundary,
                                       boundary_page2,
                                       RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_phys(&parent, two_meg_boundary - 4096) ==
           boundary_page);
    assert(address_space_user_page_phys(&parent, two_meg_boundary) ==
           boundary_page2);

    /* Sparse mappings must reach other PML4 slots and the canonical user edge. */
    uint64_t far_page = mm_alloc_page();
    assert(address_space_map_user_page(&parent, far_va, far_page,
                                       RELIEFNT_PAGE_NOEXEC));
    uint64_t edge_page = mm_alloc_page();
    assert(address_space_map_user_page(&parent, last_user_page, edge_page,
                                       RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_phys(&parent, far_va) == far_page);
    assert(address_space_user_page_phys(&parent, last_user_page) == edge_page);

    /* Sparse range queries must walk existing tables rather than scan a
     * 1 TiB reservation one page at a time. */
    uint64_t found_page = 0, found_phys = 0, found_flags = 0;
    assert(address_space_user_page_next(&parent, RELIEFNT_USER_BASE,
                                        0x0000000000800000ULL, &found_page,
                                        &found_phys, &found_flags));
    assert(found_page == two_meg_boundary - 4096);
    assert(found_phys == boundary_page);
    assert(found_flags & RELIEFNT_PAGE_USER);
    unsigned range_query_allocations = allocation_attempts;
    assert(address_space_user_range_free(&parent, 0x0000040000000000ULL,
                                         0x0000050000000000ULL));
    assert(allocation_attempts == range_query_allocations);
    assert(!address_space_user_range_free(&parent, far_va,
                                          last_user_page + 4096));
    assert(address_space_user_page_next(&parent, far_va + 4096,
                                        last_user_page + 4096, &found_page,
                                        &found_phys, NULL));
    assert(found_page == last_user_page);
    assert(found_phys == edge_page);

    /* A failed multi-level preparation leaves no partial hierarchy behind. */
    unsigned pages_before_failure = 0;
    for (unsigned i = 0; i < 1024; ++i) pages_before_failure += pages[i].refs != 0;
    allocation_attempts = 0;
    allocation_fail_after = 1;
    assert(!address_space_prepare_user_range(&parent, 0x0000020000000000ULL,
                                             0x0000020000001000ULL));
    allocation_fail_after = -1;
    assert(allocation_attempts == 2);
    unsigned pages_after_failure = 0;
    for (unsigned i = 0; i < 1024; ++i) pages_after_failure += pages[i].refs != 0;
    assert(pages_after_failure == pages_before_failure);
    assert(!address_space_user_page_phys(&parent, 0x0000020000000000ULL));

    uint64_t backing = mm_alloc_page();
    *(unsigned char *)mm_phys_to_virt(backing) = 42;
    assert(address_space_map_user_page(&parent, va, backing,
                                      RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE));
    assert(address_space_user_page_phys(&parent, va) == backing);
    assert(!address_space_user_page_readable(&parent, va));
    assert(!address_space_user_page_writable(&parent, va));
    assert(!address_space_handle_cow_fault(&parent, va));
    assert(!address_space_map_user_page(&parent, va, backing, RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_resident_kib(&parent) == 20);

    /* A late child-table allocation failure must release pages already
     * retained by the partially cloned child and leave the parent untouched. */
    unsigned refs_before_clone_failure = 0;
    for (unsigned i = 0; i < 1024; ++i) refs_before_clone_failure += pages[i].refs;
    uint64_t parent_flags_before_clone_failure =
        address_space_user_page_flags(&parent, va);
    allocation_attempts = 0;
    allocation_fail_after = 5;
    assert(!address_space_clone_cow(&parent, &child));
    allocation_fail_after = -1;
    assert(allocation_attempts == 6);
    assert(!child.pml4 && !child.cr3 && !child.user_page_count);
    assert(address_space_user_page_flags(&parent, va) ==
           parent_flags_before_clone_failure);
    unsigned refs_after_clone_failure = 0;
    for (unsigned i = 0; i < 1024; ++i) refs_after_clone_failure += pages[i].refs;
    assert(refs_after_clone_failure == refs_before_clone_failure);

    uint64_t device = mm_alloc_page();
    assert(address_space_map_user_page(&parent, va + 4096, device,
                                      RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_resident_kib(&parent) == 20);
    assert(address_space_unmap_user_page(&parent, va + 4096) == device);
    mm_free_page(device);
    assert(address_space_clone_cow(&parent, &child));
    assert(child.user_page_count == parent.user_page_count);
    assert(address_space_user_page_phys(&child, far_va) == far_page);
    assert(address_space_user_page_phys(&child, last_user_page) == edge_page);
    assert(!address_space_user_page_readable(&child, va));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_readable(&child, va));
    assert(address_space_handle_cow_fault(&child, va));
    uint64_t private_page = address_space_user_page_phys(&child, va);
    assert(private_page != backing && *(unsigned char *)mm_phys_to_virt(private_page) == 42);
    *(unsigned char *)mm_phys_to_virt(private_page) = 17;
    assert(*(unsigned char *)mm_phys_to_virt(backing) == 42);
    assert(address_space_protect_user_page(&child, va, RELIEFNT_PAGE_NOEXEC));
    assert(!address_space_handle_cow_fault(&child, va));
    assert(!address_space_user_page_writable(&child, va));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE));
    assert(!address_space_user_page_readable(&child, va));
    assert(address_space_unmap_user_page(&child, va) == private_page);
    mm_free_page(private_page);
    assert(child.user_page_count == 4);
    assert(address_space_user_resident_kib(&child) == 16);
    address_space_destroy(&child);
    address_space_destroy(&parent);
    assert(address_space_create(&parent));
    backing = mm_alloc_page();
    assert(address_space_map_user_page(&parent, va, backing,
                                      RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE |
                                      RELIEFNT_PAGE_SHARED));
    assert(address_space_clone_cow(&parent, &child));
    assert(address_space_protect_user_page(&child, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_writable(&child, va));
    assert(!address_space_handle_cow_fault(&child, va));
    assert(address_space_user_page_phys(&child, va) == backing);
    *(unsigned char *)mm_phys_to_virt(backing) = 61;
    assert(address_space_protect_user_page(&parent, va,
                                          RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_user_page_writable(&parent, va));
    assert(*(unsigned char *)mm_phys_to_virt(address_space_user_page_phys(&parent, va)) == 61);
    address_space_destroy(&parent);
    assert(*(unsigned char *)mm_phys_to_virt(address_space_user_page_phys(&child, va)) == 61);
    address_space_destroy(&child);
    test_parent_cow_write_isolation();
    test_early_mmio_split_uses_reserved_tables();
    kernel_hole_pml4e = 0;
    mm_free_page(hole_pt_phys);
    mm_free_page(hole_pd_phys);
    mm_free_page(hole_pdpt_phys);
    for (unsigned i = 0; i < 1024; ++i) assert(!pages[i].refs);
    puts("PASS page protection: PROT_NONE, private COW, shared fork, restore, unmap, ownership");
}
