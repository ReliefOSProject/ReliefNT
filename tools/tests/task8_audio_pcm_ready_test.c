/* SPDX-License-Identifier: Apache-2.0 */
/** @file
 * @brief PCM user payload preparation follows real provider readiness.
 * The actual PCM/device core is linked; the swap boundary is modeled.
 */
#define TASK8_PCM_READY_TEST 1
#include "task8_audio_pcm_user_test.c"

static struct audio_test_card *race_card;
static struct audio_pcm *race_pcm;
/** @brief Model an IRQ reporting XRUN after preparation and before transfer. */
static void race_report_xrun(void)
{
    assert(race_card && race_pcm && !race_pcm->lock.state);
    audio_test_advance(race_card,0,-EPIPE);
}

int main(void)
{
    struct audio_test_card card;
    audio_test_card_init(&card);
    struct task task = {.pid=711,.kind=TASK_KIND_USER,.state=TASK_RUNNING};
    struct task *entries[]={&task};
    tasks=entries;task_count=1;current_pid[0]=task.pid;
    struct task_file play=audio_test_file(card.id,0,AUDIO_PLAYBACK,
        RELIEFOS_O_WRONLY|RELIEFOS_O_NONBLOCK);
    assert(!audio_device_open(&task,&play));audio_test_configure_file(&play);
    uint8_t data[8192]={0};
    assert(audio_device_write(&task,&play,data,sizeof(data))==(int)sizeof(data));
    blocked=(uintptr_t)data;unsigned before=prepares;
    assert(audio_device_write(&task,&play,data,sizeof(data))==-EAGAIN);
    assert(prepares==before);
    audio_test_advance(&card,128,0);blocked=0;unmapped_tail=(uintptr_t)data+512;before=prepares;
    assert(audio_device_write(&task,&play,data,sizeof(data))==512);
    assert(prepares==before+1);
    audio_test_advance(&card,128,0);unmapped_tail=0;blocked=(uintptr_t)data+512;before=prepares;
    assert(audio_device_write(&task,&play,data,sizeof(data))==512);
    assert(prepares==before+1);
    audio_test_advance(&card,128,0);blocked=0;
    struct audio_pcm_status before_race,after_race;
    assert(!audio_pcm_status(play.audio_file->pcm,&before_race));
    race_card=&card;race_pcm=play.audio_file->pcm;prepare_hook=race_report_xrun;
    assert(audio_device_write(&task,&play,data,sizeof(data))==-EPIPE);
    assert(!prepare_hook && !audio_pcm_status(play.audio_file->pcm,&after_race));
    assert(after_race.appl_ptr==before_race.appl_ptr);
    blocked=(uintptr_t)data;before=prepares;
    assert(audio_device_write(&task,&play,data,sizeof(data))==-EPIPE);
    assert(prepares==before);
    assert(!audio_device_close(&play));assert(!audio_unregister_card(card.id,0));

    audio_test_card_init(&card);
    struct task_file capture=audio_test_file(card.id,0,AUDIO_CAPTURE,
        RELIEFOS_O_RDONLY|RELIEFOS_O_NONBLOCK);
    assert(!audio_device_open(&task,&capture));audio_test_configure_file(&capture);
    blocked=(uintptr_t)data;before=prepares;
    assert(audio_device_read(&task,&capture,data,sizeof(data))==-EAGAIN);
    assert(prepares==before);
    audio_test_advance(&card,128,0);blocked=0;unmapped_tail=(uintptr_t)data+512;before=prepares;
    assert(audio_device_read(&task,&capture,data,sizeof(data))==512);
    assert(prepares==before+1);
    audio_test_advance(&card,128,0);unmapped_tail=0;blocked=(uintptr_t)data+512;before=prepares;
    assert(audio_device_read(&task,&capture,data,sizeof(data))==512);
    assert(prepares==before+1);
    assert(!audio_unregister_card(card.id,1));blocked=(uintptr_t)data;before=prepares;
    assert(audio_device_read(&task,&capture,data,sizeof(data))==-ENODEV);
    assert(prepares==before);assert(!audio_device_close(&capture));
    puts("PASS PCM readiness precedes bounded payload preparation");return 0;
}
