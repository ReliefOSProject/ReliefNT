/* Count native data submissions while exercising the production staging path. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"
#include "../../fs/ext_common/ops.c"

static uint8_t media[96 * EXT4_MAX_BLOCK_SIZE];
static uint8_t input[65 * EXT4_MAX_BLOCK_SIZE];
static uint64_t submitted_lba[65];
static uint32_t submitted_sectors[65];
static unsigned commands, finished, fail_command, large_fail, all_fail;
static size_t allocation, peak_allocation;
static void *staging;
static struct storage_volume volume;

void *kernel_malloc(size_t bytes)
{
    if (all_fail || (large_fail && bytes > volume.ext4.block_size)) return NULL;
    assert(!staging);
    staging = malloc(bytes);
    if (staging) {
        allocation = bytes;
        if (bytes > peak_allocation) peak_allocation = bytes;
    }
    return staging;
}

void kernel_free(void *pointer)
{
    assert(pointer == staging);
    free(pointer);
    staging = NULL;
    allocation = 0;
}

void storage_memcpy(void *out, const void *in, size_t bytes)
{ memcpy(out, in, bytes); }

int storage_write_device(const struct storage_volume *v, uint64_t lba,
                         uint32_t sectors, const void *data)
{
    assert(v == &volume && data == staging);
    assert(sectors && (size_t)sectors * 512 <= allocation);
    assert(lba >= volume.ext_start_lba && lba * 512 + sectors * 512 <= sizeof(media));
    assert(commands < sizeof(submitted_lba) / sizeof(submitted_lba[0]));
    submitted_lba[commands] = lba;
    submitted_sectors[commands++] = sectors;
    if (fail_command == commands) {
        /* A failed device command may already have modified part of its extent. */
        memcpy(media + lba * 512, data, 512);
        return -RELIEFOS_EIO;
    }
    memcpy(media + lba * 512, data, sectors * 512);
    return 0;
}

void storage_ext4_cache_finish(struct storage_volume *v, uint64_t block, const uint8_t *data)
{
    assert(v == &volume);
    uint32_t bs = v->ext4.block_size;
    assert(!memcmp(data, media + v->ext_start_lba * 512 + block * bs, bs));
    ++finished;
}

static void reset(uint32_t block_size)
{
    assert(!staging);
    volume = (struct storage_volume){.ext_start_lba = 8};
    volume.ext4.block_size = block_size;
    commands = finished = fail_command = large_fail = all_fail = 0;
    peak_allocation = 0;
    memset(media, 0xa5, sizeof(media));
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (i * 37 + i / 257) % 251;
}

int main(int argc, char **argv)
{
    bool baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    for (uint32_t bs = 1024; bs <= 4096; bs *= 2) {
        reset(bs);
        assert(!ext4_ops_data_write(&volume, 3, 32, input));
        assert(finished == 32 && !staging);
        assert(!memcmp(media + 8 * 512 + 3 * bs, input, 32 * bs));
        printf("{\"block_size\":%u,\"payload_bytes\":%u,\"device_commands\":%u}\n",
               bs, 32 * bs, commands);
        if (baseline) continue;
        assert(commands == 1);
        assert(submitted_lba[0] == 8 + 3 * (bs / 512));
        assert(submitted_sectors[0] == 32 * (bs / 512));

        /* Every command and allocation remains bounded for a larger extent. */
        reset(bs);
        assert(!ext4_ops_data_write(&volume, 3, 65, input));
        assert(commands == 3 && finished == 65 && !staging);
        assert(peak_allocation <= EXT4_READAHEAD_BLOCKS * bs);
        assert(!memcmp(media + 8 * 512 + 3 * bs, input, 65 * bs));
        assert(submitted_sectors[2] == bs / 512);

        /* Low memory preserves the original one-block staging capability. */
        reset(bs); large_fail = 1;
        assert(!ext4_ops_data_write(&volume, 3, 7, input));
        assert(commands == 7 && finished == 7 && peak_allocation == bs && !staging);
        assert(!memcmp(media + 8 * 512 + 3 * bs, input, 7 * bs));
        reset(bs); all_fail = 1;
        assert(ext4_ops_data_write(&volume, 3, 7, input) == -RELIEFOS_ENOMEM);
        assert(!commands && !finished && !staging);

        /* Failed batches cannot publish clean cached bytes or advance further. */
        reset(bs); fail_command = 1;
        assert(ext4_ops_data_write(&volume, 3, 65, input) == -RELIEFOS_EIO);
        assert(commands == 1 && !finished && !staging);
        reset(bs); fail_command = 2;
        assert(ext4_ops_data_write(&volume, 3, 65, input) == -RELIEFOS_EIO);
        assert(commands == 2 && finished == 32 && !staging);
        assert(!memcmp(media + 8 * 512 + 3 * bs, input, 32 * bs));

        reset(bs);
        assert(!ext4_ops_data_write(&volume, 3, 1, input));
        assert(commands == 1 && finished == 1 && peak_allocation == bs && !staging);
    }
    return 0;
}
