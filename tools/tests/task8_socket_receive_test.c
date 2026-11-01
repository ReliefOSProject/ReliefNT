/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: socket receive.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#define main inherited_socket_batch_main
#include "socket_batch_unix_test.c"
#undef main
#include <reliefnt/swap.h>
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>

static uint8_t incoming[128],output[128];
static unsigned char control[CMSG_SPACE(sizeof(int))] __attribute__((aligned(4096)));
static unsigned char replacement[4096] __attribute__((aligned(4096)));
static bool replace_control;
static uintptr_t device_user_page,device_physical_page;
static unsigned pins;
static bool contended,blocked_writing=true;
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
    if(writing==blocked_writing && address==readonly) {
        if(contended)return SWAP_STORAGE_CONTENDED;
        readonly=0;
    }
    return 0;
}
uint64_t address_space_user_page_phys(const struct address_space *as,uint64_t address)
{
    (void)as;
    if(device_user_page && (address&~4095ULL)==device_user_page)return device_physical_page;
    if(replace_control && (address&~4095ULL)==((uintptr_t)control&~4095ULL))return (uintptr_t)replacement;
    return address&~4095ULL;
}
uint64_t address_space_user_page_flags(const struct address_space *as,uint64_t address)
{ (void)as;return device_user_page && (address&~4095ULL)==device_user_page ?
    RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_USER|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_DEVICE : 0; }
void *mm_phys_to_virt(uint64_t address) { return (void *)(uintptr_t)address; }
uint32_t mm_page_refcount(uint64_t address) { assert(address);return 1; }
void mm_retain_page(uint64_t address) { assert(address);++pins; }
void mm_free_page(uint64_t address) { assert(address && pins);--pins; }
int page_cache_retain(uint64_t address) { assert(address);return -LINUX_ENOENT; }
void page_cache_release(uint64_t address) { (void)address;assert(0); }
void page_cache_mark_shared_writable(uint64_t address) { (void)address;assert(0); }
#ifdef TASK8_TEST_ACTUAL_FRONTS
int file_can_read(const struct task_file *file) { return file!=NULL; }
int file_can_write(const struct task_file *file) { return file!=NULL; }
int64_t syscall_io_read(uint64_t fd,uint64_t address,uint64_t count);
int64_t syscall_vector_io(struct task *task,uint64_t number,uint64_t fd,uint64_t vectors,uint64_t count,
                          uint64_t position,uint64_t high,uint64_t flags);
#endif

static void send_one(uint8_t value)
{
    memset(incoming,value,sizeof(incoming));
    assert(unix_send_packet(&endpoints[0],&peers[1],incoming,sizeof(incoming),0,NULL)==sizeof(incoming));
    struct unix_rights *r=kernel_malloc(sizeof(*r));assert(r);*r=(struct unix_rights){.position=peers[1].rx_written-1,.end=peers[1].rx_written,.count=1};
    r->files[0]=task_file_get(&passed);++passed.scm_references;
    assert(!peers[1].rights);peers[1].rights=r;
}

#ifdef TASK8_DEVICE_SOCKET_TEST
int inherited_task8_socket_receive_main(void)
#else
int main(void)
#endif
{
    current.kind=TASK_KIND_USER;current.pid=current.tgid=10;current.uid=current.gid=100;current.state=TASK_READY;
    for(unsigned i=0;i<2;++i){peers[i]=(struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_DGRAM,
        .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,.send_timeout=UINT64_MAX};
        endpoints[i]=(struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};}
    passed=(struct task_file){.used=1,.references=1,.aux=1,
        .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX};send_one(0x37);
    struct iovec vector={output,sizeof(output)};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=control,.msg_controllen=sizeof(control)};
    struct trap_frame frame={.rax=LINUX_SYS_RECVMSG,.rdi=4,.rsi=(uintptr_t)&message,.rdx=MSG_PEEK};
    struct socket_message_result result={0};current.syscall_file=&endpoints[1];
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)output;contended=true;
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_PEEK,true,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(peers[1].packet_count==1 && passed.references==3 && passed.scm_references==1);
    struct unix_receive_result metadata;struct unix_rights *original=NULL;
    assert(unix_read_data(&endpoints[1],incoming,sizeof(incoming),0,&original,&metadata)==sizeof(incoming));
    assert(original);unix_rights_free(original);
    assert(!peers[1].packet_count && passed.references==2);
    /* A stage is an external root even if another copy is in an orphaned
     * self-referential SCM queue. Exercise the production graph collector. */
    struct unix_rights *orphan=kernel_malloc(sizeof(*orphan));assert(orphan);
    *orphan=(struct unix_rights){.count=1};orphan->files[0]=task_file_get(&passed);++passed.scm_references;
    peers[0].rights=orphan;peers[0].registry_next=&peers[1];unix_sockets=&peers[0];
    task_file_put(&passed);assert(passed.references==2 && passed.scm_references==1);
    task_socket_collect();assert(peers[0].rights==orphan && passed.references==2);
    peers[0].rights=NULL;unix_rights_free(orphan);unix_sockets=NULL;
    assert(passed.references==1 && !passed.scm_references);
    contended=false;task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_PEEK,true,false,&result)==sizeof(output));
    for(size_t i=0;i<sizeof(output);++i)assert(output[i]==0x37);
    assert(!owned_stage && current.files[0].description==&passed && passed.references==1);
    clear_task_file(&current.files[0]);task_lookup_scope_cancel(&current);
    assert(!passed.references && !allocations);
    passed.references=1;
    struct unix_rights pair={.count=2,.files={&passed,&passed}},*cloned=NULL;
    assert(!unix_peek_rights(&pair,&cloned) && passed.references==3 && !passed.scm_references);
    unix_rights_free(cloned);assert(passed.references==1);

    send_one(0x43);memset(output,0,sizeof(output));memset(control,0,sizeof(control));
    vector.iov_len=64;message=(struct msghdr){.msg_iov=&vector,.msg_iovlen=1,
        .msg_control=control,.msg_controllen=sizeof(control)};
    frame.rdx=MSG_TRUNC;current.syscall_file=&endpoints[1];
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)&message.msg_flags;contended=true;
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_TRUNC,true,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && passed.references==2 && !passed.scm_references && output[0]==0x43 && pins);
    memset(incoming,0x71,sizeof(incoming));
    assert(unix_send_packet(&endpoints[0],&peers[1],incoming,sizeof(incoming),0,NULL)==sizeof(incoming));
    output[0]=0x99;replace_control=true;contended=false;task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_TRUNC,true,false,&result)==128);
    assert(output[0]==0x99 && output[1]==0x43 && (message.msg_flags & MSG_TRUNC));
    assert(peers[1].packet_count==1 && current.files[0].description==&passed && passed.references==2 && !pins);
    assert(((struct cmsghdr *)(void *)control)->cmsg_type==SCM_RIGHTS && !replacement[0]);replace_control=false;
    clear_task_file(&current.files[0]);task_lookup_scope_cancel(&current);
    assert(unix_read_data(&endpoints[1],incoming,sizeof(incoming),0,NULL,&metadata)==sizeof(incoming));
    assert(incoming[0]==0x71 && passed.references==1 && !allocations);

    __builtin_memcpy(peers[0].path,"abc",3);peers[0].path_len=3;
    assert(unix_send_packet(&endpoints[0],&peers[1],"xyz",3,0,NULL)==3);
    char name[111]={0};uint32_t name_length=sizeof(name);memset(output,0,sizeof(output));
    frame=(struct trap_frame){.rax=LINUX_SYS_RECVFROM,.rdi=4,.rsi=(uintptr_t)output,.rdx=sizeof(output),
        .r8=(uintptr_t)name,.r9=(uintptr_t)&name_length};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)name;contended=true;
    assert(syscall_socket_dispatch(LINUX_SYS_RECVFROM,4,(uintptr_t)output,sizeof(output),0,(uintptr_t)name,(uintptr_t)&name_length)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && !memcmp(output,"xyz",3));
    name_length=1;contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_socket_dispatch(LINUX_SYS_RECVFROM,4,(uintptr_t)output,sizeof(output),0,(uintptr_t)name,(uintptr_t)&name_length)==3);
    assert(name_length==5 && name[0]==AF_UNIX && !memcmp(name+2,"abc",3));
    task_lookup_scope_cancel(&current);assert(!allocations && !pins);

    char a[]="one",b[]="two",x[4]={0},y[4]={0};
    struct iovec sendv[]={{a,3},{b,3}},recvv[]={{x,3},{y,3}};
    current.syscall_file=&endpoints[0];frame=(struct trap_frame){.rax=LINUX_SYS_WRITEV,.rdi=3,.rsi=(uintptr_t)sendv,.rdx=2};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)b;blocked_writing=false;contended=true;
    assert(task_socket_vector_user(&current,&endpoints[0],(uintptr_t)sendv,2,true)==KERNEL_SYSCALL_SWAP_CONTENDED);
    memset(a,'z',3);contended=false;task_lookup_scope_begin(&current,&frame);
    assert(task_socket_vector_user(&current,&endpoints[0],(uintptr_t)sendv,2,true)==6);
    task_lookup_scope_cancel(&current);
    current.syscall_file=&endpoints[1];frame=(struct trap_frame){.rax=LINUX_SYS_READV,.rdi=4,.rsi=(uintptr_t)recvv,.rdx=2};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)y;blocked_writing=true;contended=true;
    assert(task_socket_vector_user(&current,&endpoints[1],(uintptr_t)recvv,2,false)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!memcmp(x,"one",3) && !peers[1].packet_count);
    x[0]='Q';contended=false;task_lookup_scope_begin(&current,&frame);
    assert(task_socket_vector_user(&current,&endpoints[1],(uintptr_t)recvv,2,false)==6);
    assert(!memcmp(x,"Qne",3) && !memcmp(y,"two",3));
    task_lookup_scope_cancel(&current);assert(!allocations && !pins);

    struct iovec batch_vector={.iov_base=a,.iov_len=3};
    struct mmsghdr batch_message={.msg_hdr={.msg_iov=&batch_vector,.msg_iovlen=1}};
    current.syscall_file=&endpoints[0];frame=(struct trap_frame){.rax=LINUX_SYS_SENDMMSG,.rdi=3,
        .rsi=(uintptr_t)&batch_message,.rdx=1};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)&batch_message.msg_len;contended=true;
    assert(syscall_socket_mmsg(false,3,(uintptr_t)&batch_message,1,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(peers[1].packet_count==1 && task_socket_batch_accepted(&current));
    assert(unix_read_data(&endpoints[1],incoming,sizeof(incoming),0,NULL,&metadata)==3);
    contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_socket_mmsg(false,3,(uintptr_t)&batch_message,1,0,0)==1);
    assert(batch_message.msg_len==3 && !peers[1].packet_count);
    task_lookup_scope_cancel(&current);assert(!allocations && !pins);

    assert(unix_send_packet(&endpoints[0],&peers[1],"old",3,0,NULL)==3);
    batch_vector=(struct iovec){.iov_base=output,.iov_len=sizeof(output)};
    batch_message=(struct mmsghdr){.msg_hdr={.msg_iov=&batch_vector,.msg_iovlen=1}};
    struct linux_timespec timeout={2,0};current.syscall_file=&endpoints[1];
    frame=(struct trap_frame){.rax=LINUX_SYS_RECVMMSG,.rdi=4,.rsi=(uintptr_t)&batch_message,.rdx=1,.r8=(uintptr_t)&timeout};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)&timeout;contended=true;
    assert(syscall_socket_mmsg(true,4,(uintptr_t)&batch_message,1,0,(uintptr_t)&timeout)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && batch_message.msg_len==3 && task_socket_batch_accepted(&current));
    assert(unix_send_packet(&endpoints[0],&peers[1],"new",3,0,NULL)==3);
    contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_socket_mmsg(true,4,(uintptr_t)&batch_message,1,0,(uintptr_t)&timeout)==1);
    assert(!memcmp(output,"old",3) && peers[1].packet_count==1);
    task_lookup_scope_cancel(&current);
    assert(unix_read_data(&endpoints[1],incoming,sizeof(incoming),0,NULL,&metadata)==3 && !memcmp(incoming,"new",3));
    assert(!allocations && !pins);

    peers[0].type=peers[1].type=SOCK_STREAM;current.state=TASK_READY;
    assert(unix_write_data(&endpoints[0],"abc",3,NULL)==3);
    char all[6]={0};struct iovec all_vector={all,sizeof(all)};
    message=(struct msghdr){.msg_iov=&all_vector,.msg_iovlen=1};
    current.syscall_file=&endpoints[1];frame=(struct trap_frame){.rax=LINUX_SYS_RECVMSG,.rdi=4,
        .rsi=(uintptr_t)&message,.rdx=MSG_WAITALL};
    task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_WAITALL,true,false,&result)==KERNEL_SYSCALL_BLOCKED);
    assert(current.socket_receive_done==3 && owned_stage && !task_socket_stage_receive_accepted(&current));
    assert(unix_write_data(&endpoints[0],"def",3,NULL)==3);task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_WAITALL,true,false,&result)==6);
    assert(!memcmp(all,"abcdef",6) && !owned_stage);task_lookup_scope_cancel(&current);

    assert(unix_write_data(&endpoints[0],"abc",3,NULL)==3);memset(all,0,sizeof(all));
    task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,MSG_WAITALL,true,false,&result)==KERNEL_SYSCALL_BLOCKED);
    readonly=(uintptr_t)&message.msg_flags;contended=true;
    assert(task_socket_signal_prepare(&current)==SWAP_STORAGE_CONTENDED && current.socket_receive_done==3);
    contended=false;assert(!task_socket_signal_prepare(&current) && !task_socket_signal_finish(&current));
    assert(task_socket_cancel_receive(&current)==3);task_socket_stage_cancel(&current);task_lookup_scope_cancel(&current);
    assert(!memcmp(all,"abc",3) && !owned_stage && !allocations && !pins);
    peers[0].type=peers[1].type=SOCK_DGRAM;current.state=TASK_READY;
#ifdef TASK8_TEST_ACTUAL_FRONTS
    assert(unix_send_packet(&endpoints[0],&peers[1],"read",4,0,NULL)==4);
    frame=(struct trap_frame){.rax=LINUX_SYS_READ,.rdi=4,.rsi=(uintptr_t)output,.rdx=sizeof(output)};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)output;contended=true;
    assert(syscall_io_read(4,(uintptr_t)output,sizeof(output))==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count);
    contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_io_read(4,(uintptr_t)output,sizeof(output))==4 && !memcmp(output,"read",4));
    task_lookup_scope_cancel(&current);

    memcpy(a,"one",3);memcpy(b,"two",3);current.syscall_file=&endpoints[0];
    frame=(struct trap_frame){.rax=LINUX_SYS_WRITEV,.rdi=3,.rsi=(uintptr_t)sendv,.rdx=2};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)b;blocked_writing=false;contended=true;
    assert(syscall_vector_io(&current,LINUX_SYS_WRITEV,3,(uintptr_t)sendv,2,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    memset(a,'x',3);contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_vector_io(&current,LINUX_SYS_WRITEV,3,(uintptr_t)sendv,2,0,0,0)==6);
    task_lookup_scope_cancel(&current);
    current.syscall_file=&endpoints[1];frame=(struct trap_frame){.rax=LINUX_SYS_READV,.rdi=4,.rsi=(uintptr_t)recvv,.rdx=2};
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)y;blocked_writing=true;contended=true;
    assert(syscall_vector_io(&current,LINUX_SYS_READV,4,(uintptr_t)recvv,2,0,0,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    x[0]='R';contended=false;task_lookup_scope_begin(&current,&frame);
    assert(syscall_vector_io(&current,LINUX_SYS_READV,4,(uintptr_t)recvv,2,0,0,0)==6);
    assert(!memcmp(x,"Rne",3) && !memcmp(y,"two",3));
    task_lookup_scope_cancel(&current);assert(!allocations && !pins);
#endif
    puts("PASS UNIX receive, recvfrom and vector staging: PEEK/TRUNC, collector, replacement and retry");
    return 0;
}
