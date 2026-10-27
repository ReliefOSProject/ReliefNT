/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/module/internal.h"
#include <reliefnt/heap.h>
#include <reliefnt/driver_module.h>
#include "audio_fixture.h"
#include "../../kernel/device/registry.c"
#include "../../kernel/device/services.c"
#include "../../kernel/device/driver_manager_phase.c"
static struct module provider, replacement;
static size_t allocations;
static unsigned exits;
void *kernel_malloc(size_t size) { void *p=calloc(1,size); if(p)++allocations; return p; }
void kernel_free(void *memory) { if(memory){assert(allocations);--allocations;free(memory);} }
void module_memory_free(struct module_memory *memory) { assert(!memory); }
void console_log(enum console_log_level level,const char *format,...) { (void)level;(void)format; }
__attribute__((noreturn)) void panic(const char *message) { fprintf(stderr,"panic: %s\n",message);abort(); }
int sound_service_unregister_owner(uint32_t owner,uint32_t force) { (void)owner;(void)force;return 0; }
void driver_resources_release(uint32_t owner,bool irq_only) { (void)owner;(void)irq_only; }
void net_driver_detached(void) { }
static int ready(void) { return 1; }
static int configure_audio(const struct reliefos_audio_format *format) { (void)format;return 0; }
static long write_audio(const void *bytes,uint32_t length,uint32_t *status) { (void)bytes;*status=0;return length; }
static void state_audio(struct reliefos_audio_state *state)
{ *state=(struct reliefos_audio_state){.present=1,.active=1,.sample_rate=48000,.channels=2,.bits_per_sample=16}; }
static const struct reliefos_driver_audio_ops ops={ready,configure_audio,write_audio,state_audio};
static const struct reliefos_driver_kernel_api api={.abi_version=RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size=sizeof(api),.register_audio=driver_register_audio};
const struct reliefos_driver_kernel_api *driver_kernel_services(void) { return &api; }
static int start(const struct reliefos_driver_kernel_api *services) { return services->register_audio(&ops); }
static void stop(void) { ++exits; }
static const struct reliefos_driver_module descriptor={.magic=RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version=RELIEFOS_DRIVER_ABI_VERSION,.struct_size=sizeof(descriptor),
    .kind=RELIEFOS_DRIVER_KIND_AUDIO,.name="legacy_audio",.init=start,.fini=stop};
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime)); assert(runtime);
    strcpy(runtime->name,"legacy_audio"); strcpy(provider.name,"legacy_audio");
    runtime->owner=&provider; provider.runtime=runtime; module_list=runtime;
    runtime->state=MODULE_COMING; runtime->init_succeeded=true;
    uint64_t flags;kernel_execution_lock_irqsave(&flags);
    driver_manager_init();assert(!driver_module_register(&descriptor,&provider));
    uint32_t generation=0;struct reliefos_audio_state state;
    assert(driver_manager_audio_state_bound(0,&state)==-ENODEV);
    assert(driver_manager_audio_acquire(&generation,&state)==-ENODEV);
    assert(!runtime->users);
    runtime->state=MODULE_LIVE;
    assert(driver_manager_service_try_pin(0) && runtime->executing==1);
    driver_manager_service_unpin(0);assert(!runtime->executing);
    assert(!driver_manager_audio_acquire(&generation,&state));
    assert(runtime->users && module_unload("legacy_audio",0)==-EWOULDBLOCK && !exits);
    uint32_t duplicate=0;
    assert(driver_manager_audio_acquire(&duplicate,&state)==-EBUSY);
    driver_manager_audio_release(generation+100);
    assert(module_unload("legacy_audio",0)==-EWOULDBLOCK);
    struct module_runtime *new_runtime=kernel_malloc(sizeof(*new_runtime)); assert(new_runtime);
    strcpy(new_runtime->name,"replacement_audio"); strcpy(replacement.name,"replacement_audio");
    new_runtime->owner=&replacement; replacement.runtime=new_runtime;
    new_runtime->state=MODULE_COMING; new_runtime->init_succeeded=true;
    new_runtime->next=module_list; module_list=new_runtime;
    struct reliefos_driver_module new_descriptor=descriptor;
    strcpy(new_descriptor.name,"replacement_audio");
    assert(!driver_module_register(&new_descriptor,&replacement));
    new_runtime->state=MODULE_LIVE;
    uint32_t newer=0; assert(!driver_manager_audio_acquire(&newer,&state) && newer!=generation);
    assert(runtime->users && new_runtime->users);
    driver_manager_audio_release(generation);
    assert(!runtime->users && !module_unload("legacy_audio",0));
    assert(module_unload("replacement_audio",0)==-EWOULDBLOCK && audio_ops);
    driver_manager_audio_release(newer);
    assert(!module_unload("replacement_audio",0));
    assert(exits==2 && !allocations && !module_list && !provider.runtime && !replacement.runtime && !audio_ops);
    kernel_execution_unlock_irqrestore(flags);
    puts("PASS legacy OSS module owner: COMING closed, lease prevents actual unload, duplicate/stale generations, final teardown");
}
