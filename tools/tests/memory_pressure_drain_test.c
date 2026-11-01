/* Exercise real slot metadata with the existing memory-backed disk fixture. */
#define main swap_slots_fixture_main
#include "swap_slots_test.c"
#undef main

extern int swap_area_begin_drain(struct swap_area *) __attribute__((weak));
extern void swap_area_abort_drain(struct swap_area *) __attribute__((weak));
extern int swap_area_finish_drain(struct swap_area *) __attribute__((weak));

/** @brief Draining excludes allocation but preserves all existing slot bytes. */
int main(void)
{
    uint8_t h[4096];
    header(h);
    assert(!swap_area_create(h, sizeof(h), 5, 10, &first));
    assert(!swap_area_publish(first));
    assert(!swap_area_create(h, sizeof(h), 5, 1, &second));
    assert(!swap_area_publish(second));
    uint64_t token = swap_slot_alloc();
    assert(token);
    void *page = aligned_alloc(4096, 4096);
    assert(page);
    memset(page, 0x5a, 4096);
    assert(!swap_slot_submit_write(token, (uintptr_t)page));
    int rc = swap_area_begin_drain ? swap_area_begin_drain(first) : swap_area_unpublish(first);
    printf("begin live drain status=%d\n", rc);
    fflush(stdout);
    assert(!rc);
    assert(swap_area_begin_drain(first) == -EBUSY);
    uint64_t other = swap_slot_alloc();
    assert(other && (other >> 32) != (token >> 32));
    swap_slot_put(other);
    assert(swap_area_unpublish(first) == -EBUSY);
    memset(page, 0, 4096);
    assert(!swap_slot_submit_read(token, (uintptr_t)page));
    for (unsigned i = 0; i < 4096; ++i) assert(((uint8_t *)page)[i] == 0x5a);
    swap_area_abort_drain(first);
    uint64_t next = swap_slot_alloc();
    assert(next && (next >> 32) == (token >> 32));
    swap_slot_put(next);
    assert(!swap_area_begin_drain(first));
    assert(swap_area_finish_drain(first) == -EBUSY);
    swap_slot_put(token);
    assert(swap_area_unpublish(first) == -EBUSY);
    assert(!swap_area_finish_drain(first));
    assert(!swap_area_unpublish(second));
    swap_area_destroy(first);
    swap_area_destroy(second);
    free(page);
    assert(!live_allocations);
    puts("PASS live drain admission and failed-drain rollback preserve slot data");
    return 0;
}
