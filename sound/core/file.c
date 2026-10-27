/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/audio.h>
#include <reliefnt/chrdev.h>
#include <reliefnt/sched.h>
#include <linux/errno.h>
#include "mapping.h"
#include "services.h"

/** @brief Open the actual ALSA/OSS inode selected by devfs identity.
 * @param task Borrowed current task. @param file Borrowed OFD with canonical node.
 * @return Backend result or ENODEV. Execution-owned, provider already pinned;
 * partial state is cleaned by audio_character_close before that pin is dropped.
 */
static int audio_character_open(struct task *task,struct task_file *file)
{
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_open(task,file);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_CONTROL)return audio_control_open(task,file);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_MIXER)return audio_mixer_open(task,file);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_TIMER)return audio_timer_open(task,file);
    return file->node.first_cluster==STORAGE_DEV_KIND_AUDIO ? audio_oss_open(task,file) : -ENODEV;
}
/** @brief Close only state actually owned by this sound OFD.
 * @param file Borrowed canonical OFD, including a partially failed open.
 * Execution-owned task context; code pin remains held through all callbacks.
 * PCM mappings retain their independent hardware/backing references.
 */
static void audio_character_close(struct task_file *file)
{
    if(file->audio_file)(void)audio_device_close(file);
    if(file->audio_control_file)(void)audio_control_close(file);
    if(file->audio_mixer_file)(void)audio_mixer_close(file);
    if(file->audio_timer_file)(void)audio_timer_close(file);
    if(file->audio_oss_file)(void)audio_oss_close(file);
}
/** @brief Read actual ALSA/OSS stream or control events.
 * @param task Borrowed current task. @param file Retained OFD.
 * @param buffer Caller-validated destination. @param length Requested bytes.
 * @return Bytes or backend errno. Execution-owned, no registry lock spans waits;
 * preserves the original 32-bit sound backend's bounded transfer interface.
 */
static int64_t audio_character_read(struct task *task,struct task_file *file,void *buffer,uint64_t length)
{
    uint32_t count=length>UINT32_MAX ? UINT32_MAX : (uint32_t)length;
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_read(task,file,buffer,count);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_CONTROL)
        return file->audio_control_file ? audio_control_read_file(file->audio_control_file,buffer,count) :
            audio_control_read_task(task,file,buffer,count);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_TIMER)return audio_timer_read(task,file,buffer,count);
    if(file->node.first_cluster==STORAGE_DEV_KIND_AUDIO)return audio_oss_read(task,file,buffer,count);
    return -EBADF;
}
/** @brief Write bounded audio frames through the OFD's original sound core.
 * @param task Current task. @param file Retained OFD.
 * @param buffer Validated input. @param length Requested bytes.
 * @return Backend bytes/errno; control/mixer/timer writes remain EBADF.
 * Execution ownership required, no registry lock spans waits/copying.
 */
static int64_t audio_character_write(struct task *task,struct task_file *file,const void *buffer,uint64_t length)
{
    uint32_t count=length>UINT32_MAX ? UINT32_MAX : (uint32_t)length;
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_write(task,file,buffer,count);
    if(file->node.first_cluster==STORAGE_DEV_KIND_AUDIO)return audio_oss_write(task,file,buffer,count);
    return -EBADF;
}
/** @brief Dispatch only the actual selected ALSA/OSS ABI.
 * @param task Current task. @param file Retained OFD.
 * @param request Linux ioctl selector. @param argument Backend-validated operand.
 * @return Backend result or ENOTTY. Execution-owned, no registry lock held.
 */
static int64_t audio_character_ioctl(struct task *task,struct task_file *file,uint64_t request,uint64_t argument)
{
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_ioctl(task,file,request,argument);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_CONTROL)return audio_control_ioctl(task,file,request,argument);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_MIXER)return audio_mixer_ioctl(task,file,request,argument);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_TIMER)return audio_timer_ioctl(task,file,request,argument);
    if(file->node.first_cluster==STORAGE_DEV_KIND_AUDIO)return audio_oss_ioctl(task,file,request,argument);
    return -ENOTTY;
}
/** @brief Query the sound backend's readiness, including disconnect/error bits.
 * @param file Retained OFD. @param events Requested readiness bits.
 * @return Actual backend mask; no fabricated generic POLLOUT for ALSA timer.
 * Execution ownership required; adapter takes no registry lock or reference.
 */
static short audio_character_poll(struct task_file *file,short events)
{
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_poll(file,events);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_CONTROL)return audio_control_poll_file(file->audio_control_file,events);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_MIXER)return audio_mixer_poll(file,events);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_TIMER)return audio_timer_poll(file,events);
    return audio_oss_poll(file,events);
}
/** @brief Preserve PCM/timer no-seek and current OSS selector behavior.
 * @param file Retained OFD. @param offset Requested offset.
 * @param whence Linux seek selector.
 * @return Original backend result; execution-owned, no registry lock held.
 */
static int64_t audio_character_seek(struct task_file *file,int64_t offset,unsigned whence)
{
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_PCM)return audio_device_seek(file,offset,(int)whence,NULL);
    if(file->node.flags&STORAGE_NODE_FLAG_AUDIO_TIMER)return audio_timer_seek(file,offset,(int)whence,NULL);
    if(file->node.flags&(STORAGE_NODE_FLAG_AUDIO_CONTROL|STORAGE_NODE_FLAG_AUDIO_MIXER))return -ESPIPE;
    return -EINVAL;
}
static const struct character_file_operations audio_character_operations={
    .open=audio_character_open,.close=audio_character_close,.read=audio_character_read,
    .write=audio_character_write,.ioctl=audio_character_ioctl,.poll=audio_character_poll,.seek=audio_character_seek,
    .mmap=audio_character_mmap};
static const struct character_device_registration audio_alsa_registration={
    .major=116,.count=1048576,.owner=THIS_MODULE,.operations=&audio_character_operations};
static const struct character_device_registration audio_oss_registration={
    .major=14,.count=1048576,.owner=THIS_MODULE,.operations=&audio_character_operations};
/** @brief Register the real sound core's standard character namespaces.
 * @return Zero or native registration errno with the first range rolled back.
 * Init task context; registrations do not create self module references.
 */
static int audio_character_init(void)
{
    int rc=character_device_register(&audio_alsa_registration);if(rc)return rc;
    rc=character_device_register(&audio_oss_registration);
    if(!rc) {
        rc=audio_core_services_start();
        if(rc)(void)character_device_unregister(&audio_oss_registration);
    }
    if(rc)(void)character_device_unregister(&audio_alsa_registration);
    return rc;
}
/** @brief Remove sound character admission after all OFDs have drained.
 * Task context; ledger STOP may already have removed these exact registrations.
 * No backing/page release or hardware callbacks occur in this adapter.
 */
static void audio_character_exit(void)
{
    audio_core_services_stop();
    (void)character_device_unregister(&audio_oss_registration);
    (void)character_device_unregister(&audio_alsa_registration);
}
subsys_initcall(audio_character_init);
module_exit(audio_character_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT ALSA and OSS sound core");
