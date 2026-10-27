/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"

/** @brief Commit/checkpoint native journal data before flushing its dirty cache.
 * @param volume Borrowed pinned ext-family instance with parsed geometry.
 * @return Zero or first journal/cache error. Caller owns execution and a
 * synchronous I/O scope. This precedes the physical device-cache flush and
 * never runs under the transport or module/graph spinlock.
 */
int storage_ext4_sync(struct storage_volume *volume)
{
    int rc=storage_ext4_journal_commit(volume,true);
    if(!rc)rc=storage_ext4_journal_checkpoint(volume);
    if(!rc)rc=storage_ext4_cache_flush(volume);
    return rc;
}
