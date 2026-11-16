/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include <reliefnt/usercopy.h>
#include <reliefnt/time.h>

/** @brief Publish an actual completed flip with a monotonic virtual-vblank timestamp.
 * @param file Live OFD. @param event Owned allocation. @param output Hardware
 * index. @param user_data Uninterpreted client value. Task/execution context.
 */
void vg_event_publish(struct drm_file *file,struct vg_event *event,unsigned output,uint64_t user_data)
{
    uint64_t usec=vg_clock_us();
    event->payload=(struct drm_event_vblank){.base={DRM_EVENT_FLIP_COMPLETE,sizeof(event->payload)},
        .user_data=user_data,.tv_sec=usec/1000000,.tv_usec=usec%1000000,
        .sequence=++vg_outputs[output].sequence,.crtc_id=VG_CRTC+output};
    event->next=NULL;
    if(file->virtio_events_tail)file->virtio_events_tail->next=event;
    else file->virtio_events=event;
    file->virtio_events_tail=event;file->virtio_event_bytes+=sizeof(event->payload);
    (void)kernel_wait_queue_wake_all(&file->event_wait);
}

/** @brief Copy complete events, consuming each only after successful usercopy.
 * @param task Caller. @param file OFD. @param buffer User destination.
 * @param length Capacity. @return Bytes, zero if empty or errno/retry.
 */
int vg_events_read(struct task *task,struct drm_file *file,uint64_t buffer,uint32_t length)
{
    if(!file->virtio_events)return 0;
    if(length<sizeof(struct drm_event_vblank))return -EINVAL;
    uint32_t capacity=length<file->virtio_event_bytes ? length : file->virtio_event_bytes;
    capacity-=capacity%sizeof(struct drm_event_vblank);
#ifdef CONFIG_SWAP
    int rc=drm_user_prepare(task,buffer,capacity,true);if(rc)return rc;
#endif
    uint32_t copied=0;
    while(file->virtio_events && capacity-copied>=sizeof(struct drm_event_vblank)) {
        struct vg_event *event=file->virtio_events;
        if(user_copy_to_task(task,buffer+copied,&event->payload,sizeof(event->payload))<0)
            return copied ? (int)copied : -EFAULT;
        copied+=sizeof(event->payload);file->virtio_event_bytes-=sizeof(event->payload);
        file->virtio_events=event->next;drm_async_free(event);
    }
    if(!file->virtio_events)file->virtio_events_tail=NULL;
    return copied;
}

/** @brief Consume unread event allocations while their OFD/provider is retained.
 * @param file Closing OFD. Task/execution context, no hardware or user access.
 */
void vg_events_close(struct drm_file *file)
{
    while(file->virtio_events) {
        struct vg_event *event=file->virtio_events;file->virtio_events=event->next;drm_async_free(event);
    }
    file->virtio_events_tail=NULL;file->virtio_event_bytes=0;
}
