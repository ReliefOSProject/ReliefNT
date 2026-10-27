/* Test-only production module composition; the host heap is an explicit
 * boundary now that every production PCM mapping owns allocated backing. */
#include <stdlib.h>
#include <reliefnt/heap.h>
#ifndef AUDIO_FIXTURE_CUSTOM_HEAP
#include <stdlib.h>
void *kernel_malloc(size_t bytes){return malloc(bytes);}
void kernel_free(void *memory){free(memory);}
#endif
/* The physical-translation boundary must recognize a newly allocated owner
 * before HW_PARAMS publishes it into pcm_slots. Record only real allocations. */
struct audio_fixture_pcm_allocation {struct audio_fixture_pcm_allocation *next;void *memory;size_t bytes;};
static struct audio_fixture_pcm_allocation *audio_fixture_pcm_allocations;
static void *audio_fixture_pcm_malloc(size_t bytes)
{
    void *memory=kernel_malloc(bytes);if(!memory)return NULL;
    struct audio_fixture_pcm_allocation *entry=malloc(sizeof(*entry));assert(entry);
    *entry=(struct audio_fixture_pcm_allocation){audio_fixture_pcm_allocations,memory,bytes};
    audio_fixture_pcm_allocations=entry;return memory;
}
static void audio_fixture_pcm_free(void *memory)
{
    if(memory) {
        struct audio_fixture_pcm_allocation **entry=&audio_fixture_pcm_allocations;
        while(*entry && (*entry)->memory!=memory)entry=&(*entry)->next;
        assert(*entry);struct audio_fixture_pcm_allocation *old=*entry;*entry=old->next;free(old);
    }
    kernel_free(memory);
}
#define kernel_malloc audio_fixture_pcm_malloc
#define kernel_free audio_fixture_pcm_free
#include "../../../sound/core/pcm_buffer.c"
#include "../../../sound/core/pcm_mapping.c"
#include "../../../sound/core/pcm_transfer.c"
#include "../../../sound/core/pcm_format.c"
#include "../../../sound/core/pcm.c"
#undef kernel_malloc
#undef kernel_free

#ifndef AUDIO_FIXTURE_CUSTOM_MM_TRANSLATION
#include <reliefnt/mm.h>
/** @brief Translate only explicit PCM allocations in the host fixture.
 * @param address Candidate CPU alias.
 * @return Modeled physical address for a byte in a currently owned ring or
 * metadata allocation, zero otherwise. No linked module/static data accepted.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value=(uintptr_t)address;
    for(const struct audio_fixture_pcm_allocation *entry=audio_fixture_pcm_allocations;entry;entry=entry->next) {
        uintptr_t base=(uintptr_t)entry->memory;
        if(value>=base && value-base<entry->bytes)return (uint64_t)value;
    }
    return 0;
}
#endif
