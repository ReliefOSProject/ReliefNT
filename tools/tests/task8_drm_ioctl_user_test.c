/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm ioctl user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_drm_main
#include "drm_core_test.c"
#undef main
#include <reliefnt/swap.h>
static uint64_t blocked;
static bool block_write,contended;
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{assert(task && length<=32768);return contended && address==blocked && writing==block_write ? SWAP_STORAGE_CONTENDED:0;}
void storage_drain_task_io(uint32_t pid){(void)pid;}
void kernel_wait_queue_remove(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;assert(0);}
void vfs_walk_cancel(struct vfs_walk **w){assert(!*w);}
void vfs_path_put(struct vfs_path *p){assert(!p->mount);}
void storage_inode_allow_write(struct storage_inode_ref *i){(void)i;assert(0);}
int storage_inode_put(struct storage_inode_ref *i){
#ifdef TASK8_DRM_CLEANUP_TEST
 assert(!i);return 0;
#else
 (void)i;assert(0);return 0;
#endif
}
void task_process_vm_scope_release(void *s){(void)s;assert(0);}
static int64_t scoped_ioctl(struct task *t,struct task_file *f,uint32_t cmd,void *arg)
{struct trap_frame frame={.rax=LINUX_SYS_IOCTL,.rdi=3,.rsi=cmd,.rdx=(uintptr_t)arg};
 task_lookup_scope_begin(t,&frame);return drm_file_ioctl(t,f,cmd,(uintptr_t)arg);}
#ifndef TASK8_DRM_NO_MAIN
int main(void)
{
    struct task task=make_task(500,false);task.kind=TASK_KIND_USER;task.state=TASK_READY;
    struct drm_master master={.next_magic=1};struct drm_file device={.master=&master};
    struct task_file file={.drm_file=&device,.character_file=(struct character_file *)1};task.syscall_file=&file;
    char name[16]={0},date[16]={0},replacement[16]={0};
    struct drm_version version={.name=name,.name_len=sizeof(name),.date=date,.date_len=sizeof(date)};
    blocked=(uintptr_t)date;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VERSION,&version)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!name[0] && !date[0]);version.name=replacement;
    contended=false;assert(!scoped_ioctl(&task,&file,DRM_IOCTL_VERSION,&version));
    assert(!strcmp(name,"vmwgfx") && !strcmp(date,"0") && !replacement[0]);
    task_lookup_scope_cancel(&task);
    struct drm_mode_modeinfo mode={0},other_mode={0};uint32_t encoder=0;
    struct drm_mode_get_connector connector={.connector_id=DRM_KMS_CONNECTOR_ID,.count_modes=1,
        .modes_ptr=(uintptr_t)&mode,.count_encoders=1,.encoders_ptr=(uintptr_t)&encoder};
    blocked=(uintptr_t)&encoder;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETCONNECTOR,&connector)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!mode.hdisplay && !encoder);connector.modes_ptr=(uintptr_t)&other_mode;contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETCONNECTOR,&connector));
    assert(mode.hdisplay==DRM_TEST_WIDTH && !other_mode.hdisplay && encoder==DRM_KMS_ENCODER_ID);
    task_lookup_scope_cancel(&task);
    struct drm_vmw_fence_rep fence_reply={0};
    struct drm_vmw_execbuf_arg exec={.version=1,.fence_rep=(uintptr_t)&fence_reply};
    unsigned before_work=drm_vmwgfx_exec_work_count;uint32_t before_fence=drm_test_fence_issued;
    blocked=(uintptr_t)&fence_reply;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_EXECBUF,&exec)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(drm_vmwgfx_exec_work_count==before_work && drm_test_fence_issued==before_fence && !fence_reply.handle);
    task_lookup_scope_cancel(&task);contended=false;
    struct drm_auth auth={0};blocked=(uintptr_t)&auth;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_GET_MAGIC,&auth)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!device.magic && !master.magics);contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_GET_MAGIC,&auth) && auth.magic==1 && device.magic==1);
    task_lookup_scope_cancel(&task);kernel_free(master.magics);
    /* The original PRIME OFD and typed surface must survive a page-in wait. */
    struct drm_device gpu={0};device.device=&gpu;
    struct drm_vmwgfx_surface surface={.resource={.id=DRM_TEST_RESOURCE_HANDLE_MIN,
        .type=DRM_VMW_RESOURCE_SURFACE,.state=DRM_VMW_RESOURCE_LIVE,
        .logical_references=1,.prime_references=1},
        .metadata={.desc={.width=64,.height=32,.depth=1},.shareable=true}};
    drm_vmwgfx_all_resources=&surface.resource;
    struct drm_prime_file original_prime={.surface=&surface,.device=&gpu};
    struct task_file *original=&drm_test_ofd_slots[0];
    *original=(struct task_file){.used=1,.references=1,.drm_prime_file=&original_prime};
    struct task_file *slot=&drm_test_fd_slots[drm_test_fd_group(&task)][4];
    *slot=(struct task_file){.used=1,.description=original};
    struct drm_vmw_size observed={0};union drm_vmw_surface_reference_arg ref={0};
    ref.req.sid=4;ref.req.handle_type=DRM_VMW_HANDLE_PRIME;ref.rep.size_addr=(uintptr_t)&observed;
    blocked=(uintptr_t)&observed;block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(task.drm_vmwgfx_continuation && original->references==2 && surface.resource.logical_references==1);
    *slot=(struct task_file){.used=1};--original->references;contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref));
    assert(observed.width==64 && observed.height==32 && !task.drm_vmwgfx_continuation);
    assert(!original->used && surface.resource.logical_references==2);
    assert(!drm_vmwgfx_drop_file_reference(&device,surface.resource.id,DRM_VMW_RESOURCE_SURFACE));
    task_lookup_scope_cancel(&task);
    /* Common cancellation detaches immediately and queued cleanup owns both refs. */
    *original=(struct task_file){.used=1,.references=1,.drm_prime_file=&original_prime};
    *slot=(struct task_file){.used=1,.description=original};
    ref=(union drm_vmw_surface_reference_arg){0};ref.req.sid=4;ref.req.handle_type=DRM_VMW_HANDLE_PRIME;
    ref.rep.size_addr=(uintptr_t)&observed;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(original->references==2 && surface.resource.retained_references==1);
    drm_vmwgfx_cancel_task_continuation(&task);assert(!task.drm_vmwgfx_continuation);
    drm_vmwgfx_execbuf_process_retirements(8);
    assert(original->references==1 && !surface.resource.retained_references);
    task_lookup_scope_cancel(&task);
    /* An outer late-copy fault still rolls back the newly added file ref. */
    contended=true;assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref)==KERNEL_SYSCALL_SWAP_CONTENDED);
    contended=false;fail_copyout_address=(uintptr_t)&ref;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref)==-LINUX_EFAULT);
    fail_copyout_address=0;assert(surface.resource.logical_references==1 && original->references==1);
    task_lookup_scope_cancel(&task);
    /* Failure to allocate the continuation returns both temporary references. */
    contended=true;fail_kernel_malloc_after=1;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_VMW_REF_SURFACE,&ref)==-LINUX_ENOMEM);
    fail_kernel_malloc_after=-1;assert(!task.drm_vmwgfx_continuation && original->references==1 &&
                                      !surface.resource.retained_references);
    task_lookup_scope_cancel(&task);contended=false;
    drm_vmwgfx_all_resources=NULL;*slot=(struct task_file){0};*original=(struct task_file){0};
    /* A multi-item resource enumeration keeps membership fixed over waits. */
    struct task peer=make_task(501,false);peer.kind=TASK_KIND_USER;peer.syscall_file=&file;
    struct drm_mode_create_dumb dumb={.width=DRM_TEST_WIDTH,.height=DRM_TEST_HEIGHT,.bpp=32};
    assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_CREATE_DUMB,&dumb));task_lookup_scope_cancel(&peer);
    struct drm_kms_fb *fbs[3];
    for(unsigned i=0;i<3;++i){fbs[i]=drm_async_alloc(sizeof(*fbs[i]));assert(fbs[i]);
        *fbs[i]=(struct drm_kms_fb){.id=30-i*10,.references=1};}
    fbs[0]->next=fbs[1];fbs[1]->next=fbs[2];device.kms_fbs=fbs[0];uint32_t ids[3]={0};
    struct drm_mode_card_res resources={.fb_id_ptr=(uintptr_t)ids,.count_fbs=3};
    blocked=(uintptr_t)&ids[1];block_write=true;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETRESOURCES,&resources)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(ids[0]==30 && !ids[1] && task.drm_vmwgfx_continuation);
    struct drm_mode_fb_cmd2 add={.width=DRM_TEST_WIDTH,.height=DRM_TEST_HEIGHT,.pixel_format=DRM_FORMAT_XRGB8888_TEST,
        .handles={dumb.handle},.pitches={dumb.pitch}};uint32_t remove=20;
    assert(scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_ADDFB2,&add)==-4103);
    task_lookup_scope_cancel(&peer);
    assert(scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove)==-4103);
    task_lookup_scope_cancel(&peer);contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETRESOURCES,&resources));
    assert(ids[0]==30 && ids[1]==20 && ids[2]==10 && resources.count_fbs==3);
    assert(!task.drm_vmwgfx_continuation);task_lookup_scope_cancel(&task);
    ids[0]=ids[1]=ids[2]=0;contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETRESOURCES,&resources)==KERNEL_SYSCALL_SWAP_CONTENDED);
    drm_vmwgfx_cancel_task_continuation(&task);
    contended=false;int64_t add_result=scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_ADDFB2,&add);
    if(add_result)fprintf(stderr,"add after cancellation: %lld\n",(long long)add_result);
    assert(!add_result && add.fb_id);
    task_lookup_scope_cancel(&peer);drm_vmwgfx_execbuf_process_retirements(8);
    task_lookup_scope_cancel(&task);assert(fbs[0]->references==1 && fbs[1]->references==1 && fbs[2]->references==1);
    assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove));task_lookup_scope_cancel(&peer);
    remove=add.fb_id;assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove));task_lookup_scope_cancel(&peer);
    remove=30;assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove));task_lookup_scope_cancel(&peer);
    remove=10;assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove));task_lookup_scope_cancel(&peer);
    struct drm_mode_destroy_dumb destroy={.handle=dumb.handle};
    assert(!scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_DESTROY_DUMB,&destroy));task_lookup_scope_cancel(&peer);
    assert(!device.kms_fbs && mm_pages_allocated==mm_pages_freed);
    /* The accepted prefix may exceed 32KiB without an unbounded snapshot. */
    const unsigned many=8193;struct drm_kms_fb *large=calloc(many,sizeof(*large));
    uint32_t *large_ids=calloc(many,sizeof(*large_ids));assert(large && large_ids);
    for(unsigned i=0;i<many;++i){large[i].id=100000-i;large[i].references=1;
        large[i].next=i+1<many ? &large[i+1] : NULL;}
    device.kms_fbs=large;resources=(struct drm_mode_card_res){.fb_id_ptr=(uintptr_t)large_ids,.count_fbs=many};
    blocked=(uintptr_t)&large_ids[many-1];contended=true;
    assert(scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETRESOURCES,&resources)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(large_ids[0]==100000 && large_ids[many-2]==100000-(many-2) && !large_ids[many-1]);
    assert(large[many-1].references==2);
    assert(scoped_ioctl(&peer,&file,DRM_IOCTL_MODE_RMFB,&remove)==-4103);
    task_lookup_scope_cancel(&peer);contended=false;
    assert(!scoped_ioctl(&task,&file,DRM_IOCTL_MODE_GETRESOURCES,&resources));
    assert(large_ids[many-1]==100000-(many-1) && resources.count_fbs==many);
    task_lookup_scope_cancel(&task);assert(large[many-1].references==1);
    device.kms_fbs=NULL;free(large);free(large_ids);
    assert(!allocations);
    puts("PASS DRM ioctl: captured header, nested output wait and pre-effect outer output");
}
#endif
