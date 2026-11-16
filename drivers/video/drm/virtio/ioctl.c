/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "sync.h"
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/futex.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/console.h>

struct vg_submission { uint32_t count; struct vg_fence *fence; struct drm_bo *objects[]; };

/** @brief Import a nested input before any externally visible submission.
 * @param task Caller. @param to Owned kernel bytes. @param from User address.
 * @param length Exact bytes. @return Zero, errno or prerequisite retry.
 */
static int import(struct task *task,void *to,uint64_t from,uint32_t length)
{
#ifdef CONFIG_SWAP
    return drm_user_import(task,to,from,length);
#else
    return user_copy_from_task(task,to,from,length)<0 ? -EFAULT : 0;
#endif
}

/** @brief Prepare a nested output before hardware/namespace mutations.
 * @param task Caller. @param address User range. @param length Exact bytes.
 * @return Zero, errno or private prerequisite wait; no range is retained.
 */
static int output(struct task *task,uint64_t address,uint32_t length)
{
#ifdef CONFIG_SWAP
    int rc=drm_user_prepare(task,address,length,true);if(rc)return rc;
#endif
    (void)task;
    return user_range_writable(address,length) ? 0 : -EFAULT;
}

/** @brief Release all resource reservations after the actual GPU fence completes.
 * @param request Completed request, task-context retirement only.
 */
static void submission_complete(struct vg_request *request)
{
    struct vg_submission *submission=request->private;
    vg_fence_signal(submission->fence,request->result);
    vg_fence_put(submission->fence);
    for(unsigned i=0;i<submission->count;++i) {
        if(request->result)drm_bo_virtio(submission->objects[i])->error=request->result;
        drm_bo_gpu_release(submission->objects[i]);drm_bo_put(submission->objects[i]);
    }
    drm_async_free(submission);
}

/** @brief Import and submit a VirGL command stream with immutable BO authority.
 * @param task Caller. @param file Authenticated/render OFD. @param arg Copied args.
 * @return Zero or errno; user buffers never outlive import, DMA/BO pins outlive GPU.
 */
static int execbuffer(struct task *task,struct drm_file *file,struct drm_virtgpu_execbuffer *arg,int *created_fd)
{
    if(!vg.virgl)return -EOPNOTSUPP;
    if((arg->flags&~(VIRTGPU_EXECBUF_FENCE_FD_IN|VIRTGPU_EXECBUF_FENCE_FD_OUT)) ||
        arg->ring_idx || arg->num_in_syncobjs || arg->num_out_syncobjs)return -EINVAL;
    if(!arg->size || (arg->size&3) || arg->size>VG_MAX_COMMAND-sizeof(struct virtio_gpu_cmd_submit) ||
        arg->num_bo_handles>VG_MAX_RESOURCES)return -EINVAL;
    if(arg->flags&VIRTGPU_EXECBUF_FENCE_FD_IN) {
        int rc=vg_sync_wait_fd(task,arg->fence_fd);if(rc)return rc;
    }
    uint32_t *handles=kernel_malloc((size_t)arg->num_bo_handles*4+1);
    if(!handles)return -ENOMEM;
    int rc=import(task,handles,arg->bo_handles,arg->num_bo_handles*4);
    if(rc){kernel_free(handles);return rc;}
    struct vg_request *request=vg_request_create(NULL,sizeof(struct virtio_gpu_cmd_submit)+arg->size,
        sizeof(struct virtio_gpu_ctrl_hdr),VIRTIO_GPU_RESP_OK_NODATA,false);
    if(!request){kernel_free(handles);return vg.fault ? -EIO : -ENOMEM;}
    struct virtio_gpu_cmd_submit *command=request->dma.cpu;
    rc=import(task,command+1,arg->command,arg->size);
    if(rc)goto out;
    struct vg_submission *submission=drm_async_alloc(sizeof(*submission)+(size_t)arg->num_bo_handles*sizeof(struct drm_bo *));
    if(!submission){rc=-ENOMEM;goto out;}
    submission->count=0;submission->fence=NULL;int fence_fd=-1;
    for(unsigned i=0;i<arg->num_bo_handles;++i) {
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,handles[i]);
        if(!bo || !drm_bo_virtio(bo)){drm_bo_put(bo);rc=-ENOENT;break;}
        rc=vg_context_attach(file,bo);
        if(!rc)rc=drm_bo_gpu_reserve(bo);
        if(rc){drm_bo_put(bo);break;}
        submission->objects[submission->count++]=bo;
    }
    if(!rc)rc=vg_context_create(file);
    if(!rc && (arg->flags&VIRTGPU_EXECBUF_FENCE_FD_OUT)) {
        submission->fence=vg_fence_create();
        rc=submission->fence ? vg_sync_export(task,submission->fence,&fence_fd) : -ENOMEM;
    }
    if(!rc) {
        command->hdr=(struct virtio_gpu_ctrl_hdr){.type=VIRTIO_GPU_CMD_SUBMIT_3D,.ctx_id=file->virtio->context};
        command->size=arg->size;
        request->private=submission;request->complete=submission_complete;request->automatic=true;
        rc=vg_request_submit(request);
        if(!rc) {
            for(unsigned i=0;i<submission->count;++i)drm_bo_virtio(submission->objects[i])->last_fence=request->fence;
            if(fence_fd>=0){arg->fence_fd=fence_fd;*created_fd=fence_fd;}
            kernel_free(handles);return 0;
        }
    }
    if(fence_fd>=0)task_discard_file_fd(task,fence_fd);
    vg_fence_put(submission->fence);
    for(unsigned i=0;i<submission->count;++i) {
        drm_bo_gpu_release(submission->objects[i]);drm_bo_put(submission->objects[i]);
    }
    drm_async_free(submission);
out:
    kernel_free(handles);vg_request_free(request);return rc;
}

/** @brief Wait for one GEM reservation, observing both IRQ and polled completions.
 * @param task Caller. @param bo Retained BO. @param nowait Return EBUSY if pending.
 * @return Zero, EBUSY, EIO or internal scheduler-blocked status; no lock across sleep.
 */
static int wait_bo(struct task *task,struct drm_bo *bo,bool nowait)
{
    vg_process_retirements(128);
    if(vg.fault)return -EIO;
    if(drm_bo_virtio(bo)->error)return drm_bo_virtio(bo)->error;
    if(!drm_bo_gpu_busy(bo))return 0;
    if(nowait)return -EBUSY;
    (void)task;sched_sleep_current_until(time_ticks()+1);
    return KERNEL_SYSCALL_BLOCKED;
}

/** @brief Transfer a bounded resource box between guest backing and host storage.
 * @param file Authorized OFD. @param arg Copied transfer. @param from_host Readback.
 * @return Zero or errno; synchronous fenced DMA makes subsequent CPU reads safe.
 */
static int transfer(struct drm_file *file,struct drm_virtgpu_3d_transfer_to_host *arg,bool from_host)
{
    struct drm_bo *bo=drm_bo_lookup_handle_get(file,arg->bo_handle);
    struct vg_resource *r=drm_bo_virtio(bo);
    if(!r){drm_bo_put(bo);return -ENOENT;}
    int rc=0;
    if(arg->level>=r->levels || !arg->box.w || !arg->box.h || !arg->box.d || arg->offset>=drm_bo_size(bo)) {
        rc=-EINVAL;goto out;
    }
    uint32_t width=r->width>>arg->level,height=r->height>>arg->level,depth=r->depth>>arg->level;
    if(!width)width=1;if(!height)height=1;if(!depth)depth=1;
    /* Array/cube layers use z; the host validates compressed block layout. */
    if(r->array_size>depth)depth=r->array_size;
    if(arg->box.x>width || arg->box.w>width-arg->box.x || arg->box.y>height ||
        arg->box.h>height-arg->box.y || arg->box.z>depth || arg->box.d>depth-arg->box.z) {
        rc=-EINVAL;goto out;
    }
    if(r->virgl) {
        rc=vg_context_attach(file,bo);if(rc)goto out;
        struct virtio_gpu_transfer_host_3d command={.hdr={
            .type=from_host ? VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
            .ctx_id=file->virtio->context},.resource_id=r->id,.level=arg->level,.offset=arg->offset,
            .box={arg->box.x,arg->box.y,arg->box.z,arg->box.w,arg->box.h,arg->box.d},
            .stride=arg->stride,.layer_stride=arg->layer_stride};
        rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    } else if(from_host)rc=-EOPNOTSUPP;
    else {
        struct virtio_gpu_transfer_to_host_2d command={.hdr.type=VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
            .resource_id=r->id,.offset=arg->offset,.r={arg->box.x,arg->box.y,arg->box.w,arg->box.h}};
        rc=vg_command(&command,sizeof(command),NULL,0,VIRTIO_GPU_RESP_OK_NODATA);
    }
out:
    drm_bo_put(bo);return rc;
}

/** @brief Return one actual supported VirtGPU parameter using Linux's int output.
 * @param task Caller. @param arg Copied query. @return Zero or errno/retry marker.
 */
static int getparam(struct task *task,struct drm_virtgpu_getparam *arg)
{
    int32_t value=0;
    switch(arg->param) {
    case VIRTGPU_PARAM_3D_FEATURES:value=vg.virgl;break;
    case VIRTGPU_PARAM_CAPSET_QUERY_FIX:value=1;break;
    case VIRTGPU_PARAM_RESOURCE_BLOB:case VIRTGPU_PARAM_HOST_VISIBLE:
    case VIRTGPU_PARAM_CROSS_DEVICE:case VIRTGPU_PARAM_CONTEXT_INIT:
    case VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs:case VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME:break;
    default:return -EINVAL;
    }
    int rc=output(task,arg->value,sizeof(value));if(rc)return rc;
    return user_copy_to_task(task,arg->value,&value,sizeof(value))<0 ? -EFAULT : 0;
}

/** @brief Read only a negotiated VirGL capset, never advertise unsupported protocols.
 * @param task Caller. @param arg Copied query. @return Zero or errno/retry.
 */
static int getcaps(struct task *task,struct drm_virtgpu_get_caps *arg)
{
    if(!arg->size)return -EINVAL;
    struct vg_capset *cap=NULL;
    for(unsigned i=0;i<vg.capset_count;++i)
        if(vg.capsets[i].id==arg->cap_set_id && arg->cap_set_ver<=vg.capsets[i].version)cap=&vg.capsets[i];
    if(!cap)return -EINVAL;
    uint32_t bytes=arg->size<cap->size ? arg->size : cap->size;
    int rc=output(task,arg->addr,bytes);if(rc)return rc;
    struct virtio_gpu_get_capset command={.hdr.type=VIRTIO_GPU_CMD_GET_CAPSET,
        .capset_id=arg->cap_set_id,.capset_version=arg->cap_set_ver};
    uint32_t response_bytes=sizeof(struct virtio_gpu_ctrl_hdr)+cap->size;
    struct vg_request *r=vg_request_create(&command,sizeof(command),response_bytes,VIRTIO_GPU_RESP_OK_CAPSET,false);
    if(!r)return -ENOMEM;
    rc=vg_request_submit(r);if(!rc)rc=vg_request_wait(r);
    if(!rc && user_copy_to_task(task,arg->addr,r->response+1,bytes)<0)rc=-EFAULT;
    vg_request_free(r);return rc;
}

/** @brief Dispatch copied VirtGPU UAPI with transactional GEM handle publication.
 * @param task Current caller. @param file Authorized OFD. @param command Canonical
 * selector. @param data Kernel argument. @param created_handle Rollback output.
 * @return Zero, errno or scheduler/prerequisite retry; no user pointer is queued.
 */
int vg_ioctl(struct task *task,struct drm_file *file,uint32_t command,void *data,uint32_t *created_handle,int *created_fd)
{
    vg_process_retirements(128);
    if(!vg.ready || vg.fault)return -EIO;
    switch(command) {
    case DRM_IOCTL_VIRTGPU_GETPARAM:return getparam(task,data);
    case DRM_IOCTL_VIRTGPU_GET_CAPS:return getcaps(task,data);
    case DRM_IOCTL_VIRTGPU_EXECBUFFER:return execbuffer(task,file,data,created_fd);
    case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: {
        struct drm_virtgpu_resource_create *arg=data;
        uint32_t handle;uint64_t offset;
        int rc=drm_bo_create(file,NULL,arg->size ? arg->size : 4096,&handle,&offset);if(rc)return rc;
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,handle);
        rc=vg_resource_create(bo,arg,vg.virgl);
        if(!rc && vg.virgl)rc=vg_context_attach(file,bo);
        if(!rc) {arg->bo_handle=handle;arg->res_handle=drm_bo_virtio(bo)->id;*created_handle=handle;}
        drm_bo_put(bo);
        if(rc)(void)drm_bo_handle_delete(file,handle);
        return rc;
    }
    case DRM_IOCTL_VIRTGPU_MAP: {
        struct drm_virtgpu_map *arg=data;
        if(arg->pad)return -EINVAL;
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,arg->handle);
        if(!bo)return -ENOENT;
        arg->offset=drm_bo_offset(bo);drm_bo_put(bo);return 0;
    }
    case DRM_IOCTL_VIRTGPU_RESOURCE_INFO: {
        struct drm_virtgpu_resource_info *arg=data;
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,arg->bo_handle);
        struct vg_resource *r=drm_bo_virtio(bo);
        if(!r){drm_bo_put(bo);return -ENOENT;}
        arg->res_handle=r->id;arg->size=drm_bo_size(bo);arg->blob_mem=0;drm_bo_put(bo);return 0;
    }
    case DRM_IOCTL_VIRTGPU_WAIT: {
        struct drm_virtgpu_3d_wait *arg=data;if(arg->flags&~VIRTGPU_WAIT_NOWAIT)return -EINVAL;
        struct drm_bo *bo=drm_bo_lookup_handle_get(file,arg->handle);if(!bo)return -ENOENT;
        int rc=wait_bo(task,bo,arg->flags&VIRTGPU_WAIT_NOWAIT);drm_bo_put(bo);return rc;
    }
    case DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST:return transfer(file,data,false);
    case DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST:return transfer(file,data,true);
    case DRM_IOCTL_VIRTGPU_CONTEXT_INIT:case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB:return -EOPNOTSUPP;
    default:return -EINVAL;
    }
}
