/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief Link UNIX metadata pin/copy/cancel with real COW page-table cloning.
 * The host page model and explicit contended preparation are not SMP/guest.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../../kernel/syscall/ipc/socket.c"

void task8_page_model_current(struct task *task);
static struct socket_send_stage *owned;
static uint64_t blocked;
static bool contended;
struct socket_send_stage *sched_socket_stage_take(struct task *task)
{ (void)task;struct socket_send_stage *s=owned;owned=NULL;return s; }
int swap_prepare_user_range(struct task *task,uint64_t address,uint64_t length,bool writing)
{ (void)task;assert(writing && length<=4096);return contended && address==blocked ? SWAP_STORAGE_CONTENDED : 0; }
int swap_slot_get(uint64_t token) { (void)token;abort(); }
void swap_slot_put(uint64_t token) { (void)token;abort(); }
void task_pty_release_entry(struct task_pty_fd *entry) { (void)entry;abort(); }

int main(void)
{
    struct task *owner=calloc(1,sizeof(*owner)),*peer=calloc(1,sizeof(*peer));
    assert(owner && peer);owner->pid=10;owner->kind=TASK_KIND_USER;
    peer->pid=11;peer->kind=TASK_KIND_USER;peer->shared_mm=&owner->address_space;
    task8_page_model_current(owner);
    struct address_space *as=sched_task_as(owner),child={0};
    const uint64_t first=RELIEFNT_USER_BASE+0x40000,second=first+4096;
    assert(address_space_create(as));
    uint64_t a=mm_alloc_page(),b=mm_alloc_page();assert(a && b);
    memset(mm_phys_to_virt(a),0x27,4096);memset(mm_phys_to_virt(b),0x39,4096);
    assert(address_space_map_user_page(as,first,a,RELIEFNT_PAGE_WRITABLE));
    assert(address_space_map_user_page(as,second,b,RELIEFNT_PAGE_WRITABLE));
    owned=kernel_malloc(sizeof(*owned));assert(owned);*owned=(struct socket_send_stage){0};
    owned->regions[0]=(struct socket_output_region){.address=first,.length=8};
    owned->regions[1]=(struct socket_output_region){.address=second,.length=8};
    assert(!socket_pin_output(owner,owned,0) && mm_page_refcount(a)==2);
    blocked=second;contended=true;
    assert(socket_pin_output(owner,owned,1)==KERNEL_SYSCALL_SWAP_CONTENDED);
    task8_page_model_current(peer);assert(address_space_clone_cow(sched_task_as(peer),&child));
    task8_page_model_current(owner);contended=false;
    assert(!socket_pin_output(owner,owned,1));
    uint8_t data[8];memset(data,0x65,sizeof(data));socket_output_copy(owned,0,0,data,sizeof(data));
    uint64_t child_a=address_space_user_page_phys(&child,first);
    for(unsigned i=0;i<8;++i) {
        assert(((uint8_t *)mm_phys_to_virt(a))[i]==0x65);
        assert(((uint8_t *)mm_phys_to_virt(child_a))[i]==0x27);
    }
    task_socket_stage_cancel(owner);assert(!owned && mm_page_refcount(a)==1);
    address_space_destroy(&child);address_space_destroy(as);free(peer);free(owner);
    puts("PASS UNIX metadata pins preserve fork snapshot and cancel ownership");
    return 0;
}
