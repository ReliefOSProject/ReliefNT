/* Trusted early physical memory topology and bootstrap page ownership. */
#ifndef RELIEFNT_MEMORY_TOPOLOGY_H
#define RELIEFNT_MEMORY_TOPOLOGY_H

#include <reliefnt/types.h>

/* Flags describe complete page-aligned intervals, not arbitrary byte RAM. */
#define MM_TOPOLOGY_RAM         0x01u /* Trusted RAM, including boot-owned pages. */
#define MM_TOPOLOGY_USABLE      0x02u /* RAM available for allocation now. */
#define MM_TOPOLOGY_RESERVED    0x04u /* Never allocate until explicit handoff. */
#define MM_TOPOLOGY_RECLAIMABLE 0x08u /* Firmware RAM whose release is unproven. */
#define MM_TOPOLOGY_BOOTSTRAP   0x10u /* Workspace or pages owned by bootstrap. */

/** @brief One sorted, nonoverlapping, page-aligned physical half-open interval. */
struct mm_topology_range {
    uint64_t start; /**< Inclusive physical byte address. */
    uint64_t end;   /**< Exclusive physical byte address. */
    uint32_t flags; /**< MM_TOPOLOGY_* flags, with reserved taking priority. */
};

/** @brief Borrowed view whose range storage belongs to the supplied workspace. */
struct mm_topology {
    struct mm_topology_range *ranges; /**< Ordered storage; consumers must not mutate it. */
    size_t range_count;              /**< Number of valid range entries. */
    size_t range_capacity;           /**< Reserved storage used by bootstrap mutations. */
    uint64_t physical_limit;          /**< Exclusive min(2^physical_bits, 64 TiB). */
    uint64_t total_ram_bytes;        /**< Trusted page RAM, including reserved RAM. */
    uint64_t clipped_descriptors;    /**< Input map records extending beyond the limit. */
};

struct boot_info;

/**
 * @brief Compute the storage needed to normalize the borrowed boot map.
 * @param boot Borrowed decoded handoff and complete readable map and boot blocks.
 * @param bytes Required output; zeroed on failure.
 * @return 0, -EINVAL or -EOVERFLOW; no allocation or ownership transfer.
 */
int mm_topology_workspace_size(const struct boot_info *boot, size_t *bytes);
/**
 * @brief Normalize trusted physical ranges into caller-owned workspace.
 * @param boot Borrowed boot information and readable map and boot blocks.
 * @param physical_bits CPU physical address width, inclusive 32 through 52.
 * @param workspace Writable 8-byte-aligned storage disjoint from all inputs.
 * @param workspace_bytes Available bytes as returned by workspace_size or more.
 * @param out Writable result, cleared on failure, disjoint from workspace and inputs.
 * @return 0 or negative errno; no allocation, locks, or ownership transfer.
 * The output view remains valid while workspace remains alive and unchanged.
 * Borrowed maps and boot blocks need remain readable only through this call;
 * no pointer into them is retained. Firmware conflicts override usable RAM.
 */
int mm_topology_build(const struct boot_info *boot, uint8_t physical_bits,
                      void *workspace, size_t workspace_bytes,
                      struct mm_topology *out);
/**
 * @brief Revoke a topology view without freeing its caller-owned workspace.
 * @param topology Writable result, or NULL; contained storage is not released.
 */
void mm_topology_destroy(struct mm_topology *topology);
/**
 * @brief Reserve reachable bootstrap storage and publish its topology.
 * @param boot Borrowed parsed handoff; full map/boot buffers must be reachable
 * under the already installed shared direct map, including their final bytes.
 * @param physical_bits CPU physical address width, 32 through 52.
 * @return 0 or negative errno. BSP only after shared direct-map and framebuffer
 * initialization and before PMM, with interrupts disabled. No heap/PMM call.
 */
int mm_bootstrap_prepare(const struct boot_info *boot, uint8_t physical_bits);
/**
 * @brief Reserve a reachable contiguous run of bootstrap pages.
 * @param pages Nonzero number of 4 KiB pages.
 * @param inclusive_max Inclusive maximum physical byte in the returned run.
 * @return Physical start, or 0 on exhaustion/invalid input; ownership persists
 * through finish and must be transferred explicitly to its later consumer.
 */
uint64_t mm_bootstrap_alloc(uint32_t pages, uint64_t inclusive_max);
/**
 * @brief Borrow the published bootstrap topology, including owned pages.
 * @return Stable read-only view after prepare, including after finish, or NULL
 * if absent. Allocations can split/merge ranges, invalidating range pointers.
 */
const struct mm_topology *mm_bootstrap_topology(void);
/**
 * @brief Borrow the original handoff for preserving explicit boot resources.
 * @return Caller-owned boot information while bootstrap remains open, or NULL.
 * The caller must retain the handoff and its boot buffers through construction.
 */
const struct boot_info *mm_bootstrap_boot_info(void);
/**
 * @brief Close bootstrap allocation without releasing any reserved pages.
 */
void mm_bootstrap_finish(void);
#endif
