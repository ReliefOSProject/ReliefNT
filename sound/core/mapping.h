/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SOUND_MAPPING_H
#define RELIEFNT_SOUND_MAPPING_H
#include <reliefnt/audio.h>
#include <reliefnt/device_mapping.h>
/** @brief Transfer one acquired PCM backing lease to a generic VMA object.
 * @param backing Valid acquired PCM mapping, caller-owned through failure.
 * @param out Required empty output, owns the backing only on success.
 * @return Zero or negative errno. Execution-owned task context; preserves the
 * caller's PCM reference on failure, final generic release consumes it once.
 */
int audio_mapping_adopt(const struct audio_pcm_mmap *backing,struct device_mapping **out);
/** @brief Construct the actual PCM mapping behind the character-file interface.
 * @param task Current task. @param file Retained sound OFD.
 * @param offset Linux PCM mapping selector. @param length Rounded bytes.
 * @param prot Requested protection. @param flags Existing Linux mmap flags.
 * @param out Required empty owned output.
 * @return Zero or backend errno with full rollback. Execution ownership
 * required; no registry lock spans PCM acquisition or allocation.
 */
int audio_character_mmap(struct task *task,struct task_file *file,uint64_t offset,uint64_t length,
                           uint32_t prot,uint64_t flags,struct device_mapping **out);
#endif
