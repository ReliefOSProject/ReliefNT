/* Reuse the sanitizer fixture's PMM, IRQ and allocation ownership checks. */
#define main reclaim_fixture_main
#include "memory_reclaim_test.c"
#undef main

static uint64_t index_visits;

/** @brief Count identity-index work without changing cache ownership.
 * @param kind Production test probe category.
 */
void page_cache_test_probe(unsigned kind)
{ if (kind == 1) ++index_visits; }

/** @brief Check residency with a capacity-independent index work bound.
 * @param key Borrowed query identity. @param expected Expected visibility.
 */
static void expect_inode(const struct storage_node *key, int expected)
{
    uint64_t before = index_visits;
    assert(!!page_cache_has_inode(key) == expected);
    assert(index_visits - before <= 32);
}

/** @brief Check sparse/overflow identities, retirement and clustered aliases.
 * @return Zero after all references, metadata and physical frames are released.
 */
int main(void)
{
    enum { COUNT = 6000 };
    uint64_t *frames = calloc(COUNT, sizeof(*frames));
    assert(frames);
    page_cache_init();
    expect_inode(NULL, 0);
    expect_inode(&node, 0);
    for (unsigned i = 0; i < COUNT; ++i) {
        struct storage_node key = node;
        key.inode_generation = i + 1;
        key.size = (COUNT + 1ULL) * 4096;
        struct request request = {.offset = (i + 1ULL) * 4096};
        assert(!page_cache_load(&key, request.offset, loader, &request, &frames[i]));
    }
    uint64_t before = index_visits;
    for (unsigned i = 0; i < COUNT; ++i) {
        struct storage_node key = node;
        key.inode_generation = i + 1;
        expect_inode(&key, 1); /* No cached page at offset zero. */
        key.mount_generation++;
        expect_inode(&key, 0);
    }
    printf("CACHE_INODE_INDEX queries=%u visits=%llu\n", COUNT * 2,
           (unsigned long long)(index_visits - before));
    assert(index_visits > before);

    struct storage_node key = node;
    key.inode_generation = COUNT;
    /* FAT incarnation survives a rename, cluster replacement and size change. */
    key.first_cluster += 19;
    key.entry_id += 27;
    key.size = 1;
    expect_inode(&key, 1);
    struct storage_node mismatch = key;
    mismatch.flags = STORAGE_NODE_FLAG_EXFAT;
    expect_inode(&mismatch, 0);
    mismatch = key; mismatch.volume_id++;
    expect_inode(&mismatch, 0);
    mismatch = key; mismatch.type = RELIEFOS_FS_TYPE_DIR;
    expect_inode(&mismatch, 0);
    mismatch = key; mismatch.inode_generation = 0;
    expect_inode(&mismatch, 0);

    /* Retired pinned overflow frames remain physically owned but invisible. */
    page_cache_forget_node(&key);
    expect_inode(&key, 0);
    assert(page_cache_owns(frames[COUNT - 1]));
    struct request request = {.offset = 4096};
    assert(!page_cache_load(&key, request.offset, loader, &request, &request.phys));
    expect_inode(&key, 1);
    page_cache_release(frames[COUNT - 1]);
    expect_inode(&key, 1);
    page_cache_forget_node(&key);
    expect_inode(&key, 0);
    page_cache_release(request.phys);

    /* Highest aligned offset and generationless fallback keys remain visible. */
    key.inode_generation = 0;
    request = (struct request){.offset = UINT64_MAX & ~4095ULL};
    assert(!page_cache_load(&key, request.offset, loader, &request, &request.phys));
    expect_inode(&key, 1);
    mismatch = key; mismatch.entry_id++;
    expect_inode(&mismatch, 0);
    mismatch = key; mismatch.first_cluster++;
    expect_inode(&mismatch, 0);
    page_cache_forget_node(&key);
    expect_inode(&key, 0);
    page_cache_release(request.phys);

    page_cache_forget_volume(node.volume_id);
    for (unsigned i = 0; i + 1 < COUNT; ++i) page_cache_release(frames[i]);
    assert(!atomic_load(&live_pages) && !atomic_load(&live_metadata));
    free(frames);
    puts("PASS inode index: sparse/overflow keys, misses, mutable aliases and pinned retirement");
    return 0;
}
