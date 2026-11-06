/* SPDX-License-Identifier: Apache-2.0 */
#define main paging_fixture_main
#include "paging_protection_test.c"
#undef main

uint64_t mm_free_memory_kib(void) { return 4096; }

int main(void)
{
    assert(paging_fixture_main() == 0);
    struct address_space parent = {0}, child = {0};
    const uint64_t va = RELIEFNT_USER_BASE;
    assert(address_space_create(&parent));
    uint64_t physical = mm_alloc_page();
    assert(address_space_map_user_page(&parent, va, physical,
                                       RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC));
    assert(address_space_clone_cow(&parent, &child));
    assert(!address_space_retry_mapped_user_page(&parent, va, 7));
    /* Both siblings faulted on a read-only COW leaf. The first handler has
     * already published a private writable leaf when the second runs. */
    assert(address_space_handle_cow_fault(&parent, va));
    assert(!address_space_handle_cow_fault(&parent, va));
    assert(address_space_retry_mapped_user_page(&parent, va, 7));
    assert(!address_space_retry_mapped_user_page(&child, va, 7));
    assert(!address_space_retry_mapped_user_page(&parent, va, 15));
    assert(!address_space_retry_mapped_user_page(&parent, va, 0x15));
    assert(!address_space_retry_mapped_user_page(&parent, va, 0x27));
    uint64_t branch = parent.pml4[(va >> 39) & 511u];
    parent.pml4[(va >> 39) & 511u] &= ~RELIEFNT_PAGE_WRITABLE;
    assert(!address_space_retry_mapped_user_page(&parent, va, 7));
    parent.pml4[(va >> 39) & 511u] = branch;
    assert(address_space_protect_user_page(&parent, va, RELIEFNT_PAGE_NOEXEC));
    assert(!address_space_retry_mapped_user_page(&parent, va, 7));
    assert(address_space_protect_user_page(&parent, va,
                                           RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PROTNONE));
    assert(!address_space_retry_mapped_user_page(&parent, va, 7));
    address_space_destroy(&parent);
    address_space_destroy(&child);
    for (unsigned i = 0; i < 1024; ++i) assert(!pages[i].refs);
    puts("PASS stale COW fault retry preserves read-only, NX and PROT_NONE protection");
    return 0;
}
