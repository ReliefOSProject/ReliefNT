/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <reliefnt/usb.h>
#include "../../kernel/module/internal.h"

static struct module owner;
static struct usb_host *host;
static size_t allocations;
static int fail_allocation;
static unsigned calls, releases;
static atomic_bool entered, unblock, stopping, finished;
void *kernel_malloc(size_t bytes)
{ void *p=fail_allocation ? NULL : calloc(1,bytes);if(p)++allocations;return p; }
void kernel_free(void *p)
{ if(p){assert(allocations);--allocations;free(p);} }
void module_memory_free(struct module_memory *memory) { assert(!memory); }
void kernel_spin_lock(struct kernel_spinlock *lock)
{ while(__atomic_exchange_n(&lock->state,1,__ATOMIC_ACQUIRE)){} }
void kernel_spin_unlock(struct kernel_spinlock *lock)
{ __atomic_store_n(&lock->state,0,__ATOMIC_RELEASE); }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{ *flags=0;kernel_spin_lock(lock); }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{ (void)flags;kernel_spin_unlock(lock); }
static void counted_poll(void *context) { ++*(unsigned *)context; }
static void waiting_poll(void *context)
{
    assert(context==&calls);++calls;atomic_store(&entered,true);
    while(!atomic_load(&unblock)){}
}
static void *poll_thread(void *unused) { (void)unused;usb_poll();return NULL; }
static int quiesce(void *context)
{
    assert(context==&host);atomic_store(&stopping,true);
    int rc=usb_host_unregister(host);if(!rc)host=NULL;return rc;
}
static void release(void *context) { assert(context==&host && !host);++releases; }
static void *unload_thread(void *unused)
{ (void)unused;assert(!module_unload("test_usb",0));atomic_store(&finished,true);return NULL; }
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(runtime->name,"test_usb");strcpy(owner.name,"test_usb");
    runtime->owner=&owner;owner.runtime=runtime;module_list=runtime;
    runtime->state=MODULE_COMING;runtime->init_succeeded=true;
    assert(!usb_host_register(counted_poll,&calls,&owner,&host));
    assert(!module_add_resource(&owner,quiesce,release,&host));
    usb_poll();assert(!calls && !runtime->users && !runtime->executing);
    runtime->state=MODULE_LIVE;usb_poll();assert(calls==1 && !runtime->users && !runtime->executing);
    assert(!usb_host_unregister(host));host=NULL;
    assert(!usb_host_register(waiting_poll,&calls,&owner,&host));
    pthread_t reader,unloader;assert(!pthread_create(&reader,NULL,poll_thread,NULL));
    while(!atomic_load(&entered)){}
    assert(!pthread_create(&unloader,NULL,unload_thread,NULL));
    while(!atomic_load(&stopping)){}
    assert(!atomic_load(&finished) && !module_get_by_name("test_usb"));
    unsigned saved=calls;usb_poll();assert(calls==saved);
    atomic_store(&unblock,true);
    assert(!pthread_join(reader,NULL) && !pthread_join(unloader,NULL));
    assert(!host && releases==1 && !owner.runtime && !module_list && !allocations);
    struct usb_host *builtins[40];unsigned counts[40]={0};
    for(unsigned i=0;i<40;++i)assert(!usb_host_register(counted_poll,&counts[i],NULL,&builtins[i]));
    struct usb_host *duplicate=NULL;
    assert(usb_host_register(counted_poll,&counts[0],NULL,&duplicate)==-EEXIST && !duplicate);
    fail_allocation=1;
    assert(usb_host_register(waiting_poll,&counts[0],NULL,&duplicate)==-ENOMEM && !duplicate);
    fail_allocation=0;
    for(unsigned n=0;n<12;++n)usb_poll();
    for(unsigned i=0;i<40;++i)assert(counts[i] && !usb_host_unregister(builtins[i]));
    saved=calls;usb_poll();assert(calls==saved && !allocations);
    for(unsigned i=0;i<40;++i) {
        assert(!usb_host_register_service(counted_poll,counted_poll,&counts[i],NULL,&builtins[i]));
        usb_host_request_service(builtins[i]);
        usb_host_request_service(builtins[i]);
    }
    assert(usb_service_pending());
    unsigned before[40];memcpy(before,counts,sizeof(before));
    for(unsigned i=0;i<12;++i)usb_service_run();
    for(unsigned i=0;i<40;++i)assert(counts[i]==before[i]+1);
    assert(!usb_service_pending());
    for(unsigned i=0;i<40;++i)assert(!usb_host_unregister(builtins[i]));
    puts("PASS USB deferred work coalescing, fair bounded batches and idle queue clearing");
    puts("PASS USB host lifetime: COMING closed, no self-pin, callback drain before actual unload, dynamic fair polling, builtin and allocation rollback");
}
