/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/blkdev.h>
#include <reliefnt/heap.h>
#include <reliefnt/mm.h>
#include <reliefnt/swap_internal.h>
#include <reliefnt/swap.h>
#include <linux/errno.h>

#define SWAP_EXTENTS_PER_CHUNK 64u
struct swap_extent { uint64_t logical, physical, length; };
struct swap_extent_chunk {
    struct swap_extent_chunk *next;
    uint32_t used;
    struct swap_extent item[SWAP_EXTENTS_PER_CHUNK];
};
struct swap_backend {
    struct block_device_ref device;
    struct block_device_info info;
    struct swap_extent_chunk *head,*tail;
    uint64_t first,count,covered;
};

/** @brief Create an empty bounded fixed-block backend with exact device ownership.
 * @param out Required empty output. @param device Owned device, moved on success.
 * @param first First permitted physical block. @param count Nonzero block count.
 * @return Zero or negative errno. This is an activation-only allocation.
 */
int swap_backend_create(struct swap_backend **out,struct block_device_ref *device,
                        uint64_t first,uint64_t count)
{
    if (!out || !device || !device->device || !count) return -EINVAL;
    *out=NULL;struct block_device_info info;int rc=block_device_info(device,&info);if (rc) return rc;
    if (!(info.flags&BLOCK_DEVICE_SWAP_SAFE) || (info.flags&BLOCK_DEVICE_READ_ONLY)) return -EOPNOTSUPP;
    if (!info.max_blocks || !info.block_size || info.block_size>4096 ||
        4096%info.block_size || first>=info.blocks || count>info.blocks-first) return -EINVAL;
    struct swap_backend *backend=kernel_malloc(sizeof(*backend));if (!backend) return -ENOMEM;
    *backend=(struct swap_backend){.device=*device,.info=info,.first=first,.count=count};
    device->device=NULL;*out=backend;return 0;
}

/** @brief Append a unique contiguous logical block run to an unpublished backend.
 * @param backend Owned backend. @param logical Next logical block.
 * @param physical First device block. @param length Nonzero blocks.
 * @return Zero or negative errno. Allocation is allowed only before publication.
 */
int swap_backend_add_extent_checkpoint(struct swap_backend *backend,uint64_t logical,
                            uint64_t physical,uint64_t length,
                            int (*checkpoint)(void *),void *context)
{
    if (!backend || !length || logical!=backend->covered || length>UINT64_MAX-logical ||
        physical<backend->first || physical-backend->first>=backend->count ||
        length>backend->count-(physical-backend->first)) return -EINVAL;
    for (struct swap_extent_chunk *chunk=backend->head; chunk; chunk=chunk->next) {
        for (uint32_t i=0; i<chunk->used; ++i) {
            struct swap_extent *e=&chunk->item[i];
            if (physical<=e->physical ? e->physical-physical<length : physical-e->physical<e->length)
                return -EINVAL;
        }
        if (checkpoint) { int rc=checkpoint(context);if (rc)return rc; }
    }
    if (backend->tail && backend->tail->used) {
        struct swap_extent *last=&backend->tail->item[backend->tail->used-1];
        if (last->physical+last->length==physical) {
            last->length+=length;backend->covered+=length;return 0;
        }
    }
    if (!backend->tail || backend->tail->used==SWAP_EXTENTS_PER_CHUNK) {
        struct swap_extent_chunk *next=kernel_malloc(sizeof(*next));if (!next) return -ENOMEM;
        *next=(struct swap_extent_chunk){0};
        if (backend->tail) backend->tail->next=next; else backend->head=next;
        backend->tail=next;
    }
    backend->tail->item[backend->tail->used++]=(struct swap_extent){logical,physical,length};
    backend->covered+=length;return 0;
}

/** @brief Append a fixed extent without a checkpoint for bounded callers.
 * @param backend Unpublished backend. @param logical First logical block.
 * @param physical First device block. @param length Nonzero block count.
 * @return Zero or negative errno.
 */
int swap_backend_add_extent(struct swap_backend *backend,uint64_t logical,
                            uint64_t physical,uint64_t length)
{ return swap_backend_add_extent_checkpoint(backend,logical,physical,length,NULL,NULL); }

/** @brief Consume all fixed extent metadata and the exact device/code token.
 * @param backend Owned unpublished or drained backend, nullable.
 */
void swap_backend_destroy_checkpoint(struct swap_backend *backend,int (*checkpoint)(void *),void *context)
{
    if (!backend) return;
    struct swap_extent_chunk *chunk=backend->head;
    unsigned freed=0;
    while (chunk) {struct swap_extent_chunk *next=chunk->next;kernel_free(chunk);chunk=next;
        if (checkpoint && (++freed&31u)==0) (void)checkpoint(context);}
    block_device_put(&backend->device);kernel_free(backend);
}

/** @brief Release a drained backend without a checkpoint for bounded callers.
 * @param backend Owned backend, nullable.
 */
void swap_backend_destroy(struct swap_backend *backend)
{ swap_backend_destroy_checkpoint(backend,NULL,NULL); }

/** @brief Transfer one page using only prepared extents and provider-owned DMA.
 * @param area Live area retaining its backend. @param page Logical page index.
 * @param physical Caller-retained aligned physical page. @param write Direction.
 * @return Zero on complete transfer or terminal negative errno. A failed read
 * may alter the destination but never invalidates the prior disk copy; callers
 * retain original resident data through any failed write. This task-context
 * operation acquires reentrant execution ownership; callbacks cannot queue work.
 */
int swap_backend_page_io(struct swap_area *area,uint32_t page,uint64_t physical,bool write)
{
    if (!area || !area->backend || !physical || (physical&4095u)) return -EINVAL;
    struct swap_backend *backend=area->backend;
    uint64_t per_page=4096u/backend->info.block_size,start=(uint64_t)page*per_page;
    if (start>backend->covered || per_page>backend->covered-start) return -EINVAL;
    uint8_t *buffer=mm_phys_to_virt(physical);if (!buffer) return -EFAULT;
    uint64_t execution_flags;kernel_execution_lock_irqsave(&execution_flags);
    struct swap_storage_token token={0};int rc=swap_storage_begin(&token);
    if (rc) {
        bool other = rc == -EBUSY && swap_storage_other_owner();
        kernel_execution_unlock_irqrestore(execution_flags);
        return other ? SWAP_STORAGE_CONTENDED : rc;
    }
    /* Native activation uses the same exclusive token but legitimately calls
     * filesystem services; this mark excludes ordinary reentrant slot I/O. */
    token.slot=true;
    uint64_t cursor=start,remaining=per_page;
    for (struct swap_extent_chunk *chunk=backend->head; chunk && remaining && !rc; chunk=chunk->next)
        for (uint32_t i=0; i<chunk->used && remaining && !rc; ++i) {
            struct swap_extent *extent=&chunk->item[i];
            if (cursor>=extent->logical+extent->length) continue;
            if (cursor<extent->logical) {rc=-EIO;break;}
            uint64_t offset=cursor-extent->logical,available=extent->length-offset;
            while (available && remaining && !rc) {
                uint64_t n=available<remaining ? available : remaining;
                if (n>backend->info.max_blocks) n=backend->info.max_blocks;
                rc=swap_storage_transfer(&token,&backend->device,extent->physical+offset,(uint32_t)n,
                                         buffer+(cursor-start)*backend->info.block_size,write);
                if (!rc) {cursor+=n;offset+=n;remaining-=n;available-=n;}
            }
        }
    if (!rc && remaining) rc=-EIO;
    swap_storage_end(&token);kernel_execution_unlock_irqrestore(execution_flags);return rc;
}
