/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/sound_service.h>
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
static int card_index(uint32_t id)
{assert(owner.runtime->executing==1 && id==700);++called;assert(sound_service_unregister(&provider)==-16);return 4;}
static int card_snapshot(uint32_t index,uint32_t *id,struct audio_card_identity *identity)
{assert(owner.runtime->executing==1 && index==4);*id=700;strcpy(identity->name,"owned sound");return 0;}
static void tick(void){assert(owner.runtime->executing==1);++called;}
int main(void)
{
    uint32_t id=123;struct audio_card_identity identity={0};
    assert(sound_service_card_index(700)==-2);
    assert(sound_service_card_snapshot(0,&id,&identity)==-2 && id==123);
    assert(sound_service_unregister_owner(1,1)==0);
    assert(sound_service_unregister_card(1,1)==-19);
    assert(sound_service_control_node("controlC0",NULL)==-2);
    assert(sound_service_pcm_node("pcmC0D0p",NULL)==-2);
    assert(sound_service_timer_node("timer",NULL)==-2);
    struct reliefos_dir_entry entry;
    uint64_t cursor=0;assert(!sound_service_snd_readdir(&cursor,&entry) && !cursor);
    sound_service_tick();sound_service_process_faults();assert(!called);
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(owner.name,"sound_provider");strcpy(runtime->name,owner.name);
    owner.runtime=runtime;runtime->owner=&owner;runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    static const struct sound_service_operations operations={.card_index=card_index,.card_snapshot=card_snapshot,.tick=tick};
    provider=(struct kernel_service_descriptor){.operations=&operations,.owner=&owner};
    assert(!sound_service_register(&provider));
    assert(sound_service_card_index(700)==-2 && !called);
    runtime->state=MODULE_LIVE;
    assert(sound_service_card_index(700)==4 && called==1 && !runtime->executing);
    assert(!sound_service_card_snapshot(4,&id,&identity) && id==700 && !strcmp(identity.name,"owned sound"));
    sound_service_tick();assert(called==2 && !runtime->executing);
    assert(sound_service_pcm_node("pcmC0D0p",NULL)==-2);
    assert(!module_unload(owner.name,0) && !allocated && !module_list);
    assert(sound_service_card_index(700)==-2);
    puts("PASS sound service broker: absent/partial callbacks, actual LIVE code pins and exact-table dispatch, reentrant unregistration and module-ledger teardown");
}
