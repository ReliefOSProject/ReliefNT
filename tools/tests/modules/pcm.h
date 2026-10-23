/* Test-only production module composition. */
#include "../../../sound/core/pcm_buffer.c"
#include "../../../sound/core/pcm_mapping.c"
#include "../../../sound/core/pcm_transfer.c"
#include "../../../sound/core/pcm_format.c"
#include "../../../sound/core/pcm.c"

#ifndef AUDIO_FIXTURE_CUSTOM_MM_TRANSLATION
#include <reliefnt/mm.h>
/** @brief Translate only the PCM fixture's modeled identity-linked static pool.
 * @param address Candidate CPU alias to the PCM static backing.
 * @return Identity physical address for a byte in pcm_slots, or zero outside
 * the kernel-owned fixture span; ordinary host/user pointers are rejected.
 */
uint64_t mm_virt_to_phys(const void *address)
{
    uintptr_t value = (uintptr_t)address;
    uintptr_t start = (uintptr_t)&pcm_slots[0];
    size_t bytes = sizeof(pcm_slots);
    return value >= start && value - start < bytes ? (uint64_t)value : 0;
}
#endif
