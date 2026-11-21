/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SQUASHFS_CODEC_H
#define RELIEFNT_SQUASHFS_CODEC_H
#include <reliefnt/types.h>

int sqfs_codec_init(uint16_t algorithm, uint32_t block_size, const uint8_t *options,
                    uint16_t option_size, void **state);
void sqfs_codec_destroy(uint16_t algorithm, void *state);
int sqfs_codec_decode(uint16_t algorithm, void *state, const uint8_t *, uint32_t,
                      uint8_t *, uint32_t, uint32_t *);
int sqfs_lzo_decode(const uint8_t *, uint32_t, uint8_t *, uint32_t, uint32_t *);
#endif
