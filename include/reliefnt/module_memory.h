/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_MEMORY_H
#define RELIEFNT_MODULE_MEMORY_H
#include <reliefnt/types.h>

#define RELIEFNT_MODULE_BASE 0xffffff9000000000ULL
#define RELIEFNT_MODULE_END  0xffffffa000000000ULL
#define MODULE_MEMORY_WRITE 1u
#define MODULE_MEMORY_EXEC 2u

struct module_memory {
    struct module_memory *next;
    uintptr_t address;
    size_t size;
    uint64_t *pages;
    uint64_t *alias_flags;
    size_t page_count;
};

/**
 * @brief Reserve shared supervisor virtual space and allocate zeroed discontiguous pages.
 * @param size Nonzero byte count; rounded up to 4 KiB.
 * @param alignment Power-of-two virtual alignment; physical continuity is not implied.
 * @param out Required output receiving owned RW/NX storage; zeroed on failure.
 * @return Zero or a negative errno. Task context; no module code or I/O under the list lock.
 */
int module_memory_alloc(size_t size, size_t alignment, struct module_memory **out);
/**
 * @brief Change a private module region's permissions and synchronize every CPU's TLB.
 * @param memory Caller-owned allocated region with no concurrent mutation/execution.
 * @param permissions Zero for RO/NX, WRITE for RW/NX, EXEC for RX; W+X is rejected.
 * @return Zero or a negative errno; direct-map/identity aliases stay NX and cannot write RX pages.
 */
int module_memory_protect(struct module_memory *memory, unsigned int permissions);
/**
 * @brief Unmap a quiescent region, synchronize CPUs and release its backing pages.
 * @param memory Owned region or NULL; caller must have drained every user/execution pin.
 */
void module_memory_free(struct module_memory *memory);
#endif
