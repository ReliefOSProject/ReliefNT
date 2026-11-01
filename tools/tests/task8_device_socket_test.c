/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Retain an actual device mapping during UNIX ancillary completion.
 * User pages and the direct alias are host models; socket stage, SCM rights,
 * device_mapping references and common cancellation are production code.
 */
#define _GNU_SOURCE
#define TASK8_DEVICE_SOCKET_TEST 1
#include "task8_socket_receive_test.c"
#include <reliefnt/device_mapping.h>
#include <sys/mman.h>
#include <unistd.h>

static void *device_alias;
static unsigned device_releases;
static void release_device(void *context) { assert(context==device_alias);++device_releases; }
static uint64_t resolve_device(void *context,uint64_t offset)
{ assert(context==device_alias && !offset);return (uintptr_t)device_alias; }
static const struct device_mapping_operations ops={.release=release_device,.physical=resolve_device};
uint32_t sched_task_vma_capacity(const struct task *task) { (void)task;return SCHED_TASK_VMA_MAX; }
struct task_vma *sched_task_vma_at(struct task *task,uint32_t index)
{ assert(index<SCHED_TASK_VMA_MAX);return &sched_task_mm(task)->vmas[index]; }
bool paging_kernel_direct_cache_flags(uint64_t physical,uint64_t *flags)
{ assert(physical==(uintptr_t)device_alias);*flags=0;return true; }
void *paging_kernel_direct_map(uint64_t physical)
{ assert(physical==(uintptr_t)device_alias);return device_alias; }
void *paging_ioremap_device_page(uint64_t physical,uint64_t flags)
{ (void)physical;(void)flags;abort(); }
bool paging_iounmap(void *address,uint64_t bytes) { (void)address;(void)bytes;abort(); }

int main(void)
{
    current.kind=TASK_KIND_USER;current.pid=current.tgid=10;current.uid=current.gid=100;current.state=TASK_READY;
    for(unsigned i=0;i<2;++i){peers[i]=(struct unix_socket){.used=1,.refs=1,.handle=i+1,.type=SOCK_DGRAM,
        .state=UNIX_SOCKET_CONNECTED,.peer_handle=2-i,.receive_timeout=UINT64_MAX,.send_timeout=UINT64_MAX};
        endpoints[i]=(struct task_file){.used=1,.references=1,.aux=i+1,
            .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX|RELIEFOS_O_RDWR};}
    passed=(struct task_file){.used=1,.references=1,.aux=1,
        .flags=TASK_FILE_FLAG_SOCKET|TASK_FILE_FLAG_SOCKET_UNIX};send_one(0x37);
    int fd=memfd_create("task8-device-socket",0);assert(fd>=0 && !ftruncate(fd,4096));
    void *user=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    device_alias=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    assert(user!=MAP_FAILED && device_alias!=MAP_FAILED);
    device_user_page=(uintptr_t)user;device_physical_page=(uintptr_t)device_alias;
    struct device_mapping_info info={.length=4096,.max_prot=3};struct device_mapping *mapping=NULL;
    assert(!device_mapping_create(NULL,&ops,device_alias,&info,&mapping));
    sched_task_mm(&current)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)user,
        .end=(uintptr_t)user+4096,.flags=TASK_VMA_FLAG_DEVICE,.prot=3,.max_prot=3,.device_mapping=mapping};
    struct iovec vector={output,sizeof(output)};
    struct msghdr message={.msg_iov=&vector,.msg_iovlen=1,.msg_control=user,.msg_controllen=CMSG_SPACE(sizeof(int))};
    struct trap_frame frame={.rax=LINUX_SYS_RECVMSG,.rdi=4,.rsi=(uintptr_t)&message};
    struct socket_message_result result={0};current.syscall_file=&endpoints[1];
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)&message.msg_flags;contended=true;
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,0,true,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && owned_stage && !device_releases);
    assert(mmap(user,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==user);
    sched_task_mm(&current)->vmas[0]=(struct task_vma){0};device_user_page=0;device_mapping_put(mapping);
    assert(!device_releases);contended=false;task_lookup_scope_begin(&current,&frame);
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,0,true,false,&result)==sizeof(output));
    assert(device_releases==1 && !owned_stage && !((unsigned char *)user)[0]);
    assert(((struct cmsghdr *)device_alias)->cmsg_type==SCM_RIGHTS);
    assert(current.files[0].description==&passed);clear_task_file(&current.files[0]);
    task_lookup_scope_cancel(&current);assert(passed.references==1 && !allocations && !pins);

    /* Cancellation after packet ownership and DEVICE metadata pin drops the
     * borrowed backing and SCM rights exactly once, including double cancel. */
    assert(mmap(user,4096,PROT_READ|PROT_WRITE,MAP_SHARED|MAP_FIXED,fd,0)==user);
    device_user_page=(uintptr_t)user;device_physical_page=(uintptr_t)device_alias;
    assert(!device_mapping_create(NULL,&ops,device_alias,&info,&mapping));
    sched_task_mm(&current)->vmas[0]=(struct task_vma){.used=1,.start=(uintptr_t)user,
        .end=(uintptr_t)user+4096,.flags=TASK_VMA_FLAG_DEVICE,.prot=3,.max_prot=3,.device_mapping=mapping};
    send_one(0x49);message.msg_control=user;message.msg_controllen=CMSG_SPACE(sizeof(int));
    task_lookup_scope_begin(&current,&frame);readonly=(uintptr_t)&message.msg_flags;contended=true;
    assert(task_socket_message(&current,&endpoints[1],(uintptr_t)&message,0,true,false,&result)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!peers[1].packet_count && owned_stage && device_releases==1);
    sched_task_mm(&current)->vmas[0]=(struct task_vma){0};device_user_page=0;device_mapping_put(mapping);
    assert(device_releases==1);task_socket_stage_cancel(&current);task_socket_stage_cancel(&current);
    task_lookup_scope_cancel(&current);assert(device_releases==2 && !owned_stage && passed.references==1 && !allocations && !pins);
    munmap(user,4096);munmap(device_alias,4096);close(fd);
    puts("PASS UNIX ancillary device ownership across VMA replacement");return 0;
}
