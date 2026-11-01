/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Task8 host fixture: epoll user.
 * Controlled substitutes model only the boundaries asserted by this fixture.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/syscall/event/epoll.c"
#include <reliefnt/swap.h>
static struct task current;
static struct epoll_event output[2];
static uint64_t swapped, invalid;
static bool contended;
static unsigned preparations;
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{
    assert(task==&current && length==sizeof(struct epoll_event) && writing);++preparations;
    if(address==swapped){if(contended)return SWAP_STORAGE_CONTENDED;swapped=0;}
    return 0;
}
bool user_range_writable(uint64_t address,uint64_t length)
{
    uint64_t start=(uintptr_t)output;
    return address>=start && address-start<=sizeof(output) && length<=sizeof(output)-(address-start) &&
           address!=swapped && address!=invalid;
}
uint64_t kernel_poll_readiness_generation(void) { return 0; }
uint64_t task_socket_event_generation(const struct task_file *file) { (void)file;return 0; }
uint64_t time_ticks(void) { return 100; }
int64_t syscall_poll_impl(struct task *task,struct pollfd *p,uint32_t count,int64_t timeout,uint64_t generation)
{ assert(task==&current && count==1 && !timeout);(void)generation;p->revents=POLLIN;return 1; }
int64_t syscall_poll_park(struct task *task,int64_t timeout,uint64_t generation,bool wait)
{ (void)task;(void)timeout;(void)generation;(void)wait;assert(0);return 0; }
int main(void)
{
    current.kind=TASK_KIND_USER;
    struct task_epoll_entry items[2]={
        {.fd=4,.events=EPOLLIN|EPOLLET|EPOLLONESHOT,.data=41,.active=1},
        {.fd=5,.events=EPOLLIN|EPOLLET|EPOLLONESHOT,.data=42,.active=1}};
    struct task_epoll epoll={.entries=2,.capacity=2,.item=items};
    /* Only two records are mapped, although the valid address range is large. */
    swapped=(uintptr_t)&output[0];contended=true;
    assert(syscall_epoll_wait_common(&current,&epoll,(uintptr_t)output,1000000,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!items[0].ready && !items[0].disabled && !items[1].ready && !output[0].events);
    swapped=(uintptr_t)&output[1];
    assert(syscall_epoll_wait_common(&current,&epoll,(uintptr_t)output,1000000,0)==1);
    assert(output[0].data==41 && items[0].disabled && !items[1].ready && !items[1].disabled);
    assert(syscall_epoll_wait_common(&current,&epoll,(uintptr_t)&output[1],1000000,0)==KERNEL_SYSCALL_SWAP_CONTENDED);
    assert(!items[1].ready && !items[1].disabled);
    contended=false;
    assert(syscall_epoll_wait_common(&current,&epoll,(uintptr_t)&output[1],1000000,0)==1);
    assert(output[1].data==42 && items[1].disabled && preparations);
    items[0].ready=items[0].disabled=items[1].ready=items[1].disabled=0;
    invalid=(uintptr_t)&output[1];memset(output,0,sizeof(output));
    assert(syscall_epoll_wait_common(&current,&epoll,(uintptr_t)output,1000000,0)==1);
    assert(items[0].disabled && !items[1].disabled && !items[1].ready && output[0].data==41);
    puts("PASS epoll: bounded output, first contention, edge/oneshot and partial copy");
}
