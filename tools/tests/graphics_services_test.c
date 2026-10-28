/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/graphics_service.h>
#include "../../kernel/module/internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "audio_fixture.h"
static unsigned allocated,called;
void *kernel_malloc(size_t n){void *p=calloc(1,n);if(p)++allocated;return p;}
void kernel_free(void *p){if(p){assert(allocated);--allocated;free(p);}}
void module_memory_free(struct module_memory *p){assert(!p);}
static struct module owner;
static struct kernel_service_descriptor provider;
static bool registered(void)
{
    assert(owner.runtime->users==1 && owner.runtime->executing==1);++called;
    assert(graphics_service_unregister(&provider)==-16);
    assert(module_unload(owner.name,0)==-11 && owner.runtime->state==MODULE_LIVE);
    return true;
}
static int identity(struct pci_device *out)
{assert(owner.runtime->users==1 && owner.runtime->executing==1);out->vendor_id=0x15ad;return 0;}
static uint32_t retire(uint32_t budget)
{assert(owner.runtime->users==1 && owner.runtime->executing==1);return budget+1;}
int main(void)
{
    struct pci_device pci={.vendor_id=42};
    assert(!graphics_service_is_registered() && !graphics_service_render_node_ready());
    assert(!graphics_service_kms_scanout_active() && !graphics_service_task_signal_busy(NULL));
    assert(!graphics_service_task_uninterruptible(NULL));
    assert(graphics_service_pci_identity(&pci)==-19 && pci.vendor_id==42);
    assert(!graphics_service_process_retirements(2));
    graphics_service_kms_vt_release();graphics_service_cancel_task_continuation(NULL);
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(owner.name,"graphics_provider");strcpy(runtime->name,owner.name);
    owner.runtime=runtime;runtime->owner=&owner;runtime->state=MODULE_COMING;
    runtime->init_succeeded=true;module_list=runtime;
    static const struct graphics_service_operations operations={.is_registered=registered,.pci_identity=identity,.process_retirements=retire};
    provider=(struct kernel_service_descriptor){.operations=&operations,.owner=&owner};
    assert(!graphics_service_register(&provider));
    assert(!graphics_service_is_registered() && !called);
    runtime->state=MODULE_LIVE;
    assert(graphics_service_is_registered() && called==1 && !runtime->users && !runtime->executing);
    assert(!graphics_service_pci_identity(&pci) && pci.vendor_id==0x15ad);
    assert(graphics_service_process_retirements(2)==3);
    assert(!graphics_service_task_uninterruptible(NULL));
    assert(!module_unload(owner.name,0) && !allocated && !module_list);
    assert(!graphics_service_is_registered() && graphics_service_pci_identity(&pci)==-19);
    puts("PASS graphics service broker: absent/partial callbacks, exact LIVE owner pins, reentrant busy unload and ledger teardown");
}
