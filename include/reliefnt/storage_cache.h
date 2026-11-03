/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_CACHE_H
#define RELIEFNT_STORAGE_CACHE_H
#include <reliefnt/types.h>
struct block_device;
/* Notify native metadata caches before a physical write. Caller owns storage
 * exclusion; no registry/transport lock may span the callbacks. Units are
 * 512-byte sectors regardless of the actual transport block size. No I/O,
 * allocation or reference transfer; provider owners remain retained. */
void storage_filesystem_cache_event_device(const struct block_device *device,
                                           uint64_t lba,uint32_t sectors);
#endif
