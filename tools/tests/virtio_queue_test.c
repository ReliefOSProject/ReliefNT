/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/virtio.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

struct fatal_device {struct virtio_queue *queue;uint16_t available,ordinary;bool oversize;};

/** @brief Model a device completing pending normal work before the fatal fence.
 * @param value Test-owned ring and completion settings. @return NULL.
 * Separate device thread; driver cookies are deliberately never interpreted.
 */
static void *complete_fatal(void *value)
{
    struct fatal_device *device=value;struct virtio_queue *q=device->queue;
    while(__atomic_load_n(&q->available[1],__ATOMIC_ACQUIRE)==device->available) {}
    uint16_t head=q->available[2+(device->available&(q->size-1))];
    assert(head==q->size-2 && q->descriptors[head].flags==1);
    assert(q->descriptors[head+1].flags==2);
    uint16_t used=q->used_header[1];
    q->used_ring[used++&(q->size-1)]=(struct virtio_used_element){device->ordinary,24};
    q->used_ring[used++&(q->size-1)]=(struct virtio_used_element){head,device->oversize ? 25 : 24};
    __atomic_store_n(&q->used_header[1],used,__ATOMIC_RELEASE);
    return NULL;
}

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ (void)lock; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)lock; (void)flags; }

int main(void)
{
    struct virtio_queue q = {0};
    void *memory = aligned_alloc(4096, 8192);
    assert(memory);
    assert(virtio_queue_bind(&q, memory, 0x100002000ULL, 8192, 8) == 0);
    struct virtio_segment segments[] = {
        { .physical = 0x200004000ULL, .length = 32 },
        { .physical = 0x300008000ULL, .length = 24, .writable = true },
    };
    int cookies[8];
    uint16_t heads[4];
    for (unsigned i = 0; i < 4; ++i) {
        assert(virtio_queue_submit(&q, segments, 2, &cookies[i]) == 0);
        heads[i] = q.available[2 + i];
        assert(q.descriptors[heads[i]].address == segments[0].physical);
        assert(q.descriptors[heads[i]].flags == 1);
        uint16_t next = q.descriptors[heads[i]].next;
        assert(q.descriptors[next].address == segments[1].physical);
        assert(q.descriptors[next].flags == 2);
    }
    assert(virtio_queue_submit(&q, segments, 2, &cookies[4]) == -28);
    q.used_ring[0] = (struct virtio_used_element){ heads[2], 24 };
    q.used_ring[1] = (struct virtio_used_element){ heads[0], 24 };
    q.used_header[1] = 2;
    void *cookie = NULL; uint32_t length = 0;
    assert(virtio_queue_pop(&q, &cookie, &length) == 1);
    assert(cookie == &cookies[2] && length == 24 && q.free_count == 2);
    assert(virtio_queue_pop(&q, &cookie, &length) == 1);
    assert(cookie == &cookies[0] && q.free_count == 4);
    assert(virtio_queue_pop(&q, &cookie, &length) == 0);
    q.used_ring[2] = (struct virtio_used_element){ heads[1], 24 };
    q.used_ring[3] = (struct virtio_used_element){ heads[3], 24 };
    q.used_header[1] = 4;
    assert(virtio_queue_pop(&q, &cookie, &length) == 1);
    assert(virtio_queue_pop(&q, &cookie, &length) == 1);
    /* Advance both 16-bit counters through a complete wrap, reclaiming only
     * real device completions and preserving all eight descriptors. */
    for (unsigned i = 0; i < 131080; ++i) {
        assert(virtio_queue_submit(&q, segments, 2, &cookies[0]) == 0);
        uint16_t head = q.available[2 + ((uint16_t)(q.available_index - 1) & 7)];
        q.used_ring[q.used_index & 7] = (struct virtio_used_element){ head, 24 };
        q.used_header[1] = (uint16_t)(q.used_index + 1);
        assert(virtio_queue_pop(&q, &cookie, &length) == 1);
        assert(cookie == &cookies[0] && q.free_count == 8);
    }
    assert(virtio_queue_submit(&q, segments, 2, &cookies[0]) == 0);
    q.used_ring[q.used_index & 7] = (struct virtio_used_element){ 99, 24 };
    q.used_header[1] = (uint16_t)(q.used_index + 1);
    assert(virtio_queue_pop(&q, &cookie, &length) == -5);
    assert(q.broken && q.free_count == 6); /* retain DMA on malformed completion */
    assert(virtio_queue_submit(&q, segments, 2, &cookies[0]) == -5);
    memset(&q, 0, sizeof(q));
    assert(virtio_queue_bind(&q, memory, 0x100002000ULL, 8192, 7) == -22);
    assert(virtio_queue_bind(&q, memory, 0x100002000ULL, 16, 8) == -22);
    assert(virtio_queue_bind(&q, memory, 0x100002000ULL, 8192, 8) == 0);
    assert(virtio_queue_submit(&q, segments, 2, &cookies[0]) == 0);
    uint16_t head = q.available[2];
    q.used_ring[0] = (struct virtio_used_element){ head, 25 };
    q.used_header[1] = 1;
    assert(virtio_queue_pop(&q, &cookie, &length) == -5);
    assert(q.free_count == 6);
    for(unsigned invalid=0;invalid<2;++invalid) {
        memset(&q,0,sizeof(q));
        assert(virtio_queue_bind(&q,memory,0x100002000ULL,8192,8)==0);
        assert(virtio_queue_reserve_emergency(&q)==0 && q.free_count==6);
        assert(virtio_queue_reserve_emergency(&q)==-22);
        volatile uint16_t notify=0;q.notify=&notify;
        for(unsigned i=0;i<3;++i)assert(virtio_queue_submit(&q,segments,2,&cookies[i])==0);
        assert(q.free_count==0 && virtio_queue_submit(&q,segments,2,&cookies[3])==-28);
        struct fatal_device device={&q,q.available_index,q.available[2],invalid};
        pthread_t thread;assert(!pthread_create(&thread,NULL,complete_fatal,&device));
        assert(virtio_queue_emergency(&q,segments[0],segments[1],&length)==(invalid ? -5 : 0));
        assert(!pthread_join(thread,NULL));
        /* Fatal takeover neither locks nor reclaims interrupted normal work. */
        assert(q.pending==3 && q.free_count==0 && q.cookies[device.ordinary]==&cookies[0]);
        if(!invalid)assert(length==24);
    }
    free(memory);
    puts("virtio split queue: out-of-order, wrap, backpressure, DMA retention, fatal reserve passed");
    return 0;
}
