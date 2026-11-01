/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm close.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define TASK8_DRM_NO_MAIN 1
#define TASK8_DRM_CLEANUP_TEST 1
#include "task8_drm_ioctl_user_test.c"
void task_regular_io_stage_cancel(struct task *t){assert(t && !t->regular_io.active);}
void task_socket_stage_cancel(struct task *t){assert(t);}
void task_transfer_cancel(struct task *t){assert(t && !t->transfer_io);}
void task_vector_cancel(struct task *t){assert(t && !t->vector_io);}
void task_sysv_msg_cancel(struct task *t){assert(t && !t->sysv_msg.queue);}
void task_sysv_sem_cancel(struct task *t){assert(t && !t->sysv_sem.ops);}
void task_fifo_cancel(struct task *t){assert(t && !t->fifo_open_file);}
void task_pty_release_entry(struct task_pty_fd *entry){assert(entry && !entry->used);}
void task_epoll_remove_file_description(struct task_file *f){assert(f && f->drm_file && !f->drm_file->kms_enumeration);}
void syscall_flock_release_owner(struct task_file *f){assert(f);}
void block_device_put(struct block_device_ref *ref){assert(ref && !ref->device);}
void vfs_file_close(struct vfs_open_file *file){assert(!file);}
void task_pipe_release(struct task_file *f){assert(f && !(f->flags&TASK_FILE_FLAG_PIPE));}
void task_socket_release(struct task_file *f){assert(f && !(f->flags&TASK_FILE_FLAG_SOCKET));}
void task_inet_release(struct task_file *f){assert(f && !(f->flags&TASK_FILE_FLAG_SOCKET_INET));}
void task_shm_release(struct task_file *f){assert(f && !(f->flags&TASK_FILE_FLAG_DEV_SHM));}
int main(void)
{
    assert(!drm_character_init());struct pci_device pci=vmware_device();assert(!drm_device_register(&pci));
    struct task task=make_task(702,false);task.kind=TASK_KIND_USER;task.state=TASK_READY;
    struct task_file *file=kernel_malloc(sizeof(*file));assert(file);*file=(struct task_file){.used=1,.references=1};
    assert(!character_file_open(226u<<8,&task,file,&file->character_file));
    task.syscall_file=file;task.syscall_fd=3;task.syscall_file_number=LINUX_SYS_IOCTL;
    struct drm_kms_fb *first=drm_async_alloc(sizeof(*first)),*second=drm_async_alloc(sizeof(*second));
    assert(first && second);*second=(struct drm_kms_fb){.references=1,.id=20};
    *first=(struct drm_kms_fb){.references=1,.id=30,.next=second};file->drm_file->kms_fbs=first;
    uint32_t ids[2]={0};struct drm_mode_card_res resources={.fb_id_ptr=(uintptr_t)ids,.count_fbs=2};
    blocked=(uintptr_t)&ids[1];block_write=true;contended=true;
    assert(scoped_ioctl(&task,file,DRM_IOCTL_MODE_GETRESOURCES,&resources)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(ids[0]==30 && file->drm_file->kms_enumeration && second->references==2);
    task_release_syscall_file(&task);
    assert(!task.syscall_file && !task.drm_vmwgfx_continuation);
    assert(second->references==1); /* retained by queued cursor after final close */
    drm_vmwgfx_execbuf_process_retirements(8);drm_character_exit();assert(!allocations);
    puts("PASS real descriptor->graphics cancel->final close: reservation detached before OFD put");
}
