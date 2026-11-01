/* White-box boundary injection uses the unmodified production translation unit
 * and public operations, avoiding billions of retains in a host test. */
#define main cache_fixture_main
#include "memory_reclaim_test.c"
#undef main
#include "../../mm/cache/page_cache.c"

/** @brief Saturated references must never wrap into an evictable idle page.
 * @return Zero when the public retain, release, lookup and invalidate agree.
 */
int main(void)
{
    page_cache_init(); uint64_t phys=load(0), alias=0;
    struct page_cache_entry *entry=find_phys(phys); assert(entry);
    entry->refs=UINT32_MAX-1;
    assert(!page_cache_retain(phys)); assert(!page_cache_retain(phys));
    page_cache_release(phys);
    uint64_t other=load(1);
    assert(!page_cache_lookup(&node,0,&alias) && alias==phys);
    assert(!page_cache_reclaim(1));
    page_cache_release(alias);
    cache_serial=UINT64_MAX;
    struct request req={.offset=8192}; unsigned before=atomic_load(&live_pages);
    assert(page_cache_load(&node,req.offset,loader,&req,&req.phys)==PAGE_CACHE_FULL);
    assert(!req.phys && atomic_load(&live_pages)==before);
    check_page(1,other,1); page_cache_release(other);
    puts("PASS cache references saturate without becoming idle"); return 0;
}
