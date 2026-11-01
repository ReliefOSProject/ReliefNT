/* Internal early-memory helpers shared without allocating or calling bootstrap. */
#ifndef RELIEFNT_TOPOLOGY_INTERNAL_H
#define RELIEFNT_TOPOLOGY_INTERNAL_H

#include <reliefnt/memory_topology.h>
#include <reliefnt/multiboot2.h>

#define MM_TOPOLOGY_PAGE_SIZE 4096ULL
#define MM_TOPOLOGY_MAX_LIMIT (1ULL << 46)

enum mm_input_kind {
    MM_INPUT_USABLE,
    MM_INPUT_RECLAIMABLE,
    MM_INPUT_FIRMWARE_RESERVED,
    MM_INPUT_BOOT_RESERVED,
};

typedef int (*mm_input_visitor)(uint64_t start, uint64_t end,
                                enum mm_input_kind kind, void *context);

/**
 * @brief Visit raw usable bytes and aligned reserved physical intervals.
 * @param boot Borrowed readable handoff and every described input buffer.
 * @param limit Page-aligned exclusive physical limit, at most 64 TiB.
 * @param visit Optional callback; may return an error to stop visiting.
 * @param context Opaque borrowed callback context.
 * @param clipped Optional count of firmware descriptors extending past limit.
 * @return 0 or negative errno; no allocation, locks or ownership transfer.
 */
int mm_topology_visit(const struct boot_info *boot, uint64_t limit,
                      mm_input_visitor visit, void *context, uint64_t *clipped);
/**
 * @brief Read one raw usable interval from the selected validated firmware map.
 * @param boot Borrowed handoff validated by mm_topology_visit.
 * @param index Descriptor index within the selected EFI or Multiboot map.
 * @param limit Page-aligned exclusive physical limit.
 * @param start Required output for the inclusive physical byte start.
 * @param end Required output for the exclusive physical byte end.
 * @return 1 for a usable candidate, 0 for another or empty type, negative errno
 * for invalid descriptors; does not allocate or transfer ownership.
 */
int mm_topology_candidate(const struct boot_info *boot, uint32_t index,
                          uint64_t limit, uint64_t *start, uint64_t *end);
/**
 * @brief Overlay an aligned reservation on a topology without losing RAM identity.
 * @param topology Writable workspace-backed view with spare range capacity.
 * @param start Inclusive physical start, rounded down.
 * @param end Exclusive physical end, rounded up and clipped to the limit.
 * @param extra Additional MM_TOPOLOGY_BOOTSTRAP flag, or zero.
 * @return 0 or negative errno; on failure the topology is unchanged.
 */
int mm_topology_reserve(struct mm_topology *topology, uint64_t start,
                        uint64_t end, uint32_t extra);
/**
 * @brief Convert a current identity or shared direct-map pointer to physical.
 * @param pointer Borrowed current kernel virtual pointer represented as an integer.
 * @return Physical address for a direct-map alias, otherwise the identity value.
 */
uint64_t mm_topology_pointer_phys(uint64_t pointer);
#endif
