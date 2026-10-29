/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PARTITION_MBR_H
#define RELIEFNT_PARTITION_MBR_H
#include <reliefnt/types.h>
#define MBR_PARTITION_CAPACITY 128u
struct mbr_partition { uint64_t first, count; uint8_t type; };
/** @brief Read MBR primary and DOS EBR logical extents without trusting CHS fields.
 * @param read Reads one512-byte sector; negative errno preserved.
 * @param context Borrowed reader context. @param sectors Actual512-byte sectors.
 * @param entries Required128-slot output: primary0..3, logical4 onward.
 * @return Zero, ENOENT for absent signature, EOPNOTSUPP for protective GPT,
 * EINVAL for overlaps/bounds/cycles, ENOSPC on slot exhaustion, or I/O errno.
 * Task context; bounded reads and stack storage, no writes or allocation.
 * Failure clears the complete output so no partial corrupt table is published.
 */
int mbr_read_partitions(int (*read)(void *,uint64_t,uint8_t[512]),void *context,
                         uint64_t sectors,struct mbr_partition entries[MBR_PARTITION_CAPACITY]);
#endif
