/* Real range writer and inode codec; allocation-free journal/map/cache seams
 * expose scratch lifetime without counting unrelated cache/journal workspaces.
 * The independent image write suite covers those implementations together. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../block/core/storage_internal.h"

static struct storage_volume volume;
static uint8_t media[96 * EXT4_MAX_BLOCK_SIZE], input[65 * EXT4_MAX_BLOCK_SIZE];
static struct { void *pointer; size_t bytes; } live[3];
static unsigned attempts, allocations, before_transaction, starts, stops, aborts;
static unsigned commands, fail_command, fail_alloc, reads, map_limit, alloc_limit;
static unsigned mappings, freed;
static uint64_t mapped_logical;
static bool active, hole, unwritten;

void *kernel_malloc(size_t bytes)
{
    ++attempts;
    if (attempts == fail_alloc) return NULL;
    for (unsigned i = 0; i < 3; ++i) if (!live[i].pointer) {
        void *pointer = malloc(bytes);
        assert(pointer);
        live[i].pointer = pointer; live[i].bytes = bytes;
        ++allocations;
        if (!starts) ++before_transaction;
        return pointer;
    }
    abort();
}

void kernel_free(void *pointer)
{
    if (!pointer) return;
    for (unsigned i = 0; i < 3; ++i) if (live[i].pointer == pointer) {
        free(pointer); live[i].pointer = NULL; live[i].bytes = 0; return;
    }
    abort();
}

void storage_memcpy(void *out, const void *in, size_t bytes) { memcpy(out, in, bytes); }
void storage_memzero(void *out, size_t bytes) { memset(out, 0, bytes); }
int time_wall_clock(struct reliefos_time_info *now)
{ *now = (struct reliefos_time_info){.unix_seconds = 1790640000}; return 0; }

static uint8_t *block(uint64_t physical)
{
    size_t offset = volume.ext_start_lba * 512 + physical * volume.ext4.block_size;
    assert(offset + volume.ext4.block_size <= sizeof(media));
    return media + offset;
}

int storage_ext4_read_group(struct storage_volume *v, uint64_t group,
                            struct storage_ext4_group_view *out)
{
    assert(v == &volume && !group);
    *out = (struct storage_ext4_group_view){.inode_table = 90}; return 0;
}

int storage_ext4_cache_get(struct storage_volume *v, uint64_t physical,
                           uint8_t **out, bool write)
{
    assert(v == &volume && physical == 90 && active);
    (void)write; *out = block(physical); return 0;
}

int storage_ext4_cache_mark_dirty(struct storage_volume *v, uint64_t physical)
{ assert(v == &volume && physical == 90 && active); return 0; }

int storage_ext4_cache_read_blocks(struct storage_volume *v, uint64_t physical,
                                   uint32_t blocks, void *out)
{
    assert(v == &volume && blocks == 1 && active);
    ++reads; memcpy(out, block(physical), volume.ext4.block_size); return 0;
}

void storage_ext4_cache_finish(struct storage_volume *v, uint64_t physical,
                               const uint8_t *data)
{ assert(v == &volume && active && !memcmp(block(physical), data, v->ext4.block_size)); }

int storage_write_device(const struct storage_volume *v, uint64_t lba,
                         uint32_t sectors, const void *data)
{
    assert(v == &volume && active && sectors);
    bool staged = false;
    for (unsigned i = 0; i < 3; ++i)
        if (live[i].pointer == data && live[i].bytes >= sectors * 512) staged = true;
    assert(staged && lba * 512 + sectors * 512 <= sizeof(media));
    if (++commands == fail_command) return -RELIEFOS_EIO;
    memcpy(media + lba * 512, data, sectors * 512); return 0;
}

int storage_ext4_journal_start(struct storage_volume *v, uint32_t credits,
                               struct storage_ext4_handle *handle)
{
    assert(v == &volume && !active && credits == EXT4_JOURNAL_CREDITS);
    ++starts; active = true;
    *handle = (struct storage_ext4_handle){.volume = v, .active = true}; return 0;
}

int storage_ext4_journal_stop(struct storage_ext4_handle *handle)
{
    assert(handle->volume == &volume && handle->active && active);
    ++stops; active = false; handle->active = false; return 0;
}

void storage_ext4_journal_abort(struct storage_ext4_handle *handle, int error)
{ assert(handle->volume == &volume && active && error < 0); ++aborts; }

int storage_ext4_map_block(struct storage_volume *v, uint64_t ino,
                           const struct ext4_inode_view *in, uint64_t logical,
                           bool create, struct storage_ext4_map_result *out)
{
    assert(v == &volume && ino == 1 && in->mode == 0100644 && !create && active);
    mapped_logical = logical;
    *out = (struct storage_ext4_map_result){.physical = 3 + logical,
        .length = map_limit, .hole = hole, .unwritten = unwritten}; return 0;
}

int storage_ext4_alloc_blocks(struct storage_volume *v, uint64_t goal, uint32_t count,
                              uint64_t *first, uint32_t *allocated)
{
    assert(v == &volume && hole && active && count);
    (void)goal; *first = 3 + mapped_logical;
    *allocated = count < alloc_limit ? count : alloc_limit; return 0;
}

int storage_ext4_free_blocks(struct storage_volume *v, uint64_t first, uint32_t count)
{ assert(v == &volume && first >= 3 && count && active); ++freed; return 0; }

int storage_ext4_change_mapping(struct storage_volume *v, uint64_t ino,
    struct ext4_inode_view *in, uint64_t logical, uint64_t physical,
    uint32_t count, bool is_unwritten, bool remove)
{
    assert(v == &volume && ino == 1 && in->mode == 0100644 && active);
    assert((hole || unwritten) && physical == 3 + logical && count);
    assert(!is_unwritten && !remove); ++mappings; return 0;
}

static void reset(uint32_t bs)
{
    assert(!active);
    for (unsigned i = 0; i < 3; ++i) assert(!live[i].pointer);
    volume = (struct storage_volume){.ext_start_lba = 8};
    volume.ext4.block_size = bs; volume.ext4.inode_size = 256;
    volume.ext4.inodes_count = volume.ext4.inodes_per_group = 128;
    volume.ext4.super_view.block_size = bs;
    attempts = allocations = before_transaction = starts = stops = aborts = 0;
    commands = fail_command = fail_alloc = reads = mappings = freed = 0;
    map_limit = alloc_limit = 80; hole = unwritten = false;
    memset(media, 0xa5, sizeof(media)); memset(block(90), 0, bs);
    /* Regular inode #1, 80 initialized file blocks; preserve untouched bytes. */
    block(90)[0] = 0xa4; block(90)[1] = 0x81;
    uint32_t size = 80 * bs;
    for (unsigned i = 0; i < 4; ++i) block(90)[4 + i] = size >> (8 * i);
    for (size_t i = 0; i < sizeof(input); ++i) input[i] = (i * 37 + i / 257) % 251;
}

static void written(uint64_t offset, uint32_t len, unsigned prealloc)
{
    uint32_t done = UINT32_MAX;
    assert(!storage_ext4_write_file_range(&volume, 1, offset, input, len, &done));
    assert(done == len && before_transaction == prealloc);
    assert(starts == stops && !aborts && !active && !freed);
    for (unsigned i = 0; i < 3; ++i) assert(!live[i].pointer);
    assert(!memcmp(block(3) + offset, input, len));
}

int main(int argc, char **argv)
{
    bool baseline = argc > 1 && !strcmp(argv[1], "--baseline");
    for (uint32_t bs = 1024; bs <= 4096; bs *= 2) {
        reset(bs); written(0, 32 * bs, baseline ? 1 : 0);
        assert(starts == 1 && commands == 1 && !reads);
        assert(allocations == (baseline ? 3u : 2u));
        printf("{\"range_block_size\":%u,\"aligned_bytes\":%u,\"allocations\":%u,"
               "\"scratch_allocations\":%u}\n", bs, 32 * bs, allocations, before_transaction);
        if (baseline) continue;

        /* Extent boundaries and shortened allocations remain whole blocks. */
        reset(bs); map_limit = 7; written(bs, 65 * bs, 0);
        assert(starts == 10 && allocations == 20 && !reads);
        reset(bs); map_limit = 7; alloc_limit = 3; hole = true;
        written(0, 32 * bs, 0);
        assert(starts == 11 && mappings == 11 && allocations == 22 && !reads);

        /* Head, tail and a full-block middle share one scratch allocation. */
        reset(bs); written(17, 2 * bs, 1);
        assert(starts == 3 && reads == 2 && allocations == 7);
        assert(block(3)[16] == 0xa5 && block(5)[17] == 0xa5);
        reset(bs); written(0, bs + 17, 1);
        assert(starts == 2 && reads == 1 && allocations == 5);
        assert(block(4)[17] == 0xa5);
        reset(bs); hole = true; written(17, bs - 31, 1);
        assert(starts == 1 && !reads && mappings == 1 && allocations == 3);
        for (unsigned i = 0; i < 17; ++i) assert(block(3)[i] == 0);
        for (unsigned i = bs - 14; i < bs; ++i) assert(block(3)[i] == 0);
        reset(bs); unwritten = true; written(17, bs - 31, 1);
        assert(starts == 1 && !reads && mappings == 1 && allocations == 3);
        assert(block(3)[16] == 0 && block(3)[bs - 14] == 0);

        /* Partial-write ENOMEM occurs before journal entry or media changes. */
        reset(bs); fail_alloc = 1; uint32_t done = UINT32_MAX;
        assert(storage_ext4_write_file_range(&volume, 1, 17, input, bs, &done) == -RELIEFOS_ENOMEM);
        assert(!done && attempts == 1 && !starts && !commands && !active);
        for (unsigned i = 0; i < bs; ++i) assert(block(3)[i] == 0xa5);

        reset(bs); fail_alloc = 2;
        assert(storage_ext4_write_file_range(&volume, 1, 17, input, bs - 31, &done) == -RELIEFOS_ENOMEM);
        assert(!done && starts == 1 && stops == 1 && aborts == 1 && !commands);
        assert(before_transaction == 1 && !active);
        for (unsigned i = 0; i < 3; ++i) assert(!live[i].pointer);
        reset(bs); fail_alloc = 1;
        assert(storage_ext4_write_file_range(&volume, 1, 0, input, bs, &done) == -RELIEFOS_ENOMEM);
        assert(!done && starts == 1 && stops == 1 && aborts == 1 && !commands);
        assert(!before_transaction && !active);
        for (unsigned i = 0; i < 3; ++i) assert(!live[i].pointer);

        /* Partial errors still release scratch and report only prior commits. */
        reset(bs); fail_command = 2;
        assert(storage_ext4_write_file_range(&volume, 1, 17, input, 2 * bs, &done) == -RELIEFOS_EIO);
        assert(done == bs - 17 && starts == 2 && stops == 2 && aborts == 1);
        assert(before_transaction == 1 && !active);
        for (unsigned i = 0; i < 3; ++i) assert(!live[i].pointer);

        reset(bs); written(0, 0, 0);
        assert(!attempts && !starts && !commands);
    }
    puts("PASS real ext range writer scratch allocation, payload and transaction ordering");
    return 0;
}
