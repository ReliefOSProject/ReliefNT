/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include "sync.h"
#include "../file_ops.h"
#include <reliefnt/heap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/syscall.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/sched.h>
#include <reliefnt/time.h>
#include <reliefnt/futex.h>
#include <linux/sync_file.h>
#include <linux/poll.h>

struct vg_fence { uint32_t references; int status; uint64_t timestamp; };
struct vg_sync_file { char name[32]; uint32_t count; struct vg_fence *fences[]; };
static uint64_t next_inode;

/** @brief Create a pending completion; submissions and exported OFDs own refs.
 * @return Owned reference or NULL; execution ownership excludes concurrent puts.
 */
struct vg_fence *vg_fence_create(void)
{
    struct vg_fence *f=drm_async_alloc(sizeof(*f));
    if(f)*f=(struct vg_fence){.references=1};
    return f;
}

/** @brief Record a terminal hardware result once, in task-context retirement.
 * @param f Borrowed live fence, nullable. @param status Zero or negative errno.
 * No backend lock or user memory; poll sees completion after this publication.
 */
void vg_fence_signal(struct vg_fence *f,int status)
{
    if(f && !f->status){f->timestamp=vg_clock_us()*1000;f->status=status ? status : 1;}
}

/** @brief Release a submission or sync_file lease with execution ownership.
 * @param f Owned nullable reference; the final put releases the module pin.
 */
void vg_fence_put(struct vg_fence *f)
{if(f && !--f->references)drm_async_free(f);}

/** @brief Inspect a flattened fence set after servicing bounded GPU retirements.
 * @param sync Live set. @return Zero while pending, one or negative terminal errno.
 * Execution-owned; the driver retains each request until DMA really finishes.
 */
static int status(struct vg_sync_file *sync)
{
    vg_process_retirements(128);int error=0;bool pending=false;
    for(unsigned i=0;i<sync->count;++i) {
        int s=sync->fences[i]->status;
        if(!s)pending=true;else if(s<0 && !error)error=s;
    }
    return pending ? 0 : error ? error : 1;
}

/** @brief Drop the only OFD-owned fence array before provider release.
 * @param file Canonical closing/partially opened description. Task context.
 */
void vg_sync_close(struct task_file *file)
{
    if(file->kind!=TASK_FILE_KIND_DRM_SYNC || !file->aux)return;
    struct vg_sync_file *s=(void *)(uintptr_t)file->aux;file->aux=0;
    for(unsigned i=0;i<s->count;++i)vg_fence_put(s->fences[i]);
    drm_async_free(s);
}

/** @brief Build a provider-pinned anonymous OFD from borrowed fence sets.
 * @param task Caller. @param name Bounded NUL-terminated label.
 * @param a First set. @param ac First count. @param b Optional second set.
 * @param bc Second count. @param out Required descriptor output.
 * @return Zero or errno; duplicates are coalesced, all prepublication errors unwind.
 * Execution ownership serializes file promotion, close and fence references.
 */
static int publish(struct task *task,const char *name,struct vg_fence **a,unsigned ac,
                   struct vg_fence **b,unsigned bc,int *out)
{
    if(ac>VG_MAX_RESOURCES || bc>VG_MAX_RESOURCES-ac)return -E2BIG;
    struct vg_sync_file *s=drm_async_alloc(sizeof(*s)+(size_t)(ac+bc)*sizeof(*a));
    if(!s)return -ENOMEM;
    __builtin_memset(s,0,sizeof(*s));
    for(unsigned i=0;i<sizeof(s->name)-1 && name[i];++i)s->name[i]=name[i];
    for(unsigned i=0;i<ac+bc;++i) {
        struct vg_fence *f=i<ac ? a[i] : b[i-ac];bool duplicate=false;
        for(unsigned j=0;j<s->count;++j)if(s->fences[j]==f)duplicate=true;
        if(!duplicate){++f->references;s->fences[s->count++]=f;}
    }
    struct task_file *slot=NULL;int fd=task_allocate_fd(task,0,&slot);
    if(fd<0) {
        for(unsigned i=0;i<s->count;++i)vg_fence_put(s->fences[i]);
        drm_async_free(s);return fd;
    }
    slot->kind=TASK_FILE_KIND_DRM_SYNC;slot->aux=(uintptr_t)s;
    slot->flags=RELIEFOS_O_RDWR;slot->fd_flags=RELIEFOS_FD_CLOEXEC;
    slot->node.type=RELIEFOS_FS_TYPE_FILE;
    slot->drm_prime_ino=++next_inode;slot->drm_prime_uid=task->fsuid;slot->drm_prime_gid=task->fsgid;
    static const char path[]="anon_inode:[sync_file]";
    __builtin_memcpy(slot->path,path,sizeof(path));
    int rc=drm_character_attach_anonymous(task,slot);
    if(rc){vg_sync_close(slot);task_discard_file_fd(task,fd);return rc;}
    *out=fd;return 0;
}

/** @brief Export one completion with descriptor-local CLOEXEC and OFD lifetime.
 * @param task Caller. @param fence Borrowed live fence. @param fd Output.
 * @return Zero or errno; submission and OFD retain independent module pins.
 */
int vg_sync_export(struct task *task,struct vg_fence *fence,int *fd)
{return publish(task,"virtio_gpu",&fence,1,NULL,0,fd);}

/** @brief Resolve only a live Linux sync_file, never another driver's raw state.
 * @param task Caller. @param fd Descriptor. @return Borrowed set or NULL.
 * Execution ownership protects the descriptor until the current callback ends.
 */
static struct vg_sync_file *from_fd(struct task *task,int fd)
{
    struct task_file *f=task_file_for_fd(task,fd);
    return f && f->kind==TASK_FILE_KIND_DRM_SYNC ? (void *)(uintptr_t)f->aux : NULL;
}

/** @brief Wait before submission until all imported dependencies terminate.
 * @param task Caller. @param fd Input descriptor.
 * @return Zero, EINVAL, device errno or internal blocked status. Task context,
 * no lock across sleeping; retries occur before any command/fd publication.
 */
int vg_sync_wait_fd(struct task *task,int fd)
{
    struct vg_sync_file *s=from_fd(task,fd);if(!s)return -EINVAL;
    int rc=status(s);if(rc)return rc<0 ? rc : 0;
    sched_sleep_current_until(time_ticks()+1);return KERNEL_SYSCALL_BLOCKED;
}

/** @brief Apply Linux sync_file readiness to an actual completed fence set.
 * @param file Canonical OFD. @param events Requested bits.
 * @return POLLIN after success or failure; no successful early completion.
 */
short vg_sync_poll(struct task_file *file,short events)
{return status((void *)(uintptr_t)file->aux) ? (events&POLLIN) : 0;}

/** @brief Import a copied argument before mutation, respecting swap prerequisites.
 * @param task Caller. @param to Kernel buffer. @param from User address.
 * @param bytes Exact length. @return Zero, EFAULT or private retry.
 */
static int read_argument(struct task *task,void *to,uint64_t from,size_t bytes)
{
#ifdef CONFIG_SWAP
    return drm_user_import(task,to,from,bytes);
#else
    return user_copy_from_task(task,to,from,bytes)<0 ? -EFAULT : 0;
#endif
}

/** @brief Prepare a user output before irreversible namespace changes.
 * @param task Caller. @param address User range. @param bytes Exact length.
 * @return Zero, EFAULT or private retry; no pointer is retained.
 */
static int prepare(struct task *task,uint64_t address,size_t bytes)
{
#ifdef CONFIG_SWAP
    int rc=drm_user_prepare(task,address,bytes,true);if(rc)return rc;
#else
    (void)task;
#endif
    return user_range_writable(address,bytes) ? 0 : -EFAULT;
}

/** @brief Implement standard sync_file queries/merge without borrowing user data.
 * @param task Caller. @param file Admitted description. @param command Selector.
 * @param argument User address. @return Zero, errno or prerequisite retry.
 * Execution-owned; merge copyout failure discards its new fd exactly once.
 */
int64_t vg_sync_ioctl(struct task *task,struct task_file *file,uint32_t command,uint64_t argument)
{
    struct vg_sync_file *s=(void *)(uintptr_t)file->aux;
    if(command==SYNC_IOC_SET_DEADLINE) {
        struct sync_set_deadline hint;int rc=read_argument(task,&hint,argument,sizeof(hint));
        if(rc)return rc;return hint.pad ? -EINVAL : 0; /* optional scheduling hint */
    }
    if(command==SYNC_IOC_MERGE) {
        struct sync_merge_data data;int rc=read_argument(task,&data,argument,sizeof(data));
        if(rc)return rc;if(data.flags || data.pad)return -EINVAL;
        struct vg_sync_file *other=from_fd(task,data.fd2);if(!other)return -ENOENT;
        rc=prepare(task,argument,sizeof(data));if(rc)return rc;
        data.name[sizeof(data.name)-1]=0;
        rc=publish(task,data.name,s->fences,s->count,other->fences,other->count,&data.fence);
        if(rc)return rc;
        if(user_copy_to_task(task,argument,&data,sizeof(data))<0) {
            task_discard_file_fd(task,data.fence);return -EFAULT;
        }
        return 0;
    }
    if(command==SYNC_IOC_FILE_INFO) {
        struct sync_file_info info;int rc=read_argument(task,&info,argument,sizeof(info));
        if(rc)return rc;if(info.flags || info.pad)return -EINVAL;
        bool details=info.num_fences!=0;
        if(details && info.num_fences<s->count)return -EINVAL;
        rc=prepare(task,argument,sizeof(info));if(rc)return rc;
        if(details){rc=prepare(task,info.sync_fence_info,(size_t)s->count*sizeof(struct sync_fence_info));if(rc)return rc;}
        info.status=status(s);info.num_fences=s->count;
        __builtin_memcpy(info.name,s->name,sizeof(info.name));
        if(details)for(unsigned i=0;i<s->count;++i) {
            struct sync_fence_info f={.status=s->fences[i]->status,.timestamp_ns=s->fences[i]->timestamp};
            __builtin_memcpy(f.obj_name,"virtio_gpu",11);__builtin_memcpy(f.driver_name,"virtio_gpu",11);
            if(user_copy_to_task(task,info.sync_fence_info+(size_t)i*sizeof(f),&f,sizeof(f))<0)return -EFAULT;
        }
        return user_copy_to_task(task,argument,&info,sizeof(info))<0 ? -EFAULT : 0;
    }
    return -ENOTTY;
}
