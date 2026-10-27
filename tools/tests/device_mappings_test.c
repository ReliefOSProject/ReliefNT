/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/device_mapping.h>
#include "../../kernel/module/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio_fixture.h"
static unsigned allocations,references,destroyed;
static bool fail_heap;
void *kernel_malloc(size_t bytes){if(fail_heap)return NULL;void *memory=calloc(1,bytes);if(memory)++allocations;return memory;}
void kernel_free(void *memory){if(memory){assert(allocations);--allocations;free(memory);}}
void module_memory_free(struct module_memory *memory){assert(!memory);}
static void release(void *context){assert(context==&references && references);--references;}
static void destroy(void *context){assert(context==&references && !references);++destroyed;}
static uint64_t physical(void *context,uint64_t offset){assert(context==&references && references);return 0x24000000u+offset;}
static const struct device_mapping_operations operations={.release=release,.destroy=destroy,.physical=physical};
int main(void)
{
    struct module owner={0};struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(owner.name,"mapped_provider");strcpy(runtime->name,owner.name);owner.runtime=runtime;runtime->owner=&owner;
    runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    struct device_mapping *mapping=NULL;references=1;
    struct device_mapping_info info={.length=8192,.max_prot=3,.can_split=true};
    assert(device_mapping_create(&owner,&operations,&references,&info,&mapping)==-19 && !mapping && references==1 && !runtime->users);
    runtime->state=MODULE_LIVE;fail_heap=true;
    assert(device_mapping_create(&owner,&operations,&references,&info,&mapping)==-12 && !mapping && references==1 && !runtime->users);fail_heap=false;
    assert(!device_mapping_create(&owner,&operations,&references,&info,&mapping) && runtime->users==1);
    assert(device_mapping_info(mapping)->length==8192 && device_mapping_info(mapping)->can_split);
    assert(device_mapping_physical(mapping,0)==0x24000000u && device_mapping_physical(mapping,4096)==0x24001000u);
    assert(!device_mapping_physical(mapping,1) && !device_mapping_physical(mapping,8192));
    assert(!device_mapping_get(mapping) && !device_mapping_get(mapping) && references==1 && runtime->users==1);
    assert(module_unload(owner.name,0)==-11);
    device_mapping_put(mapping);device_mapping_put(mapping);assert(references==1 && !destroyed && runtime->users==1);
    assert(module_unload(owner.name,0)==-11);
    device_mapping_put(mapping);assert(!references && destroyed==1 && !runtime->users);
    assert(!module_unload(owner.name,0) && !allocations && !module_list);
    references=1;info.length=4097;
    assert(device_mapping_create(NULL,&operations,&references,&info,&mapping)==-22 && !mapping && references==1);
    info.length=4096;info.max_prot=8;
    assert(device_mapping_create(NULL,&operations,&references,&info,&mapping)==-22 && !mapping);
    info.max_prot=1;assert(!device_mapping_create(NULL,&operations,&references,&info,&mapping));
    device_mapping_put(mapping);assert(!references && destroyed==2 && !allocations);
    device_mapping_put(NULL);
    puts("PASS generic device VMA: COMING admission, independent module pin, callback-free fork/split sharing, exact physical bounds, final backend release/destroy and OOM rollback");
}
