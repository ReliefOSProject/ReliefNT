#define AUDIO_FIXTURE_CUSTOM_HEAP 1
/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../kernel/module/internal.h"
#include <reliefnt/heap.h>
#include "audio_fixture.h"
#include "audio_fake_card.h"
#include "../../sound/core/core.c"
#include "modules/pcm.h"
#include "../../sound/core/alsa_control.c"
#include "../../sound/core/mixer.c"

static struct module provider;
static size_t allocations;
static unsigned resources_released;
void *kernel_malloc(size_t size) { void *p=calloc(1,size); if(p)++allocations; return p; }
void kernel_free(void *memory) { if(memory){assert(allocations);--allocations;free(memory);} }
void module_memory_free(struct module_memory *memory) { assert(!memory); }
/* The fixture supplies the hardware provider lookup; reference accounting and
 * unload are the production module runtime, not a mock busy result. */
struct module *driver_module_owner(uint32_t owner) { assert(owner==0x7a); return &provider; }
void audio_timer_period_elapsed(uint32_t card,uint32_t device,enum audio_direction direction,uint64_t frames,int error)
{ (void)card;(void)device;(void)direction;(void)frames;(void)error; }
static int stop_card(void *context) { return audio_unregister_card(((struct audio_test_card *)context)->id,0); }
static void free_card(void *context) { assert(context); ++resources_released; }
int main(void)
{
    struct module_runtime *runtime=kernel_malloc(sizeof(*runtime)); assert(runtime);
    strcpy(runtime->name,"audio_provider"); strcpy(provider.name,"audio_provider");
    runtime->owner=&provider; provider.runtime=runtime;
    runtime->state=MODULE_COMING; runtime->init_succeeded=true; module_list=runtime;
    struct audio_test_card card; audio_test_card_init(&card);
    assert(!module_add_resource(&provider,stop_card,free_card,&card));
    uint32_t stream;
    assert(audio_stream_open(card.id,0,AUDIO_PLAYBACK,&stream)==-ENODEV);
    assert(!runtime->users);
    runtime->state=MODULE_LIVE;
    struct audio_control_file *control=NULL;
    struct audio_mixer_file *mixer=NULL;
    assert(!audio_control_open_card(card.id,&control));
    assert(runtime->users && module_unload("audio_provider",0)==-EWOULDBLOCK);
    assert(!audio_mixer_open_card(card.id,&mixer));
    assert(!audio_control_close_file(control));
    assert(runtime->users && module_unload("audio_provider",0)==-EWOULDBLOCK);
    int error=0; struct audio_pcm *pcm=audio_pcm_open(card.id,0,AUDIO_PLAYBACK,&error);
    assert(pcm && !error);
    assert(!audio_mixer_close_file(mixer));
    struct audio_pcm_mmap mapping;
    assert(!audio_pcm_mmap_acquire(pcm,AUDIO_PCM_MMAP_OFFSET_STATUS_OLD,4096,AUDIO_PCM_PROT_READ,&mapping));
    assert(!audio_pcm_mmap_retain(pcm,mapping.region));
    audio_pcm_release(pcm);
    assert(card.close_count==1 && runtime->users);
    assert(module_unload("audio_provider",0)==-EWOULDBLOCK);
    audio_pcm_mmap_release(pcm,mapping.region);
    assert(runtime->users && module_unload("audio_provider",0)==-EWOULDBLOCK);
    audio_pcm_mmap_release(pcm,mapping.region);
    assert(!runtime->users);
    assert(!module_unload("audio_provider",0));
    assert(!allocations && !provider.runtime && !module_list && resources_released==1);
    puts("PASS module audio ownership: COMING closed, control/mixer/PCM opens busy, close-before-munmap, forked VMA refs, final unload");
}
