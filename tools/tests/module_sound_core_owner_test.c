/* SPDX-License-Identifier: Apache-2.0 */
#define AUDIO_FIXTURE_CUSTOM_HEAP 1
#include "../../kernel/module/internal.h"
#include <reliefnt/audio.h>
#include <reliefnt/heap.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "audio_fixture.h"
#include "audio_fake_card.h"
static struct module sound_core;
/* Compile the real card registry with a genuine optional sound-core owner. */
#undef THIS_MODULE
#define THIS_MODULE (&sound_core)
#include "../../sound/core/core.c"
#undef THIS_MODULE
#define THIS_MODULE ((struct module *)0)
static unsigned allocations;
void *kernel_malloc(size_t n){void *p=calloc(1,n);if(p)++allocations;return p;}
void kernel_free(void *p){if(p){assert(allocations);--allocations;free(p);}}
void module_memory_free(struct module_memory *p){assert(!p);}
struct module *driver_module_owner(uint32_t owner){assert(owner==0x7a);return NULL;}
void audio_timer_period_elapsed(uint32_t card,uint32_t device,enum audio_direction direction,uint64_t frames,int error)
{(void)card;(void)device;(void)direction;(void)frames;(void)error;}
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime));assert(runtime);
    strcpy(sound_core.name,"snd");strcpy(runtime->name,sound_core.name);
    sound_core.runtime=runtime;runtime->owner=&sound_core;runtime->state=MODULE_COMING;runtime->init_succeeded=true;module_list=runtime;
    struct audio_card_identity identity={.id="lease",.name="core owner"};
    struct audio_test_card hardware={0};uint32_t id=42;
    assert(audio_register_card_owned(&identity,&audio_test_ops,&hardware,0x7a,&id)==-ENODEV && id==42);
    assert(!runtime->users);
    runtime->state=MODULE_LIVE;
    assert(!audio_register_card_owned(&identity,&audio_test_ops,&hardware,0x7a,&id));
    assert(runtime->users==1 && module_unload(sound_core.name,0)==-EWOULDBLOCK);
    assert(!audio_request_controller_disconnect(id) && audio_disconnect_work_pending());
    assert(runtime->users==1 && module_unload(sound_core.name,0)==-EWOULDBLOCK);
    assert(audio_process_pending_disconnects(1)==1 && !runtime->users && !audio_disconnect_work_pending());
    assert(audio_unregister_card(id,1)==-ENODEV && !runtime->users);
    uint32_t tokens[16];
    for(unsigned i=0;i<16;++i)assert(!audio_register_card_owned(&identity,&audio_test_ops,&hardware,0x7a,&tokens[i]));
    assert(runtime->users==16);
    assert(audio_register_card_owned(&identity,&audio_test_ops,&hardware,0x7a,&id)==-ENOSPC && runtime->users==16);
    assert(!audio_unregister_owner(0x7a,1) && !runtime->users);
    assert(!module_unload(sound_core.name,0) && !module_list && !sound_core.runtime && !allocations);
    puts("PASS actual sound-core owner: COMING refuses registration, live cards and queued disconnect block unload, exhaustion balances pins, final owner disconnect permits unload");
}
