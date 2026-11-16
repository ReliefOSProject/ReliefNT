/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/time.h>
#include <reliefnt/console.h>
#include <linux/time.h>

/** @brief Obtain a clamped cross-CPU clock instead of best-effort boot logging time.
 * @return CLOCK_MONOTONIC microseconds, suitable for unsigned deadline differences.
 */
uint64_t vg_clock_us(void)
{
    struct linux_timespec now; (void)time_clock_get(1,&now);
    return (uint64_t)now.tv_sec*1000000+(uint64_t)now.tv_nsec/1000;
}

/** @brief Translate a valid device error without treating it as successful work.
 * @param type Response type. @return Negative Linux errno.
 */
static int response_error(uint32_t type)
{
    switch (type) {
    case VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY: return -ENOMEM;
    case VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID:
    case VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER: return -EINVAL;
    default: return -EIO;
    }
}

/** @brief Mark completed requests from DMA responses without freeing their state.
 * No allocation, user access, execution lock, resource callback or sleep; IRQ safe.
 */
void vg_poll(void)
{
    for (unsigned queue = 0; queue < 2; ++queue) {
        void *cookie; uint32_t length;
        for (unsigned n = 0; n < VIRTIO_QUEUE_MAX; ++n) {
            int rc = virtio_queue_pop(&vg.queues[queue], &cookie, &length);
            if (!rc) break;
            if (rc < 0) { __atomic_store_n(&vg.fault,true,__ATOMIC_RELEASE); break; }
            struct vg_request *request = cookie;
            int result = 0;
            if (!request->cursor) {
                struct virtio_gpu_ctrl_hdr *header = request->response;
                if (length < sizeof(*header) ||
                    !(header->flags & VIRTIO_GPU_FLAG_FENCE) ||
                    header->fence_id != request->fence) result = -EIO;
                else if (header->type != request->expected) result = response_error(header->type);
                else if (length < request->response_bytes) result = -EIO;
            }
            request->result = result; request->used_bytes = length;
            __atomic_store_n(&request->done,true,__ATOMIC_RELEASE);
            /* The consumer may free cookie immediately after this store. */
        }
    }
}

/** @brief Stage a request and response into independently retained DMA pages.
 * @param command Valid kernel packet. @param bytes Command size. @param response_bytes
 * Response capacity. @param expected Response type. @param cursor Cursor fast path.
 * @return Owned request or NULL; task context, no borrowed memory survives.
 */
struct vg_request *vg_request_create(const void *command, uint32_t bytes,
    uint32_t response_bytes, uint32_t expected, bool cursor)
{
    if(vg.request_count>=120 && !vg.reaping) {
        vg_process_retirements(128);
        if(vg.request_count>=120) {
            struct vg_request *oldest=NULL;
            for(struct vg_request *r=vg.requests;r;r=r->next)if(r->automatic)oldest=r;
            if(oldest){(void)vg_request_wait(oldest);vg_process_retirements(128);}
        }
    }
    if (bytes < sizeof(struct virtio_gpu_ctrl_hdr) || bytes > VG_MAX_COMMAND ||
        response_bytes > VG_MAX_COMMAND || (!cursor && response_bytes < sizeof(struct virtio_gpu_ctrl_hdr)) ||
        vg.fault || vg.request_count >= 128) return NULL;
    bool pinned = vg.ready;
    struct vg_request *request = pinned ? drm_async_alloc(sizeof(*request)) : kernel_malloc(sizeof(*request));
    if (!request) return NULL;
    *request = (struct vg_request){ .pinned=pinned,.cursor=cursor,.expected=expected,
        .response_bytes=response_bytes };
    uint32_t response_offset = (bytes + 63u) & ~63u;
    if (dma_buffer_alloc(&request->dma,(size_t)response_offset+response_bytes,UINT64_MAX)) {
        if(pinned)drm_async_free(request);else kernel_free(request); return NULL;
    }
    if(command)__builtin_memcpy(request->dma.cpu,command,bytes);
    else __builtin_memset(request->dma.cpu,0,bytes);
    request->dma.length = bytes;
    request->response = (void *)((uint8_t *)request->dma.cpu+response_offset);
    if (response_bytes) __builtin_memset(request->response,0,response_bytes);
    return request;
}

/** @brief Atomically publish a staged packet with a unique fenced identity.
 * @param request Prepared owned request. @return Zero or errno; task/execution
 * context. A published request remains on the device list until completion/reset.
 */
int vg_request_submit(struct vg_request *request)
{
    if (!request || request->fence || vg.fault || !vg.transport.live) return -EIO;
    struct virtio_gpu_ctrl_hdr *header = request->dma.cpu;
    request->context=header->ctx_id;
    if (vg.next_fence == UINT64_MAX) return -EOVERFLOW;
    request->fence = ++vg.next_fence;
    if (!request->cursor) { header->flags |= VIRTIO_GPU_FLAG_FENCE; header->fence_id=request->fence; }
    struct virtio_segment segments[2] = {
        { .physical=request->dma.physical,.length=request->dma.length },
        { .physical=request->dma.physical + (uintptr_t)request->response - (uintptr_t)request->dma.cpu,
          .length=request->response_bytes,.writable=true },
    };
    request->submitted_us = vg_clock_us();
    int rc = virtio_queue_submit(&vg.queues[request->cursor ? 1 : 0],segments,
                                 request->cursor ? 1 : 2,request);
    if (rc) { request->fence=0; return rc; }
    request->next=vg.requests; vg.requests=request; ++vg.request_count;
    return 0;
}

/** @brief Poll a synchronous control command with a bounded reset-on-timeout path.
 * @param request Published request. @return Response errno; failed reset retains
 * all memory. Task context; queue locks are dropped before polling device state.
 */
int vg_request_wait(struct vg_request *request)
{
    for (unsigned n=0; n<50000000; ++n) {
        vg_poll();
        if (__atomic_load_n(&request->done,__ATOMIC_ACQUIRE)) return request->result;
        if (vg.fault || vg_clock_us()-request->submitted_us >= 3000000) break;
        __asm__ volatile("pause");
    }
    console_error("[virtio-gpu] request timeout/fault fence=%llu; resetting device\n",
                  (unsigned long long)request->fence);
    (void)vg_device_fault(-EIO);
    return -EIO;
}

/** @brief Consume a prepared/completed request after device DMA has ended.
 * @param request Owned request. Live DMA is deliberately retained on reset failure.
 * Task/execution context; asynchronous completion callbacks run separately.
 */
void vg_request_free(struct vg_request *request)
{
    if (!request || (request->fence && !__atomic_load_n(&request->done,__ATOMIC_ACQUIRE))) return;
    if (request->fence) {
        struct vg_request **link=&vg.requests;
        while (*link && *link!=request) link=&(*link)->next;
        if (*link) { *link=request->next; --vg.request_count; }
    }
    bool pinned=request->pinned;
    (void)dma_buffer_free(&request->dma);
    if(pinned)drm_async_free(request);else kernel_free(request);
}

/** @brief Run a fenced control request and copy only its validated response.
 * @param command Kernel packet. @param bytes Packet bytes. @param response Optional
 * response output. @param response_bytes Capacity. @param expected Required type.
 * @return Zero or errno, retaining live DMA on an unresettable device.
 */
int vg_command(const void *command,uint32_t bytes,void *response,uint32_t response_bytes,uint32_t expected)
{
    if (!response_bytes) response_bytes=sizeof(struct virtio_gpu_ctrl_hdr);
    struct vg_request *request=vg_request_create(command,bytes,response_bytes,expected,false);
    if(!request)return vg.fault ? -EIO : -ENOMEM;
    int rc=vg_request_submit(request);
    if(!rc)rc=vg_request_wait(request);
    if(!rc && response)__builtin_memcpy(response,request->response,response_bytes);
    vg_request_free(request);return rc;
}
