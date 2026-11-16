/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/heap.h>

/** @brief Lazily create an isolated host VirGL context for one open description.
 * @param file Live authenticated/render OFD. @return Zero or errno; task context.
 */
int vg_context_create(struct drm_file *file)
{
    if(!file || !vg.virgl)return -EOPNOTSUPP;
    if(file->virtio)return 0;
    if(vg.next_context==UINT32_MAX)return -ENOSPC;
    struct vg_file *context=drm_async_alloc(sizeof(*context));if(!context)return -ENOMEM;
    *context=(struct vg_file){.context=++vg.next_context};
    static const char name[]="ReliefNT VirGL";
    struct virtio_gpu_ctx_create command={.hdr={.type=VIRTIO_GPU_CMD_CTX_CREATE,.ctx_id=context->context},
        .nlen=sizeof(name)-1};
    __builtin_memcpy(command.debug_name,name,sizeof(name)-1);
    int rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(rc){drm_async_free(context);return rc;}
    file->virtio=context;return 0;
}

/** @brief Add a resource to one context without extending the GEM handle lifetime.
 * @param file Live OFD. @param bo Retained resource BO. @return Zero or errno.
 * Dual linked metadata is removed by either context close or final BO release.
 */
int vg_context_attach(struct drm_file *file,struct drm_bo *bo)
{
    struct vg_resource *r=drm_bo_virtio(bo);if(!r)return -EINVAL;
    int rc=vg_context_create(file);if(rc)return rc;
    for(struct vg_file_resource *a=file->virtio->resources;a;a=a->next)if(a->resource==r)return 0;
    struct vg_file_resource *a=kernel_malloc(sizeof(*a));if(!a)return -ENOMEM;
    struct virtio_gpu_ctx_resource command={.hdr={.type=VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
        .ctx_id=file->virtio->context},.resource_id=r->id};
    rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    if(rc){kernel_free(a);return rc;}
    *a=(struct vg_file_resource){.next=file->virtio->resources,.resource_next=r->contexts,
        .resource=r,.owner=file->virtio};
    file->virtio->resources=a;r->contexts=a;return 0;
}

/** @brief Unlink one dual-listed attachment after hardware no longer needs it.
 * @param a Owned attachment; execution ownership excludes concurrent close.
 */
static void unlink_attachment(struct vg_file_resource *a)
{
    struct vg_file_resource **link=&a->owner->resources;
    while(*link && *link!=a)link=&(*link)->next;
    if(*link)*link=a->next;
    link=&a->resource->contexts;
    while(*link && *link!=a)link=&(*link)->resource_next;
    if(*link)*link=a->resource_next;
    kernel_free(a);
}

/** @brief Detach a final resource from every live context before host unref.
 * @param r Final resource. @return Zero or protocol errno; task context.
 */
int vg_context_detach_resource(struct vg_resource *r)
{
    int rc=0;
    while(r->contexts) {
        struct vg_file_resource *a=r->contexts;
        if(vg.transport.live && !vg.fault) {
            struct virtio_gpu_ctx_resource command={.hdr={.type=VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
                .ctx_id=a->owner->context},.resource_id=r->id};
            rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
            if(rc && vg.transport.live && vg_device_fault(rc))return rc;
        }
        unlink_attachment(a);
    }
    return rc;
}

/** @brief Fence outstanding context submissions before destroying its authority.
 * @param file Closing OFD; all attachment metadata is consumed before return.
 * Failed reset retains context/module metadata rather than freeing live state.
 */
void vg_context_close(struct drm_file *file)
{
    struct vg_file *context=file->virtio;if(!context)return;
    for(struct vg_request *r=vg.requests;r;r=r->next) {
        if(r->context==context->context && !r->done && vg_request_wait(r) && vg.transport.live)return;
    }
    if(vg.transport.live && !vg.fault) {
        struct virtio_gpu_ctx_destroy command={.hdr={.type=VIRTIO_GPU_CMD_CTX_DESTROY,.ctx_id=context->context}};
        if(vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA) && vg.transport.live &&
            vg_device_fault(-EIO))return;
    }
    while(context->resources)unlink_attachment(context->resources);
    file->virtio=NULL;drm_async_free(context);
}
