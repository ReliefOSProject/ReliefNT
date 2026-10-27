/* SPDX-License-Identifier: Apache-2.0 */
#include "mapping.h"
#include <reliefnt/heap.h>
#include <reliefnt/sched.h>
#include <linux/errno.h>
struct audio_mapping_context {struct audio_pcm_mmap backing;};
/** @brief Drop the sharing family's one PCM backing lease after all VMAs retire.
 * @param value Owned sound mapping context with a valid PCM reference.
 * Task context under execution ownership; PCM handles STOP/backing retirement
 * and the generic mapping keeps sound-core code pinned across this callback.
 */
static void audio_mapping_release(void *value)
{
    struct audio_mapping_context *context=value;
    audio_pcm_mmap_release(context->backing.pcm,context->backing.region);
}
/** @brief Consume the context only after the final backing release completes.
 * @param value Owned context, consumed once; its PCM may already be freed.
 * Execution-owned task context without registry locks; no PCM dereference.
 */
static void audio_mapping_destroy(void *value)
{kernel_free(value);}
/** @brief Translate a page of explicitly owned coherent PCM backing.
 * @param value Retained context. @param offset In-bounds page offset.
 * @return Actual physical page, zero on overflow; no locks/references changed.
 */
static uint64_t audio_mapping_physical(void *value,uint64_t offset)
{
    const struct audio_mapping_context *context=value;
    return offset>UINT64_MAX-context->backing.backing ? 0 : context->backing.backing+offset;
}
/** @brief Adopt one PCM lease without exposing PCM types to the core VM layer.
 * @param backing Caller-owned acquired mapping, transferred only on success.
 * @param out Required empty owned output.
 * @return Zero or negative errno; execution-owned task context, no registry
 * locks. A failed allocation/provider pin leaves PCM ownership with caller.
 */
int audio_mapping_adopt(const struct audio_pcm_mmap *backing,struct device_mapping **out)
{
    if(!out)return -EINVAL;*out=NULL;
    if(!backing || !backing->pcm || !backing->length)return -EINVAL;
    struct audio_mapping_context *context=kernel_malloc(sizeof(*context));if(!context)return -ENOMEM;
    context->backing=*backing;
    static const struct device_mapping_operations operations={
        .release=audio_mapping_release,.destroy=audio_mapping_destroy,.physical=audio_mapping_physical};
    struct device_mapping_info info={.length=backing->length,
        .max_prot=backing->region==AUDIO_PCM_MMAP_STATUS ? 1u : 3u,.can_split=true};
    int rc=device_mapping_create(THIS_MODULE,&operations,context,&info,out);
    if(rc)kernel_free(context);return rc;
}
/** @brief Build a provider-owned mapping for a genuine sound description.
 * @param task Current task. @param file Retained OFD.
 * @param offset PCM selector. @param length Rounded bytes.
 * @param prot Requested protection. @param flags Existing mmap flags.
 * @param out Required empty owned output.
 * @return Zero or backend errno; execution-owned task context. Preserves the
 * existing sound flag policy; allocation/adoption failures release PCM once.
 */
int audio_character_mmap(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                           uint32_t prot,uint64_t flags,struct device_mapping **out)
{
    (void)flags;
    if(!out)return -EINVAL;*out=NULL;
    if(file->node.first_cluster!=STORAGE_DEV_KIND_AUDIO)return -EINVAL;
    struct audio_pcm_mmap backing={0};int rc=audio_device_mmap(task,file,offset,length,prot,&backing);
    if(!rc){rc=audio_mapping_adopt(&backing,out);if(rc)audio_pcm_mmap_release(backing.pcm,backing.region);}
    return rc;
}
