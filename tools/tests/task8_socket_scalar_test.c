/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: socket scalar.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_socket_batch_main
#include "socket_batch_unix_test.c"
#undef main
#include <reliefnt/swap.h>
#include "task8_socket_output_stubs.h"
int64_t syscall_io_write(uint64_t fd,uint64_t address,uint64_t length);

static uint8_t packet[65536],received_packet[65536];
static uint64_t blocked;
static bool contended;
struct socket_send_stage;
static struct socket_send_stage *owned_stage;
struct socket_send_stage *sched_socket_stage(const struct task *task)
{ assert(task==&current);return owned_stage; }
bool sched_socket_stage_set(struct task *task,struct socket_send_stage *stage)
{ assert(task==&current && !owned_stage);owned_stage=stage;return true; }
struct socket_send_stage *sched_socket_stage_take(struct task *task)
{ assert(task==&current);struct socket_send_stage *s=owned_stage;owned_stage=NULL;return s; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && length<=32768);
    if(!writing && address==blocked && contended)return SWAP_STORAGE_CONTENDED;
    return 0;
}

struct task_pty_fd *task_pty_for_io(struct task *task, int fd)
{ (void)task; (void)fd; return NULL; }

int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=current.tgid=10;current.state=TASK_READY;
    for(unsigned i=0;i<2;++i){peers[i]=(struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_DGRAM,
        .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,.send_timeout=UINT64_MAX};
        endpoints[i]=(struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};}
    current.syscall_file=&endpoints[0];current.state=TASK_READY;
    for(size_t i=0;i<sizeof(packet);++i)packet[i]=(uint8_t)(i*19u);
    struct trap_frame frame={.rax=LINUX_SYS_WRITE,.rdi=3,.rsi=(uintptr_t)packet,.rdx=sizeof(packet)};
    blocked=(uintptr_t)packet+32768;contended=true;
    task_lookup_scope_begin(&current,&frame);
    assert(syscall_io_write(3,(uintptr_t)packet,sizeof(packet))==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count);
    memset(packet,0x55,32768);contended=false;
    task_lookup_scope_begin(&current,&frame);
    assert(syscall_io_write(3,(uintptr_t)packet,sizeof(packet))==sizeof(packet));
    struct unix_receive_result metadata;struct unix_rights *rights=NULL;
    assert(unix_read_data(&endpoints[1],received_packet,sizeof(received_packet),0,&rights,&metadata)==sizeof(packet));
    for(size_t i=0;i<sizeof(packet);++i)assert(received_packet[i]==(uint8_t)(i*19u));
    task_lookup_scope_cancel(&current);assert(!allocations && !owned_stage);
    puts("PASS UNIX datagram write: bounded staged import and retry");
    return 0;
}
