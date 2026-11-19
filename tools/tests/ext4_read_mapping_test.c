/* Exercise the real byte-range reader with independently described mappings.
 * Cache seams deliberately replace their returned pointer on every call. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"

static struct storage_volume volume;
static uint8_t media[256 * 4096], scratch[4096];
static struct { uint32_t first, count, physical; bool unwritten; } runs[8];
static unsigned nr_runs, maps, cached_reads, commands, fail_map, fail_command;
static int fail_error;
static uint32_t map_at[32];

void storage_memcpy(void *out, const void *in, size_t n) { memcpy(out, in, n); }
void storage_memzero(void *out, size_t n) { memset(out, 0, n); }

int storage_ext4_map_block(struct storage_volume *v, uint64_t ino,
    const struct ext4_inode_view *in, uint64_t logical, bool create,
    struct storage_ext4_map_result *out)
{
    assert(v == &volume && ino == 12 && in && !create && logical < 192);
    assert(maps < sizeof(map_at) / sizeof(map_at[0])); map_at[maps++] = logical;
    if (maps == fail_map) return fail_error;
    *out = (struct storage_ext4_map_result){.hole = true, .length = 192 - logical};
    for (unsigned i = 0; i < nr_runs; ++i) {
        if (logical >= runs[i].first && logical - runs[i].first < runs[i].count) {
            out->hole = false; out->physical = runs[i].physical + logical - runs[i].first;
            out->length = runs[i].count - (logical - runs[i].first);
            out->unwritten = runs[i].unwritten; return 0;
        }
        if (runs[i].first > logical && runs[i].first - logical < out->length)
            out->length = runs[i].first - logical;
    }
    return 0;
}

int storage_ext4_cache_get(struct storage_volume *v, uint64_t physical,
                           uint8_t **out, bool write)
{
    assert(v == &volume && !write && physical < 256);
    ++cached_reads;
    if (++commands == fail_command) return fail_error;
    memcpy(scratch, media + physical * v->ext4.block_size, v->ext4.block_size);
    *out = scratch; return 0;
}

int storage_ext4_cache_read_blocks(struct storage_volume *v, uint64_t physical,
                                   uint32_t blocks, void *out)
{
    assert(v == &volume && blocks && blocks <= 32 && physical + blocks <= 256);
    if (++commands == fail_command) return fail_error;
    memcpy(out, media + physical * v->ext4.block_size, blocks * v->ext4.block_size);
    memset(scratch, 0xee, sizeof(scratch)); return 0;
}

static uint8_t oracle(uint64_t byte)
{
    uint32_t bs = volume.ext4.block_size;
    uint64_t logical = byte / bs;
    for (unsigned i = 0; i < nr_runs; ++i)
        if (logical >= runs[i].first && logical - runs[i].first < runs[i].count)
            return runs[i].unwritten ? 0 : media[(runs[i].physical + logical - runs[i].first) * bs + byte % bs];
    return 0;
}

static void reset(uint32_t bs)
{
    volume = (struct storage_volume){0}; volume.ext4.block_size = bs;
    maps = cached_reads = commands = fail_map = fail_command = nr_runs = 0;
    fail_error = -RELIEFOS_EIO;
    memset(runs, 0, sizeof(runs));
    for (size_t i = 0; i < sizeof(media); ++i) media[i] = (i * 43 + i / 313) % 251;
}

static void read_range(struct ext4_inode_view *in, uint64_t offset, uint32_t bytes,
                        unsigned expected_maps)
{
    uint8_t *out = malloc(bytes + 2); assert(out); memset(out, 0xa5, bytes + 2);
    uint32_t done = UINT32_MAX;
    assert(!storage_ext4_read_file_range(&volume, 12, in, offset, out + 1, bytes, &done));
    uint32_t expected = offset < in->size ? (in->size - offset < bytes ? in->size - offset : bytes) : 0;
    assert(done == expected && maps == expected_maps && out[0] == 0xa5 && out[bytes + 1] == 0xa5);
    for (uint32_t i = 0; i < done; ++i) assert(out[i + 1] == oracle(offset + i));
    for (uint32_t i = done; i < bytes; ++i) assert(out[i + 1] == 0xa5);
    free(out);
}

int main(int argc, char **argv)
{
    bool baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    for (uint32_t bs = 1024; bs <= 4096; bs *= 2) {
        struct ext4_inode_view in = {.mode = 0100644, .size = 128 * bs};
        reset(bs); nr_runs = 1; runs[0].count = 128; runs[0].physical = 7;
        read_range(&in, 0, 128 * bs, baseline ? 4 : 1);
        printf("{\"block_size\":%u,\"read_blocks\":128,\"mapping_lookups\":%u,\"data_commands\":%u}\n",
               bs, maps, commands);
        if (baseline) continue;
        assert(commands == 4);
        in.size = 128 * bs - 19;
        reset(bs); nr_runs = 1; runs[0].count = 128; runs[0].physical = 7;
        read_range(&in, 17, 128 * bs, 1); assert(cached_reads == 2 && commands == 6);
        /* A later read must resolve mappings anew after a writer changes them. */
        maps = commands = cached_reads = 0; runs[0].physical = 23;
        read_range(&in, 17, 128 * bs, 1);

        in.size = 128 * bs;
        reset(bs); nr_runs = 3;
        runs[0].first = 2; runs[0].count = 40; runs[0].physical = 7;
        runs[1].first = 44; runs[1].count = 35; runs[1].physical = 100; runs[1].unwritten = true;
        runs[2].first = 85; runs[2].count = 43; runs[2].physical = 180;
        read_range(&in, bs + 17, 128 * bs, 6);
        assert(map_at[0] == 1 && map_at[1] == 2 && map_at[2] == 42);
        assert(map_at[3] == 44 && map_at[4] == 79 && map_at[5] == 85);

        reset(bs); nr_runs = 2;
        runs[0].count = 33; runs[0].physical = 7;
        runs[1].first = 33; runs[1].count = 95; runs[1].physical = 100;
        fail_map = 2;
        uint8_t *out = malloc(128 * bs); assert(out); memset(out, 0xa5, 128 * bs);
        uint32_t done;
        assert(storage_ext4_read_file_range(&volume, 12, &in, 0, out, 128 * bs, &done) == -RELIEFOS_EIO);
        assert(done == 33 * bs && maps == 2 && commands == 2);
        for (unsigned i = 0; i < done; ++i) assert(out[i] == oracle(i));
        for (unsigned i = done; i < 128 * bs; ++i) assert(out[i] == 0xa5);

        reset(bs); nr_runs = 1; runs[0].count = 128; runs[0].physical = 7; fail_command = 2;
        memset(out, 0xa5, 128 * bs);
        assert(storage_ext4_read_file_range(&volume, 12, &in, 0, out, 128 * bs, &done) == -RELIEFOS_EIO);
        assert(done == 32 * bs && maps == 1 && commands == 2 && volume.ext4.read_bytes == done);
        for (unsigned i = 0; i < done; ++i) assert(out[i] == oracle(i));
        for (unsigned i = done; i < 128 * bs; ++i) assert(out[i] == 0xa5);

        /* Resumable reads unwind with the same completed prefix; a retry
         * cannot reuse this call's mapping values after ownership changes. */
        reset(bs); nr_runs = 1; runs[0].count = 128; runs[0].physical = 7;
        fail_command = 2; fail_error = -RELIEFOS_EAGAIN;
        assert(storage_ext4_read_file_range(&volume, 12, &in, 0, out, 128 * bs, &done) == -RELIEFOS_EAGAIN);
        assert(done == 32 * bs && maps == 1 && commands == 2);
        maps = commands = cached_reads = fail_command = 0;
        runs[0].physical = 23; read_range(&in, 0, 128 * bs, 1);
        free(out);
        maps = 0; read_range(&in, UINT64_MAX, 1, 0);
    }
    puts("PASS extent/hole/unwritten/EOF mapping reuse and partial read errors");
    return 0;
}
