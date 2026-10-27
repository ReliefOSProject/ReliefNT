/* SPDX-License-Identifier: Apache-2.0 */
#include "../core/storage_internal.h"

/** @brief Read a POSIX short extent from a pinned optical device into current bytes.
 * @param ref Borrowed exact block/code reference held for the whole call.
 * @param offset Byte position. @param buffer Writable length-byte kernel extent.
 * @param length Requested bytes. @param out_read Required byte-count output.
 * @return Zero, bounds/geometry errno, or native error including EAGAIN unchanged.
 * Execution ownership required; global I/O admission and transport exclusion
 * protect scratch staging. One native block per short read avoids replaying
 * earlier chunks when a later asynchronous optical command yields.
 */
int storage_optical_read(const struct block_device_ref *ref,uint64_t offset,void *buffer,
                            uint32_t length,uint32_t *out_read)
{
    if(!out_read || (!buffer && length))return -22;*out_read=0;if(!length)return 0;
    struct block_device_info info;int rc=block_device_info(ref,&info);if(rc)return rc;
    if(!(info.flags&BLOCK_DEVICE_OPTICAL) || info.block_size!=2048)return -95;
    if(info.blocks>UINT64_MAX/2048)return -75;
    uint64_t bytes=info.blocks*2048;if(offset>=bytes)return 0;
    rc=storage_acquire_task_io();if(rc)return rc;
    uint32_t within=offset%2048,count=2048-within;
    if(count>length)count=length;
    kernel_spin_lock(&storage_transport_lock);
    rc=block_device_read(ref,offset/2048,1,storage_scratch);
    if(!rc){storage_memcpy(buffer,storage_scratch+within,count);*out_read=count;}
    kernel_spin_unlock(&storage_transport_lock);return rc;
}
