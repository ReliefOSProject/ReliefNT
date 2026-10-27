/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../fs/ext_common/ext2_cache.c"
#include "../../fs/ext_common/cache.c"

/* Only physical allocation and its CPU alias are emulated. No disk, parser,
 * journal or VFS success is mocked: this test exercises the actual cache pages,
 * unfinished-transaction guard and retirement under caller-provided exclusion. */
static struct {uint64_t physical;unsigned char *cpu;uint32_t pages;} runs[8];
static uint64_t next_physical=0x100000;
static unsigned allocated,freed;
static bool allocation_failure,mapping_failure;
uint64_t mm_alloc_pages(uint32_t pages)
{
    if(allocation_failure)return 0;
    for(unsigned i=0;i<8;++i)if(!runs[i].cpu) {
        runs[i].cpu=calloc(pages,4096);assert(runs[i].cpu);runs[i].pages=pages;
        runs[i].physical=next_physical;next_physical+=(uint64_t)pages*4096;
        ++allocated;return runs[i].physical;
    }
    abort();
}
void *mm_phys_to_virt(uint64_t physical)
{
    if(mapping_failure)return NULL;
    for(unsigned i=0;i<8;++i)if(runs[i].cpu && physical==runs[i].physical)return runs[i].cpu;
    return NULL;
}
void mm_free_pages(uint64_t physical,uint32_t pages)
{
    for(unsigned i=0;i<8;++i)if(runs[i].cpu && runs[i].physical==physical) {
        assert(pages==runs[i].pages);free(runs[i].cpu);runs[i].cpu=NULL;++freed;return;
    }
    abort();
}

int main(void)
{
    unsigned char input[4096],output[4096];memset(input,0x5a,sizeof(input));
    for(unsigned cycle=0;cycle<32;++cycle) {
        g_active_volume=&g_volumes[0];g_active_volume->mount_generation=cycle+1;
        ext2_cache_store(7,8,input);assert(ext2_cache_read(7,8,output));
        assert(!memcmp(input,output,sizeof(input)));
        assert(!ext2_cache_release() && !ext2_cache);
        assert(!ext2_cache_read(7,8,output));
        for(unsigned t=0;t<EXT4_CACHE_TABLE_COUNT;++t)assert(ext4_cache_ensure(&ext4_cache_tables[t]));
        for(unsigned t=0;t<EXT4_CACHE_TABLE_COUNT;++t) {
            struct storage_ext4_cache_entry *entry=&ext4_cache_tables[t].entries[0];
            entry->volume=g_active_volume;entry->valid=1;entry->dirty=1;
            unsigned before=freed;
            assert(storage_ext4_cache_quiesce()==-16 && freed==before);
            assert(storage_ext4_cache_release()==-16 && freed==before);
            entry->dirty=0;entry->pinned=1;
            assert(storage_ext4_cache_release()==-16 && freed==before);
            entry->pinned=0;
        }
        /* Actual final-volume invalidation clears all borrowed volume pointers.
         * No claim about journal persistence is made by this allocator test. */
        storage_ext4_cache_invalidate(g_active_volume);
        assert(!storage_ext4_cache_quiesce());
        for(unsigned t=0;t<EXT4_CACHE_TABLE_COUNT;++t)
            assert(!ext4_cache_tables[t].entries[0].volume);
        assert(!storage_ext4_cache_release());
        for(unsigned t=0;t<EXT4_CACHE_TABLE_COUNT;++t)assert(!ext4_cache_tables[t].entries);
        assert(allocated==freed);
    }
    allocation_failure=true;
    ext2_cache_store(7,8,input);assert(!ext2_cache);
    assert(!ext4_cache_ensure(&ext4_cache_tables[0]));
    allocation_failure=false;mapping_failure=true;
    ext2_cache_store(7,8,input);assert(!ext2_cache);
    assert(!ext4_cache_ensure(&ext4_cache_tables[0]));
    mapping_failure=false;
    assert(!ext2_cache_release() && !storage_ext4_cache_release() && allocated==freed);
    puts("PASS native ext caches: 32 retirement cycles, all dirty/pinned domains, OOM/map rollback, exact page release");
}
