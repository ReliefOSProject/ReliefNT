/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/service.h>
#include "../../kernel/module/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio_fixture.h"
static size_t allocated;
static long fail_after=-1;
void *kernel_malloc(size_t bytes)
{if(!fail_after)return NULL;if(fail_after>0)--fail_after;void *p=calloc(1,bytes);if(p)++allocated;return p;}
void kernel_free(void *p){if(p){assert(allocated);--allocated;free(p);}}
void module_memory_free(struct module_memory *p){assert(!p);}
static struct module owner;
static struct kernel_service_slot slot;
static struct kernel_service_descriptor descriptor;
static unsigned called;
static int callback(void)
{
    ++called;
    /* No service registry lock spans provider code. */
    assert(kernel_service_unregister(&slot,&descriptor)==-16);
    return 42;
}
struct operations {int (*call)(void);};
static const struct operations operations={callback};
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(owner.name,"service_provider");strcpy(runtime->name,owner.name);
    owner.runtime=runtime;runtime->owner=&owner;runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    descriptor=(struct kernel_service_descriptor){.operations=&operations,.owner=&owner};
    struct kernel_service_call call={0},other={0};
    assert(!kernel_service_acquire(&slot,&call) && !call.operations);
    assert(!kernel_service_register(&slot,&descriptor));
    assert(kernel_service_register(&slot,&descriptor)==-17);
    assert(!kernel_service_acquire(&slot,&call) && !runtime->executing);
    runtime->state=MODULE_LIVE;
    assert(kernel_service_acquire(&slot,&call) && runtime->executing==1);
    assert(((const struct operations *)call.operations)->call()==42 && called==1);
    assert(runtime->users==1);
    assert(module_unload(owner.name,0)==-11 && runtime->state==MODULE_LIVE);
    assert(kernel_service_acquire(&slot,&other) && runtime->users==2);
    kernel_service_release(&other);assert(runtime->users==1);
    /* Incomplete teardown must leave ordinary callbacks LIVE, so a periodic
     * IRQ cannot randomly strand an otherwise idle core in GOING. */
    runtime->state=MODULE_GOING;
    assert(!kernel_service_acquire(&slot,&other) && !other.operations);
    runtime->state=MODULE_LIVE;
    assert(((const struct operations *)call.operations)->call()==42 && called==2);
    kernel_service_release(&call);assert(!call.operations && !runtime->executing && !runtime->users);
    runtime->executing=UINT64_MAX;
    assert(!kernel_service_acquire(&slot,&call) && !call.operations && !runtime->users);
    runtime->executing=0;
    kernel_service_release(&call);
    assert(!module_unload(owner.name,0) && !owner.runtime && !module_list && !allocated);
    assert(!kernel_service_acquire(&slot,&call));
    assert(kernel_service_unregister(&slot,&descriptor)==-2);
    for(int failure=0;failure<2;++failure) {
        runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
        strcpy(owner.name,"failed_service");strcpy(runtime->name,owner.name);
        owner.runtime=runtime;runtime->owner=&owner;runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
        fail_after=failure;assert(kernel_service_register(&slot,&descriptor)==-12);fail_after=-1;
        runtime->state=MODULE_LIVE;assert(!kernel_service_acquire(&slot,&call));
        assert(!module_unload(owner.name,0) && !allocated);
    }
    descriptor.owner=NULL;
    assert(!kernel_service_register(&slot,&descriptor));
    assert(kernel_service_acquire(&slot,&call));
    assert(kernel_service_unregister(&slot,&descriptor)==-16);
    kernel_service_release(&call);
    assert(!kernel_service_unregister(&slot,&descriptor) && !allocated);
    assert(!kernel_service_register(&slot,&descriptor));
    assert(!kernel_service_unregister(&slot,&descriptor) && !allocated);
    assert(kernel_service_register(NULL,&descriptor)==-22);
    struct kernel_service_descriptor invalid={0};assert(kernel_service_register(&slot,&invalid)==-22);
    puts("PASS actual optional-service gate: COMING/LIVE/GOING, callback reentry, executing-code teardown, OOM rollback, missing providers and builtin detach/reload");
}
