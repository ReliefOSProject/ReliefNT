/* SPDX-License-Identifier: Apache-2.0 */
/* Descriptor-only tests mock block admission, not hardware/module lifecycles.
 * Real block/module ownership has separate block_registry/volume/native tests. */
#ifndef RELIEFNT_BLOCK_DESCRIPTION_FIXTURE_H
#define RELIEFNT_BLOCK_DESCRIPTION_FIXTURE_H
#include <reliefnt/blkdev.h>
#include <reliefnt/storage.h>
#include <assert.h>
static unsigned fixture_block_refs;
static unsigned char fixture_block_identity;
int storage_disk_block_get(uint32_t disk,struct block_device_ref *ref)
{
    assert(disk==0 && ref && !ref->device);
    ref->device=(struct block_device *)&fixture_block_identity;++fixture_block_refs;return 0;
}
void block_device_put(struct block_device_ref *ref)
{
    if(!ref || !ref->device)return;
    assert(ref->device==(struct block_device *)&fixture_block_identity && fixture_block_refs);
    --fixture_block_refs;ref->device=NULL;
}
uint64_t storage_block_rdev(uint32_t value)
{
    uint32_t disk=STORAGE_BLOCK_DISK_ID(value),part=STORAGE_BLOCK_PARTITION(value)+1;
    return disk&STORAGE_BLOCK_OPTICAL_BIT ? block_device_make_number(11,disk&~STORAGE_BLOCK_OPTICAL_BIT) :
        block_device_make_number(259,disk*256u+part);
}
#endif
