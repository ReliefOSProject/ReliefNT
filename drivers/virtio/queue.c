/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/virtio.h>
#include <linux/errno.h>

/** @brief Bind an empty queue to stopped coherent pages with native split layout.
 * @param q Exclusive object. @param memory 16-byte-aligned storage.
 * @param physical DMA base. @param bytes Capacity. @param size Descriptor count.
 * @return Zero or -EINVAL. Caller owns storage and excludes hardware access.
 */
int virtio_queue_bind(struct virtio_queue *q, void *memory, uint64_t physical,
                       size_t bytes, uint16_t size)
{
    if (!q || !memory || !physical || ((uintptr_t)memory & 15) || (physical & 15) ||
        !size || size > VIRTIO_QUEUE_MAX || (size & (size - 1))) return -EINVAL;
    size_t avail = (size_t)size * 16;
    size_t used = (avail + 6 + (size_t)size * 2 + 3) & ~(size_t)3;
    size_t end = used + 6 + (size_t)size * 8;
    if (bytes < end || physical > UINT64_MAX - end) return -EINVAL;
    __builtin_memset(memory, 0, end);
    q->descriptors = memory;
    q->available = (void *)((uint8_t *)memory + avail);
    q->used_header = (void *)((uint8_t *)memory + used);
    q->used_ring = (void *)(q->used_header + 2);
    q->descriptor_physical = physical;
    q->available_physical = physical + avail;
    q->used_physical = physical + used;
    q->size = q->free_count = size;
    for (uint16_t i = 0; i < size; ++i) q->descriptors[i].next = i + 1;
    return 0;
}

/** @brief Publish a complete direct chain before its avail index and doorbell.
 * @param q Retained live ring. @param segments Caller-owned DMA extents.
 * @param count Extent count. @param cookie Stable opaque completion identity.
 * @return Zero or -EINVAL/-ENOSPC/-EIO; IRQ-safe, no retained segments pointer.
 */
int virtio_queue_submit(struct virtio_queue *q, const struct virtio_segment *segments,
                         unsigned count, void *cookie)
{
    if (!q || !segments || !cookie || !count || count > q->size) return -EINVAL;
    uint32_t writable = 0; bool writing = false;
    for (unsigned i = 0; i < count; ++i) {
        if (!segments[i].physical || !segments[i].length ||
            segments[i].physical > UINT64_MAX - (segments[i].length - 1u) ||
            (writing && !segments[i].writable)) return -EINVAL;
        if (segments[i].writable) {
            if (segments[i].length > UINT32_MAX - writable) return -EINVAL;
            writable += segments[i].length; writing = true;
        }
    }
    uint64_t flags; kernel_spin_lock_irqsave(&q->lock, &flags);
    int rc = 0;
    if (q->broken) rc = -EIO;
    else if (count > q->free_count) rc = -ENOSPC;
    else {
        uint16_t head = q->free_head, id = head;
        for (unsigned i = 0; i < count; ++i) {
            uint16_t next = q->descriptors[id].next;
            q->descriptors[id].address = segments[i].physical;
            q->descriptors[id].length = segments[i].length;
            q->descriptors[id].flags = (i + 1 < count ? 1 : 0) |
                                       (segments[i].writable ? 2 : 0);
            if (i + 1 < count) q->descriptors[id].next = next;
            id = next;
        }
        q->free_head = id; q->free_count -= count; ++q->pending;
        q->cookies[head] = cookie; q->chain_lengths[head] = count;
        q->writable_bytes[head] = writable;
        q->available[2 + (q->available_index & (q->size - 1))] = head;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        q->available[1] = ++q->available_index;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (q->notify && !(q->used_header[0] & 1)) *q->notify = q->index;
    }
    kernel_spin_unlock_irqrestore(&q->lock, flags);
    return rc;
}

/** @brief Validate device-produced used entries before reclaiming driver memory.
 * @param q Retained ring. @param cookie Completion identity output.
 * @param length Completed writable-byte count output.
 * @return One/zero or -EIO; malformed entries poison the ring without freeing DMA.
 */
int virtio_queue_pop(struct virtio_queue *q, void **cookie, uint32_t *length)
{
    if (!q || !cookie || !length) return -EINVAL;
    uint64_t flags; kernel_spin_lock_irqsave(&q->lock, &flags);
    int rc = 0;
    uint16_t produced = q->used_header[1];
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    uint16_t ready = (uint16_t)(produced - q->used_index);
    if (q->broken || ready > q->pending) goto broken;
    if (!ready) goto out;
    struct virtio_used_element used = q->used_ring[q->used_index & (q->size - 1)];
    if (used.id >= q->size || !q->chain_lengths[used.id] ||
        used.length > q->writable_bytes[used.id]) goto broken;
    uint16_t id = used.id, count = q->chain_lengths[id];
    *cookie = q->cookies[id]; *length = used.length;
    q->cookies[id] = NULL; q->chain_lengths[id] = 0;
    for (unsigned i = 1; i < count; ++i) id = q->descriptors[id].next;
    q->descriptors[id].next = q->free_head; q->free_head = used.id;
    q->free_count += count; --q->pending; ++q->used_index;
    rc = 1; goto out;
broken:
    q->broken = true; rc = -EIO;
out:
    kernel_spin_unlock_irqrestore(&q->lock, flags);
    return rc;
}

/** @brief Exclude a final two-descriptor chain from the normal free list.
 * @param q Bound empty stopped ring. @return Zero or -EINVAL without mutation.
 */
int virtio_queue_reserve_emergency(struct virtio_queue *q)
{
    if(!q || q->size<4 || q->pending || q->free_count!=q->size ||
       q->free_head || q->available_index || q->emergency_reserved)return -EINVAL;
    q->free_count-=2;q->descriptors[q->size-3].next=q->size;
    q->emergency_reserved=true;return 0;
}

/** @brief Submit a fatal owner's chain without a possibly interrupted ring lock.
 * @param q Reserved live ring. @param command Readable packet. @param response
 * Writable reply. @param length Used bytes output. @return Zero or bounded errno.
 * All peers and normal producers must be stopped permanently. Hardware avail
 * is authoritative if the interrupted producer had not committed its index.
 * Ordinary cookies and descriptor metadata are never dereferenced or reclaimed.
 */
int virtio_queue_emergency(struct virtio_queue *q,struct virtio_segment command,
                           struct virtio_segment response,uint32_t *length)
{
    if(!q || !q->emergency_reserved || q->broken || !length || !q->notify ||
       !command.physical || !command.length || command.writable ||
       !response.physical || !response.length || !response.writable ||
       command.physical>UINT64_MAX-(command.length-1u) ||
       response.physical>UINT64_MAX-(response.length-1u))return -EINVAL;
    uint16_t head=q->size-2,used=q->used_header[1],available=q->available[1];
    q->descriptors[head]=(struct virtio_descriptor){command.physical,command.length,1,(uint16_t)(head+1)};
    q->descriptors[head+1]=(struct virtio_descriptor){response.physical,response.length,2,0};
    q->available[2+(available&(q->size-1))]=head;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    q->available[1]=(uint16_t)(available+1);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    *q->notify=q->index;
    for(uint32_t attempt=0;attempt<50000000;++attempt) {
        uint16_t produced=q->used_header[1];
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if((uint16_t)(produced-used)>q->size)return -EIO;
        while(used!=produced) {
            struct virtio_used_element element=q->used_ring[used++&(q->size-1)];
            if(element.id==head) {
                if(element.length>response.length)return -EIO;
                *length=element.length;return 0;
            }
        }
        __asm__ volatile("pause");
    }
    return -ETIMEDOUT;
}
