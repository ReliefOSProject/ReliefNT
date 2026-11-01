/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/paging.h>

/**
 * @brief Resolve a table page owned by the kernel's already installed map.
 * @param physical Aligned physical address read from a trusted kernel table.
 * @return Borrowed identity alias for image tables or trusted direct-map alias
 * for allocator/bootstrap-owned tables. No coverage query or ownership change.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical)
{
    if (physical >= RELIEFNT_KERNEL_HOLE_START &&
        physical <= RELIEFNT_KERNEL_HOLE_END - 4096)
        return (uint64_t *)(uintptr_t)physical;
    return (uint64_t *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + physical);
}
