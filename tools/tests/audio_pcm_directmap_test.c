#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <reliefnt/mm.h>
#include "audio_fixture.h"
#include "audio_fake_card.h"

#include "../../sound/core/core.c"
#define AUDIO_FIXTURE_CUSTOM_MM_TRANSLATION
#include "modules/pcm.h"
#undef AUDIO_FIXTURE_CUSTOM_MM_TRANSLATION

enum {
    TEST_RING_BYTES = 128 * 1024,
    TEST_DMA_PHYS = 0x24000000u,
    TEST_DRIVER_DMA_BUS = 0x34000000u,
};

static uint8_t *test_ring_storage;
static size_t test_ring_storage_bytes;
static uint64_t test_kernel_free_count;

/** @brief Model a heap allocation returned through the supervisor direct map. */
void *kernel_malloc(size_t size)
{
    void *memory = NULL;
    if (posix_memalign(&memory, 4096, size)) return NULL;
    test_ring_storage = memory;
    test_ring_storage_bytes = size;
    return memory;
}

/** @brief Release the retained direct-map heap allocation after the last VMA. */
void kernel_free(void *memory)
{
    assert(memory == test_ring_storage);
    ++test_kernel_free_count;
    free(memory);
    test_ring_storage = NULL;
    test_ring_storage_bytes = 0;
}

/** @brief Translate test direct-map and low linked pointers to physical bytes. */
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    uintptr_t start = (uintptr_t)test_ring_storage;
    if (test_ring_storage && value >= start &&
        value - start < test_ring_storage_bytes) {
        return TEST_DMA_PHYS + (value - start);
    }
    /* The no-PIE fixture places static kernel objects in the modeled identity
     * window, corresponding to the kernel's low supervisor image mapping. */
    if (value >= 0x10000u && value < 0x40000000u) return (uint64_t)value;
    return 0;
}

/** @brief Resolve physical test-ring pages to the same coherent CPU backing. */
void *mm_phys_to_virt(uint64_t physical)
{
    if (test_ring_storage && physical >= TEST_DMA_PHYS &&
        physical - TEST_DMA_PHYS < test_ring_storage_bytes) {
        return test_ring_storage + (physical - TEST_DMA_PHYS);
    }
    if (physical >= 0x10000u && physical < 0x40000000u)
        return (void *)(uintptr_t)physical;
    return NULL;
}

/* The PCM fixture links the core directly and does not include timer.c. */
void audio_timer_period_elapsed(uint32_t card, uint32_t device,
                                enum audio_direction direction,
                                uint64_t frames, int error)
{
    (void)card; (void)device; (void)direction; (void)frames; (void)error;
}

/** @brief Check DMA and userspace aliases for an allocator-backed PCM ring. */
static void test_large_ring_physical_alias_and_lifetime(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    int error = 0;
    struct audio_pcm *pcm = audio_pcm_open(card.id, 0, AUDIO_PLAYBACK, &error);
    assert(pcm && !error);
    pcm->caps.buffer_bytes_max = AUDIO_PCM_BUFFER_BYTES;
    struct audio_params params = {48000, 2, 16, 4, 8192, 32768};
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(pcm->dma.bytes == TEST_RING_BYTES);
    assert(pcm->dma.kernel == (void *)pcm->extended_buffer);
    assert(pcm->dma.bus == TEST_DMA_PHYS);
    assert(pcm->dma.bus != (uint64_t)(uintptr_t)pcm->dma.kernel);
    /* Model a card-provided IOMMU/bus mapping for this unchanged CPU ring. */
    pcm->dma.bus = TEST_DRIVER_DMA_BUS;
    assert(!audio_pcm_hw_params(pcm, &params));
    assert(pcm->dma.bus == TEST_DRIVER_DMA_BUS);
    assert(!audio_pcm_prepare(pcm));
    assert(card.prepared_dma.bus == TEST_DRIVER_DMA_BUS);
    assert(card.prepared_dma.kernel == pcm->dma.kernel);

    struct audio_pcm_mmap data;
    assert(!audio_pcm_mmap_acquire(pcm, AUDIO_PCM_MMAP_OFFSET_DATA,
                                   TEST_RING_BYTES,
                                   AUDIO_PCM_PROT_READ | AUDIO_PCM_PROT_WRITE,
                                   &data));
    assert(data.backing == TEST_DMA_PHYS);
    assert(data.backing != (uint64_t)(uintptr_t)pcm->dma.kernel);
    uint8_t *mapped = mm_phys_to_virt(data.backing);
    assert(mapped == pcm->dma.kernel);
    ((uint8_t *)pcm->dma.kernel)[17] = 0x5a;
    assert(mapped[17] == 0x5a);
    mapped[TEST_RING_BYTES - 1] = 0xa5;
    assert(((uint8_t *)pcm->dma.kernel)[TEST_RING_BYTES - 1] == 0xa5);

    struct audio_pcm_mmap status, control;
    assert(!audio_pcm_mmap_acquire(pcm, AUDIO_PCM_MMAP_OFFSET_STATUS_NEW,
                                   4096, AUDIO_PCM_PROT_READ, &status));
    assert(!audio_pcm_mmap_acquire(pcm, AUDIO_PCM_MMAP_OFFSET_CONTROL_NEW,
                                   4096, AUDIO_PCM_PROT_READ | AUDIO_PCM_PROT_WRITE,
                                   &control));
    assert(status.backing == mm_virt_to_phys(pcm->status_page));
    assert(control.backing == mm_virt_to_phys(pcm->control_page));
    assert(status.backing && control.backing);

    audio_pcm_release(pcm);
    assert(test_kernel_free_count == 0);
    assert(mm_phys_to_virt(data.backing) == mapped);
    audio_pcm_mmap_release(pcm, AUDIO_PCM_MMAP_STATUS);
    audio_pcm_mmap_release(pcm, AUDIO_PCM_MMAP_CONTROL);
    audio_pcm_mmap_release(pcm, AUDIO_PCM_MMAP_DATA);
    assert(test_kernel_free_count == 1);
    assert(!audio_unregister_card(card.id, 0));
}

/** @brief Exercise DMA physical address, mmap alias coherence, and VMA pinning. */
int main(void)
{
    test_large_ring_physical_alias_and_lifetime();
    puts("PASS PCM direct-map DMA and mmap backing coherence/lifetime");
    return 0;
}
