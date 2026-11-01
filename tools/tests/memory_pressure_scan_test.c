/* Production page tables with host-backed RAM; no scheduler or disk needed. */
#define SWAP_PAGING_FIXTURE_ENTRY swap_paging_fixture_entry
#include "swap_paging_test.c"

int main(void)
{
    struct address_space as = {0}; nx_enabled = true;
    assert(address_space_create(&as));
    const uint64_t targets[] = {0x120000000ULL, 0x240000000ULL};
    for (unsigned i = 0; i < 2; ++i) {
        uint64_t frame = mm_alloc_page(); assert(frame);
        assert(address_space_map_user_page(&as, targets[i], frame,
                                           RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC));
    }
    uint64_t cursor = RELIEFNT_USER_BASE, found = 0;
    int rc = paging_user_pressure_step(&as, &cursor, &found);
    printf("first sparse inspection result=%d cursor=%llu first=%llu\n",
           rc, (unsigned long long)cursor, (unsigned long long)found); fflush(stdout);
    assert(rc == 1 && !found && cursor > RELIEFNT_USER_BASE && cursor <= targets[0]);
    unsigned candidates = 0, steps = 1;
    while ((rc = paging_user_pressure_step(&as, &cursor, &found)) != 0) {
        assert(rc == 1 || rc == 2);
        if (rc == 2) { assert(candidates < 2 && found == targets[candidates]); ++candidates; }
        assert(++steps < 5000);
    }
    assert(candidates == 2 && cursor == RELIEFNT_USER_TOP);
    address_space_destroy(&as); assert(!allocated);
    printf("PASS sparse pressure cursor found both pages in %u single steps\n", steps);
    return 0;
}
