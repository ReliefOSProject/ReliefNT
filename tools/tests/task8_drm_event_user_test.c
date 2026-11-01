/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: drm event user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <reliefnt/paging.h>
#include <reliefnt/sched.h>
#include <reliefnt/syscall_internal.h>
#include "../../drivers/video/drm/event_io.c"
#include "../../drivers/video/drm/drm_file.c"
static unsigned range_checks,blocks;
bool user_range_writable(uint64_t p,uint64_t n){(void)p;(void)n;++range_checks;return false;}
int file_can_read(const struct task_file *f){return !(f->flags&RELIEFOS_O_WRONLY);}
int kernel_wait_queue_add(struct kernel_wait_queue *q,struct task *t){(void)q;(void)t;return 0;}
void sched_block_current(void){++blocks;}
void sched_sleep_current_until(uint64_t ticks){(void)ticks;assert(0);}
uint64_t time_ticks(void){return 1;}
int main(void)
{
    struct task task={.kind=TASK_KIND_USER};struct drm_file drm={0};
    struct task_file file={.drm_file=&drm,.flags=RELIEFOS_O_NONBLOCK};
    uint64_t absent=RELIEFNT_USER_BASE+4096;
    assert(drm_event_read(&task,&file,absent,65536)==-RELIEFOS_EAGAIN);
    assert(!range_checks);
    struct iovec vec[2]={{(void *)(uintptr_t)absent,4096},
                         {(void *)(uintptr_t)(absent+8192),4096}};
    assert(drm_event_readv(&task,&file,vec,2,false,0,0)==-RELIEFOS_EAGAIN);
    assert(!range_checks);
    assert(drm_event_read(&task,&file,RELIEFNT_USER_TOP-1,2)==-RELIEFOS_EFAULT);
    vec[1].iov_base=(void *)(uintptr_t)(RELIEFNT_USER_TOP-1);vec[1].iov_len=2;
    assert(drm_event_readv(&task,&file,vec,2,false,0,0)==-RELIEFOS_EFAULT);
    assert(drm_event_readv(&task,&file,vec,2,true,UINT64_MAX,0)==-RELIEFOS_EINVAL);
    file.flags=0;assert(drm_event_read(&task,&file,absent,8)==KERNEL_SYSCALL_BLOCKED && blocks==1);
    puts("PASS DRM empty events: aperture without payload access, scalar/vector readiness");
}
