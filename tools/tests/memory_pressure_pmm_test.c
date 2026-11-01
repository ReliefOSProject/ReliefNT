/* Exercise emergency admission through the production physical allocator.
 * The existing physical fixture supplies a real topology and host-backed RAM. */
#define main physical_metadata_fixture_main
#include "physical_metadata_test.c"
#undef main

/** @brief Ordinary allocations leave an emergency page reserve intact. */
static void ordinary_reserve(void)
{
    map_ram(LOW, 512);
    struct multiboot2_mmap_entry entry = {.addr = LOW, .len = 512 * PAGE, .type = 1};
    boot_memory(&entry, 1, 0);
    uint64_t initial = mm_free_memory_kib() / 4;
    assert(initial > 128);
    uint64_t *pages = calloc((size_t)initial, sizeof(*pages));
    assert(pages);
    uint64_t count = 0, page;
    while ((page = mm_alloc_page()) != 0) {
        assert(count < initial);
        pages[count++] = page;
    }
    uint64_t remaining = mm_free_memory_kib() / 4;
    printf("ordinary allocation: initial=%llu claimed=%llu remaining=%llu\n",
           (unsigned long long)initial, (unsigned long long)count,
           (unsigned long long)remaining);
    fflush(stdout);
    assert(mm_emergency_reserve_pages() == 64 && remaining == 64);
    assert(count + remaining == initial);
    assert(!mm_alloc_pages_below(1, UINT64_MAX));
    uint64_t emergency = mm_alloc_emergency_page();
    assert(emergency && mm_free_memory_kib() / 4 == remaining - 1);
    assert(!mm_alloc_page() && !mm_alloc_pages_below(1, UINT64_MAX));
    mm_free_page(emergency);
    assert(mm_free_memory_kib() / 4 == remaining);
    for (uint64_t i = 0; i < count; ++i) mm_free_page(pages[i]);
    assert(mm_free_memory_kib() / 4 == initial);
    free(pages);
}

/** @brief A small physical pool keeps one quarter for emergency recovery. */
static void small_reserve(void)
{
    map_ram(LOW, 64);
    struct multiboot2_mmap_entry entry = {.addr = LOW, .len = 64 * PAGE, .type = 1};
    boot_memory(&entry, 1, 0);
    uint64_t initial = mm_free_memory_kib() / 4;
    uint64_t reserve = mm_emergency_reserve_pages();
    assert(reserve == initial / 4 && reserve < 64 && reserve > 0);
    uint64_t pages[64], count = 0, page;
    while ((page = mm_alloc_page()) != 0) pages[count++] = page;
    assert(mm_free_memory_kib() / 4 == reserve && count + reserve == initial);
    while ((page = mm_alloc_emergency_page()) != 0) pages[count++] = page;
    assert(count == initial && !mm_free_memory_kib());
    for (uint64_t i = 0; i < count; ++i) mm_free_page(pages[i]);
    assert(mm_free_memory_kib() / 4 == initial);
    printf("small pool: initial=%llu reserve=%llu, all pages restored\n",
           (unsigned long long)initial, (unsigned long long)reserve);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "ordinary")) ordinary_reserve();
    else if (!strcmp(argv[1], "small")) small_reserve();
    else assert(!"unknown fixture case");
    puts("PASS emergency reserve admission");
    return 0;
}
