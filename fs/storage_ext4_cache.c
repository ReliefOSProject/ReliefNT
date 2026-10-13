/* ext4 bounded block caches: fixed-capacity, generation-guarded, lazily
 * allocated.  This file only caches and performs bounded device I/O; it does
 * no on-disk format parsing (see storage_internal.h for the entry contract).
 * The four caches are the block / inode / dir / journal eviction domains;
 * this task drives the block domain through the five public functions and
 * manages the other three in lockstep (flush and invalidate), leaving their
 * routing to the later read/write tasks.  Host tests override the capacity
 * macros with -D to exercise eviction at small scale. */

#ifndef EXT4_BLOCK_CACHE_ENTRIES
#define EXT4_BLOCK_CACHE_ENTRIES 64u
#endif
#ifndef EXT4_INODE_CACHE_ENTRIES
#define EXT4_INODE_CACHE_ENTRIES 64u
#endif
#ifndef EXT4_DIR_CACHE_ENTRIES
#define EXT4_DIR_CACHE_ENTRIES 32u
#endif
#ifndef EXT4_JOURNAL_CACHE_ENTRIES
#define EXT4_JOURNAL_CACHE_ENTRIES 32u
#endif

struct storage_ext4_cache_entry {
    struct storage_volume *volume;
    uint64_t block;
    uint64_t age;
    uint32_t volume_generation;
    uint8_t valid;
    uint8_t dirty;
    uint8_t pinned;
    uint8_t checksum_ok;
    uint8_t data[EXT4_MAX_BLOCK_SIZE];
};

struct ext4_cache_table {
    struct storage_ext4_cache_entry *entries;
    uint64_t age_clock;
    uint32_t capacity;
};

static struct ext4_cache_table ext4_cache_tables[] = {
    {NULL, 0, EXT4_BLOCK_CACHE_ENTRIES},
    {NULL, 0, EXT4_INODE_CACHE_ENTRIES},
    {NULL, 0, EXT4_DIR_CACHE_ENTRIES},
    {NULL, 0, EXT4_JOURNAL_CACHE_ENTRIES},
};
#define EXT4_CACHE_TABLE_BLOCK 0
#define EXT4_CACHE_TABLE_COUNT \
    ((uint32_t)(sizeof(ext4_cache_tables) / sizeof(ext4_cache_tables[0])))

/* Transport descriptors consume physical addresses. The kernel image is
 * identity-mapped below 192 MiB, within AHCI's 32-bit DMA address range.
 * Keep this bounded staging area alive across asynchronous retries; storage
 * transactions serialize access and retain their owner on EAGAIN. */
static uint8_t ext4_read_dma[EXT4_READAHEAD_BLOCKS * EXT4_MAX_BLOCK_SIZE]
    __attribute__((aligned(4096)));

/* Translates a filesystem block to partition sectors, refusing anything
 * outside the volume's geometry or partition range before touching the
 * device ("bounded I/O"). */
static int ext4_cache_block_range(const struct storage_volume *volume, uint64_t block,
                                  uint64_t *out_lba, uint32_t *out_sectors)
{
    uint32_t block_size;
    uint32_t sectors;
    uint64_t offset;

    if (!volume) {
        return -RELIEFOS_EINVAL;
    }
    block_size = volume->ext4.block_size;
    if (block_size < EXT4_MIN_BLOCK_SIZE || block_size > EXT4_MAX_BLOCK_SIZE ||
        (block_size % SECTOR_SIZE) != 0 || block >= volume->ext4.blocks_count) {
        return -RELIEFOS_EINVAL;
    }
    sectors = block_size / SECTOR_SIZE;
    offset = block * (uint64_t)sectors;
    if (offset / sectors != block || offset + sectors > volume->ext_sector_count ||
        volume->ext_start_lba + offset < volume->ext_start_lba) {
        return -RELIEFOS_EINVAL;
    }
    *out_lba = volume->ext_start_lba + offset;
    *out_sectors = sectors;
    return 0;
}

static struct storage_ext4_cache_entry *ext4_cache_lookup(struct ext4_cache_table *table,
                                                          const struct storage_volume *volume,
                                                          uint64_t block)
{
    if (!table->entries) {
        return NULL;
    }
    for (uint32_t i = 0; i < table->capacity; ++i) {
        struct storage_ext4_cache_entry *entry = &table->entries[i];
        if (entry->valid && entry->volume == volume && entry->block == block &&
            entry->volume_generation == volume->mount_generation) {
            return entry;
        }
    }
    return NULL;
}

static bool ext4_cache_ensure(struct ext4_cache_table *table)
{
    size_t bytes;
    uint64_t phys;

    if (table->entries) {
        return true;
    }
    bytes = sizeof(*table->entries) * (size_t)table->capacity;
    /* Keep the bounded cache outside the fixed kernel image load region, as
     * ext2_cache does; an allocation failure only disables caching. */
    phys = mm_alloc_pages((uint32_t)((bytes + 4095u) / 4096u));
    if (!phys) {
        return false;
    }
    table->entries = (struct storage_ext4_cache_entry *)(uintptr_t)phys;
    storage_memzero(table->entries, bytes);
    return true;
}

static int ext4_cache_write_back(const struct storage_volume *volume,
                                 struct storage_ext4_cache_entry *entry)
{
    uint64_t lba;
    uint32_t sectors;
    int ret = ext4_cache_block_range(volume, entry->block, &lba, &sectors);
    if (ret < 0) {
        return ret;
    }
    return storage_write_device(volume, lba, sectors, entry->data);
}

/* Returns the entry holding (volume, block) with fresh device data, or NULL
 * with *err set.  for_write pins the entry until mark_dirty/flush/invalidate.
 * Eviction is LRU by age over unpinned entries; a dirty current-generation
 * victim is written back first, while entries stamped with an older
 * generation are discarded (a newer mount supersedes their contents). */
static struct storage_ext4_cache_entry *ext4_cache_fill(struct ext4_cache_table *table,
                                                        struct storage_volume *volume,
                                                        uint64_t block, bool for_write,
                                                        int *err)
{
    struct storage_ext4_cache_entry *victim = NULL;
    uint64_t lba;
    uint32_t sectors;
    int ret;

    *err = ext4_cache_block_range(volume, block, &lba, &sectors);
    if (*err < 0) {
        return NULL;
    }
    {
        struct storage_ext4_cache_entry *entry = ext4_cache_lookup(table, volume, block);
        if (entry) {
            entry->pinned = entry->pinned || for_write;
            entry->age = ++table->age_clock;
            return entry;
        }
    }
    for (uint32_t i = 0; i < table->capacity; ++i) {
        if (!table->entries[i].valid) {
            victim = &table->entries[i];
            break;
        }
    }
    if (!victim) {
        for (uint32_t i = 0; i < table->capacity; ++i) {
            struct storage_ext4_cache_entry *entry = &table->entries[i];
            if (entry->pinned) {
                continue;
            }
            if (!victim || entry->age < victim->age) {
                victim = entry;
            }
        }
    }
    if (!victim) {
        /* Capacity is full and every entry is pinned. */
        *err = -RELIEFOS_ENOMEM;
        return NULL;
    }
    if (victim->valid && victim->dirty &&
        victim->volume_generation == victim->volume->mount_generation) {
        ret = ext4_cache_write_back(victim->volume, victim);
        if (ret < 0) {
            *err = ret;
            return NULL;
        }
    }
    ret = storage_read_device(volume, lba, sectors, victim->data);
    if (ret < 0) {
        victim->valid = 0;
        victim->dirty = 0;
        *err = ret;
        return NULL;
    }
    victim->volume = volume;
    victim->block = block;
    victim->volume_generation = volume->mount_generation;
    victim->age = ++table->age_clock;
    victim->valid = 1;
    victim->dirty = 0;
    victim->pinned = for_write ? 1 : 0;
    victim->checksum_ok = 0;
    return victim;
}

int storage_ext4_cache_read(struct storage_volume *volume, uint64_t block, void *out)
{
    struct ext4_cache_table *table = &ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK];
    struct storage_ext4_cache_entry *entry;
    uint64_t lba;
    uint32_t sectors;
    int err;

    if (!out) {
        return -RELIEFOS_EINVAL;
    }
    if (!ext4_cache_ensure(table)) {
        /* ext2_cache convention: allocation failure only disables caching
         * for this I/O. */
        err = ext4_cache_block_range(volume, block, &lba, &sectors);
        if (err < 0) {
            return err;
        }
        err = storage_read_device(volume, lba, sectors, ext4_read_dma);
        if (!err) storage_memcpy(out, ext4_read_dma, volume->ext4.block_size);
        return err;
    }
    entry = ext4_cache_fill(table, volume, block, false, &err);
    if (!entry) {
        return err;
    }
    storage_memcpy(out, entry->data, volume->ext4.block_size);
    return 0;
}

/* Batch adjacent cache misses, splitting at every cached block so dirty data
 * is never replaced by stale device bytes. Read fill never evicts dirty or
 * pinned entries: pressure only disables read caching for that block. */
int storage_ext4_cache_read_blocks(struct storage_volume *v, uint64_t first,
                                   uint32_t count, void *out)
{
    struct ext4_cache_table *table = &ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK];
    uint64_t lba;
    uint32_t sectors;
    if (!out || !count || !v || first >= v->ext4.blocks_count ||
        count > v->ext4.blocks_count - first) return -RELIEFOS_EINVAL;
    int ret = ext4_cache_block_range(v, first + count - 1, &lba, &sectors);
    if (ret < 0) return ret;
    bool caching = ext4_cache_ensure(table);
    uint32_t bs = v->ext4.block_size;
    for (uint32_t done = 0; done < count;) {
        struct storage_ext4_cache_entry *entry = ext4_cache_lookup(table, v, first + done);
        if (entry) {
            storage_memcpy((uint8_t *)out + (size_t)done * bs, entry->data, bs);
            entry->age = ++table->age_clock; ++v->ext4.read_cache_hits; ++done;
            continue;
        }
        uint32_t run = 1;
        while (run < count - done && run < EXT4_READAHEAD_BLOCKS &&
               !ext4_cache_lookup(table, v, first + done + run)) ++run;
        ret = ext4_cache_block_range(v, first + done, &lba, &sectors);
        if (ret < 0) return ret;
        ret = storage_read_device(v, lba, run * sectors, ext4_read_dma);
        if (ret < 0) return ret;
        storage_memcpy((uint8_t *)out + (size_t)done * bs, ext4_read_dma, (size_t)run * bs);
        ++v->ext4.read_commands; v->ext4.read_blocks += run;
        for (uint32_t j = 0; caching && j < run; ++j) {
            struct storage_ext4_cache_entry *victim = NULL;
            for (uint32_t i = 0; i < table->capacity; ++i) {
                struct storage_ext4_cache_entry *candidate = &table->entries[i];
                if (!candidate->valid) { victim = candidate; break; }
                if (!candidate->pinned && !candidate->dirty &&
                    (!victim || candidate->age < victim->age)) victim = candidate;
            }
            if (!victim) continue;
            victim->volume = v; victim->block = first + done + j;
            victim->volume_generation = v->mount_generation;
            victim->valid = 1; victim->dirty = 0; victim->pinned = 0; victim->checksum_ok = 0;
            victim->age = ++table->age_clock;
            storage_memcpy(victim->data, (uint8_t *)out + (size_t)(done + j) * bs, bs);
        }
        done += run;
    }
    return 0;
}

int storage_ext4_cache_get(struct storage_volume *volume, uint64_t block, uint8_t **data,
                           bool for_write)
{
    struct ext4_cache_table *table = &ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK];
    struct storage_ext4_cache_entry *entry;
    int err;

    if (!data) {
        return -RELIEFOS_EINVAL;
    }
    if (!ext4_cache_ensure(table)) {
        return -RELIEFOS_ENOMEM;
    }
    entry = ext4_cache_fill(table, volume, block, for_write, &err);
    if (!entry) {
        return err;
    }
    if (for_write) {
        err = storage_ext4_journal_capture(volume, block, entry->data);
        if (err < 0) {
            entry->pinned = storage_ext4_journal_owns(volume, block);
            return err;
        }
    }
    *data = entry->data;
    return 0;
}

int storage_ext4_cache_mark_dirty(struct storage_volume *volume, uint64_t block)
{
    struct ext4_cache_table *table = &ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK];
    struct storage_ext4_cache_entry *entry = ext4_cache_lookup(table, volume, block);

    if (!entry) {
        uint64_t lba;
        uint32_t sectors;
        int ret = ext4_cache_block_range(volume, block, &lba, &sectors);
        return ret < 0 ? ret : -RELIEFOS_ENOENT;
    }
    int ret = storage_ext4_journal_publish(volume, block, entry->data);
    if (ret < 0) return ret;
    entry->dirty = 1;
    entry->checksum_ok = 0;
    entry->pinned = storage_ext4_journal_owns(volume, block);
    return 0;
}

int storage_ext4_cache_flush(struct storage_volume *volume)
{
    int result = 0;

    if (!volume) {
        return -RELIEFOS_EINVAL;
    }
    for (uint32_t t = 0; t < EXT4_CACHE_TABLE_COUNT; ++t) {
        struct ext4_cache_table *table = &ext4_cache_tables[t];
        if (!table->entries) {
            continue;
        }
        for (uint32_t i = 0; i < table->capacity; ++i) {
            struct storage_ext4_cache_entry *entry = &table->entries[i];
            if (entry->volume != volume || !entry->valid) {
                continue;
            }
            if (entry->volume_generation != volume->mount_generation) {
                /* Superseded mount: its blocks cannot be published back. */
                entry->valid = 0;
                entry->dirty = 0;
                entry->pinned = 0;
                continue;
            }
            if (storage_ext4_journal_owns(volume, entry->block)) continue;
            if (entry->dirty) {
                int ret = ext4_cache_write_back(volume, entry);
                if (ret < 0) {
                    if (!result) {
                        result = ret;
                    }
                    continue;
                }
                entry->dirty = 0;
            }
            entry->pinned = 0;
        }
    }
    return result;
}

void storage_ext4_cache_finish(struct storage_volume *volume, uint64_t block, const uint8_t *data)
{
    struct storage_ext4_cache_entry *entry =
        ext4_cache_lookup(&ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK], volume, block);
    if (!entry) return;
    storage_memcpy(entry->data, data, volume->ext4.block_size);
    entry->dirty = 0; entry->pinned = 0; entry->checksum_ok = 0;
}

void storage_ext4_cache_invalidate_write(const struct storage_volume *writer,uint64_t lba,uint32_t count)
{
    if (!writer || !count || lba>UINT64_MAX-count) return;
    for (unsigned t=0;t<EXT4_CACHE_TABLE_COUNT;t++) {
        struct ext4_cache_table *table=&ext4_cache_tables[t]; if (!table->entries) continue;
        for (unsigned i=0;i<table->capacity;i++) {
            struct storage_ext4_cache_entry *e=&table->entries[i];
            if (!e->valid || e->dirty || e->pinned) continue;
            const struct storage_volume *v=e->volume;
            bool same=v==writer;
            if (v->kind==STORAGE_VOLUME_RAM && writer->kind==STORAGE_VOLUME_RAM)
                same=v->ram_base && v->ram_base==writer->ram_base;
            else if (v->transport==writer->transport) {
                if (v->transport==STORAGE_TRANSPORT_AHCI) same=v->hba_port==writer->hba_port;
                else if (v->transport==STORAGE_TRANSPORT_NVME) same=v->nvme==writer->nvme && v->nvme_nsid==writer->nvme_nsid;
                else if (v->transport==STORAGE_TRANSPORT_IDE_PIO) same=v->ide_command_base==writer->ide_command_base && v->ide_drive==writer->ide_drive;
            }
            if (!same) continue;
            uint64_t first=v->ext_start_lba+e->block*(v->ext4.block_size/512);
            if (first<lba+count && lba<first+v->ext4.block_size/512) e->valid=0;
        }
    }
}

void storage_ext4_cache_invalidate(struct storage_volume *volume)
{
    if (!volume) {
        return;
    }
    for (uint32_t t = 0; t < EXT4_CACHE_TABLE_COUNT; ++t) {
        struct ext4_cache_table *table = &ext4_cache_tables[t];
        if (!table->entries) {
            continue;
        }
        for (uint32_t i = 0; i < table->capacity; ++i) {
            struct storage_ext4_cache_entry *entry = &table->entries[i];
            if (entry->volume == volume) {
                entry->valid = 0;
                entry->dirty = 0;
                entry->pinned = 0;
            }
        }
    }
}

/* Called by the mount path once a cached block's metadata checksums have
 * been verified; the cache itself never parses formats. */
static void ext4_cache_note_checksum_ok(struct storage_volume *volume, uint64_t block)
{
    struct storage_ext4_cache_entry *entry =
        ext4_cache_lookup(&ext4_cache_tables[EXT4_CACHE_TABLE_BLOCK], volume, block);
    if (entry) {
        entry->checksum_ok = 1;
    }
}
