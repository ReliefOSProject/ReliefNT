/* Exercise the real cache and count eligibility inspections, not elapsed time. */
#define main memory_reclaim_fixture_main
#include "memory_reclaim_test.c"
#undef main

extern uint32_t page_cache_pressure_scan(uint32_t budget, uint32_t *inspected,
                                         bool *cycle_complete) __attribute__((weak));
static uintptr_t watched_function;
static uint64_t inspections;
static uint64_t retained_frame;

void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void *callee, void *caller)
{
    (void)caller;
    if ((uintptr_t)callee == watched_function) ++inspections;
}

void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void *callee, void *caller)
{ (void)callee; (void)caller; }

/** @brief Release an owned fixture frame and report the actual final release. */
bool mm_free_page_reclaimed(uint64_t physical)
{
    if (!physical) return false;
    if (physical == retained_frame) return false;
    mm_free_page(physical);
    return true;
}

/** @brief Check the cursor when its current overflow chunk is retired. */
static void overflow_retirement(void)
{
    page_cache_init();
    node.size = (SLOTS + 65ULL) * 4096ULL;
    uint64_t *pages = fill();
    uint64_t extra[65];
    for (unsigned i = 0; i < 65; ++i) extra[i] = load(SLOTS + i);
    for (unsigned batch = 0; batch < SLOTS / 32; ++batch) {
        uint32_t inspected = 0;
        bool complete = false;
        assert(!page_cache_pressure_scan(32, &inspected, &complete));
        assert(inspected == 32 && !complete);
    }
    /* The last extra page is alone in the head chunk currently in the cursor. */
    page_cache_release(extra[64]);
    uint32_t inspected = 0;
    bool complete = false;
    assert(!page_cache_pressure_scan(32, &inspected, &complete));
    assert(inspected == 32 && !complete);
    for (unsigned i = 0; i < 64; ++i) page_cache_release(extra[i]);
    assert(!page_cache_pressure_scan(32, &inspected, &complete));
    assert(!inspected && complete);
    for (unsigned i = 0; i < SLOTS; ++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node);
    assert(!atomic_load(&live_pages));
    free(pages);
    puts("PASS bounded cursor survives overflow retirement");
}

/** @brief Prove a clean cache candidate cannot consume an unbounded batch. */
static void bounded_scan(uintptr_t target, bool pinned)
{
    page_cache_init();
    uint64_t *pages = fill();
    page_cache_release(pages[SLOTS - 1]);
    if (pinned) retained_frame = pages[SLOTS - 1];
    unsigned old_loads = atomic_load(&loads);
    uint32_t total = 0, batches = 0;
    bool complete = false;
    watched_function = target;
    while (!complete && batches++ < SLOTS / 32 + 2) {
        uint32_t inspected = 0;
        uint64_t before = inspections;
        uint32_t reclaimed;
        if (page_cache_pressure_scan) {
            reclaimed = page_cache_pressure_scan(32, &inspected, &complete);
        } else {
            /* Before the new API exists, this is the production clean-cache
             * reclaim available to a caller. It must fail the work bound. */
            reclaimed = (uint32_t)page_cache_reclaim(1);
            inspected = (uint32_t)(inspections - before);
            complete = true;
        }
        uint64_t calls = inspections - before;
        printf("cache batch=%u inspected=%u predicate_calls=%llu reclaimed=%u\n",
               batches, inspected, (unsigned long long)calls, reclaimed);
        fflush(stdout);
        assert(inspected <= 32 && calls <= 32);
        total += reclaimed;
    }
    watched_function = 0;
    assert(complete && total == (pinned ? 0u : 1u) && atomic_load(&loads) == old_loads);
    if (pinned) {
        assert(*(uint8_t *)(uintptr_t)retained_frame == (SLOTS - 1) % 251);
        mm_free_page(retained_frame);
        retained_frame = 0;
    }
    for (unsigned i = 0; i < SLOTS - 1; ++i) page_cache_release(pages[i]);
    page_cache_invalidate_node(&node);
    assert(!atomic_load(&live_pages));
    free(pages);
    puts("PASS bounded clean-cache inspection and actual reclamation");
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    uintptr_t target = (uintptr_t)strtoull(argv[1], NULL, 16);
    assert(target);
    if (!strcmp(argv[2], "overflow")) {
        assert(page_cache_pressure_scan);
        overflow_retirement();
    } else {
        assert(!strcmp(argv[2], "normal") || !strcmp(argv[2], "pinned"));
        bounded_scan(target, !strcmp(argv[2], "pinned"));
    }
    return 0;
}
