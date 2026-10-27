#include "storage_internal.h"
#include <reliefnt/export.h>
static uint64_t storage_metadata_epoch = 1;

uint64_t storage_metadata_generation(void)
{
    return storage_metadata_epoch;
}

void storage_set_io_async_context(bool enabled)
{
    /* The asynchronous path stores a single pending command, scratch DMA
     * buffer and owner PID globally.  It is safe only while there is one
     * possible caller.  On SMP every operation is completed synchronously
     * inside its storage transaction; a page fault must never inherit or
     * overwrite another CPU's resumable command state. */
    if (smp_cpu_count() > 1u) {
        storage_io_async_context = false;
        storage_io_write_started = true;
        return;
    }
    storage_io_async_context = enabled;
    storage_io_write_started = false;
}

int storage_acquire_task_io(void)
{
    uint32_t pid;

    if (!storage_io_async_context) {
        return 0;
    }
    pid = sched_current_pid();
    if (!pid) {
        return 0;
    }
    if (storage_task_io_owner == 0 || storage_task_io_owner == pid) {
        storage_task_io_owner = pid;
        return 0;
    }
    return -RELIEFOS_EAGAIN;
}

/** @brief Drain exiting-task native transfers before releasing generic I/O ownership.
 * @param pid Exiting identity, zero ignored. Reaper under execution ownership;
 * no module/registry lock spans provider cancellation, which touches only owned
 * driver DMA and discards data instead of dereferencing retired task buffers.
 */
void storage_drain_task_io(uint32_t pid)
{
    if(!pid)return;
    kernel_spin_lock(&storage_transport_lock);
    block_device_cancel_all(pid);
    kernel_spin_unlock(&storage_transport_lock);
    storage_release_task_io(pid);
}

void storage_release_task_io(uint32_t pid)
{
    if (pid && storage_task_io_owner == pid) {
        storage_task_io_owner = 0;
    }
}

/**
 * @brief Test whether the current single-CPU read transaction may be retried after yielding.
 * @return True only for an asynchronous context before any write and with at most one CPU.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
bool storage_async_can_yield(void)
{
    /* AHCI command descriptors, DMA staging buffers, filesystem caches and
     * the pending-command record are still single-instance state.  Allowing
     * a command to survive a syscall yield lets another CPU reuse that state
     * before the original instruction is retried.  Keep the fast retry mode
     * for the single-CPU legacy path, but use a fully synchronous transaction
     * once SMP is active. */
    return storage_io_async_context && !storage_io_write_started &&
           smp_cpu_count() <= 1u;
}

/**
 * @brief Advance the metadata epoch and make the current asynchronous transaction non-replayable.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_begin_mutation(void)
{
    ++storage_metadata_epoch;
    if (storage_io_async_context) {
        storage_io_write_started = true;
    }
}

/**
 * @brief Zero exactly the requested byte extent.
 * @param dst Writable kernel extent of len bytes; nullable only for zero length.
 * @param len Number of bytes to clear.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_memzero(void *dst, size_t len)
{
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < len; ++i) {
        p[i] = 0;
    }
}

/** @brief Notify each pinned native cache provider once without optional imports.
 * @param writer Borrowed stable raw/native writer, NULL for global invalidation.
 * @param lba First physical sector, ignored without writer.
 * @param sectors Sector count, ignored without writer.
 * @return None. Execution ownership keeps all contexts stable; callbacks are
 * IRQ-safe, nonallocating and perform no I/O. A physical alias need not have
 * its own codec; each native provider checks its own cached device identities.
 */
void storage_filesystem_cache_event(const struct storage_volume *writer,uint64_t lba,uint32_t sectors)
{
    const struct storage_filesystem_ops *seen[STORAGE_MAX_VOLUMES+2];uint32_t count=0;
    for(uint32_t i=0;i<STORAGE_MAX_VOLUMES+2;++i) {
        const struct storage_volume *volume=i==0 ? writer : i==1 ? &g_shmem_volume : &g_volumes[i-2];
        const struct storage_filesystem_ops *ops=volume ? volume->fs_ops : NULL;
        if(!ops || (writer ? !ops->invalidate_write : !ops->invalidate))continue;
        bool duplicate=false;
        for(uint32_t n=0;n<count;++n)if(seen[n]==ops){duplicate=true;break;}
        if(duplicate)continue;seen[count++]=ops;
        if(writer)ops->invalidate_write(writer,lba,sectors);else ops->invalidate();
    }
}

/**
 * @brief Retire native metadata and physical-I/O cache entries.
 * @par Context
 * Also advances the mutation epoch used to reject replay of an accepted write.
 * Caller holds storage execution ownership; no namespace or module callback.
 */
void storage_cache_invalidate(void)
{
    ++storage_metadata_epoch;
    storage_filesystem_cache_event(NULL,0,0);
    storage_fat_cache.valid = 0;
    storage_read_cache.valid = 0;
    storage_dir_lookup_cache.valid = 0;
    for (uint32_t i = 0; i < STORAGE_DIR_INDEX_ENTRIES; ++i) {
        storage_dir_index[i].valid = 0;
    }
    storage_dir_index_next = 0;
    storage_dir_iter_cache.valid = 0;
    storage_write_chain_cache.valid = 0;
}

/**
 * @brief Retire sector and directory caches after block I/O.
 * @par Context
 * Advances the mutation epoch while preserving the separate write-chain hint.
 * Caller holds storage execution ownership; no allocation or provider callback.
 */
void storage_sector_cache_invalidate(void)
{
    ++storage_metadata_epoch;
    storage_fat_cache.valid = 0;
    storage_read_cache.valid = 0;
    storage_dir_lookup_cache.valid = 0;
    for (uint32_t i = 0; i < STORAGE_DIR_INDEX_ENTRIES; ++i) {
        storage_dir_index[i].valid = 0;
    }
    storage_dir_index_next = 0;
    storage_dir_iter_cache.valid = 0;
}

/**
 * @brief Copy an exact nonoverlapping byte extent without allocating.
 * @param dst Writable kernel extent of len bytes.
 * @param src Readable kernel extent of len bytes, not overlapping dst.
 * @param len Number of bytes to copy.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_memcpy(void *dst, const void *src, size_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < len; ++i) {
        d[i] = s[i];
    }
}

/**
 * @brief Compare two exact byte extents lexicographically.
 * @param a Readable kernel extent of len bytes.
 * @param b Readable kernel extent of len bytes.
 * @param len Number of bytes to compare.
 * @return Zero if equal, otherwise the difference between the first unequal unsigned bytes.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_memcmp(const void *a, const void *b, size_t len)
{
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

/**
 * @brief Measure a nullable terminated kernel string.
 * @param s Readable NUL-terminated kernel string, nullable.
 * @return Bytes before NUL, or zero for NULL.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
size_t storage_strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) {
        ++n;
    }
    return n;
}

/**
 * @brief Copy and terminate a bounded kernel string, truncating when necessary.
 * @param dst Writable cap-byte kernel buffer, nullable for a no-op.
 * @param cap Buffer capacity including its terminator; zero is a no-op.
 * @param src Readable terminated string, nullable for empty output.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
void storage_copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

/**
 * @brief Compare two terminated kernel strings byte for byte.
 * @param a Readable terminated kernel string, nullable.
 * @param b Readable terminated kernel string, nullable.
 * @return One if both non-NULL strings match, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_text_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b && *a == *b) {
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}

/**
 * @brief Compare terminated kernel strings with ASCII-only case folding.
 * @param a Readable terminated kernel string, nullable.
 * @param b Readable terminated kernel string, nullable.
 * @return One if both non-NULL strings match, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_text_eq_ci(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == 0 && *b == 0;
}



/**
 * @brief Look up a cached directory record in the currently selected volume.
 * @param directory_cluster Native parent cluster, at least two.
 * @param name Required nonempty terminated lookup name.
 * @param out Optional writable copy of the cached node, assigned only on a hit.
 * @return One for a cache hit, zero for invalid input or a miss.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
int storage_dir_index_lookup(uint32_t directory_cluster, const char *name,
                                    struct storage_node *out)
{
    if (directory_cluster < 2 || !name || !name[0]) {
        return 0;
    }
    for (uint32_t i = 0; i < STORAGE_DIR_INDEX_ENTRIES; ++i) {
        struct storage_dir_index_entry *entry = &storage_dir_index[i];
        if (!entry->valid || entry->volume != g_active_volume ||
            entry->directory_cluster != directory_cluster ||
            !storage_text_eq(entry->name, name)) {
            continue;
        }
        if (out) {
            *out = entry->node;
        }
        return 1;
    }
    return 0;
}

/**
 * @brief Publish a copied directory record in the bounded replacement cache.
 * @param directory_cluster Native parent cluster, at least two.
 * @param name Required nonempty terminated name, copied into the cache.
 * @param node Required borrowed live metadata, copied by value.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_dir_index_store(uint32_t directory_cluster, const char *name,
                                    const struct storage_node *node)
{
    struct storage_dir_index_entry *entry;
    if (directory_cluster < 2 || !name || !name[0] || !node) {
        return;
    }
    entry = &storage_dir_index[storage_dir_index_next % STORAGE_DIR_INDEX_ENTRIES];
    storage_dir_index_next = (storage_dir_index_next + 1u) % STORAGE_DIR_INDEX_ENTRIES;
    entry->volume = g_active_volume;
    entry->directory_cluster = directory_cluster;
    entry->node = *node;
    storage_copy_text(entry->name, sizeof(entry->name), name);
    entry->valid = 1;
}

static uint64_t storage_rdtsc(void)
{
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t storage_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

void storage_make_guid(uint8_t guid[16], uint64_t tag,
                              const struct install_disk_state *disk,
                              uint64_t sector_count)
{
    uint64_t a = storage_mix64(storage_rdtsc() ^ tag ^
                               ((uint64_t)(disk ? disk->port : 0) << 32) ^
                               sector_count);
    uint64_t b = storage_mix64(storage_rdtsc() ^ (tag << 1) ^
                               ((uint64_t)(disk ? disk->bus : 0) << 40) ^
                               ((uint64_t)(disk ? disk->slot : 0) << 24) ^
                               ((uint64_t)(disk ? disk->function : 0) << 16));
    for (uint32_t i = 0; i < 8U; ++i) {
        guid[i] = (uint8_t)(a >> (i * 8U));
        guid[8U + i] = (uint8_t)(b >> (i * 8U));
    }
    guid[6] = (uint8_t)((guid[6] & 0x0fU) | 0x40U);
    guid[8] = (uint8_t)((guid[8] & 0x3fU) | 0x80U);
}

static void storage_guid_append_hex2(char *dst, uint32_t *pos,
                                     uint32_t cap, uint8_t value)
{
    static const char hex[] = "0123456789abcdef";
    if (!dst || !pos || *pos + 2U >= cap) {
        return;
    }
    dst[(*pos)++] = hex[value >> 4];
    dst[(*pos)++] = hex[value & 0x0fU];
    dst[*pos] = 0;
}

int storage_guid_valid(const uint8_t guid[16])
{
    uint8_t or_all = 0;
    uint8_t and_all = 0xffU;
    for (uint32_t i = 0; i < 16U; ++i) {
        or_all |= guid[i];
        and_all &= guid[i];
    }
    return or_all != 0 && and_all != 0xffU;
}

void storage_format_guid(const uint8_t guid[16], char *out, uint32_t cap)
{
    uint32_t pos = 0;
    if (!out || cap < RELIEFOS_MACHINE_IDENTITY_UUID_LEN) {
        if (out && cap) {
            out[0] = 0;
        }
        return;
    }
    out[0] = 0;
    for (uint32_t i = 0; i < 16U; ++i) {
        if (i == 4U || i == 6U || i == 8U || i == 10U) {
            out[pos++] = '-';
            out[pos] = 0;
        }
        storage_guid_append_hex2(out, &pos, cap, guid[i]);
    }
}

/**
 * @brief Identify the reserved FAT-family permissions sidecar using ASCII case folding.
 * @param name Readable terminated basename, nullable.
 * @return One for LEONACL.SYS regardless of ASCII case, zero otherwise.
 * @par Context
 * Caller supplies stable kernel buffers; no user-pointer access, locks, allocation or ownership transfer.
 */
int storage_is_acl_metadata_name(const char *name)
{
    return storage_text_eq_ci(name, "LEONACL.SYS");
}

/**
 * @brief Choose the smaller unsigned 32-bit scalar.
 * @param a First value.
 * @param b Second value.
 * @return The smaller argument, unchanged.
 * @par Context
 * Pure scalar operation; any context and no ownership transfer.
 */
uint32_t min_u32(uint32_t a, uint32_t b)
{
    return a < b ? a : b;
}

/**
 * @brief Choose the smaller unsigned 64-bit scalar.
 * @param a First value.
 * @param b Second value.
 * @return The smaller argument, unchanged.
 * @par Context
 * Pure scalar operation; any context and no ownership transfer.
 */
uint64_t min_u64(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}


/**
 * @brief Select a live native volume while preserving the previous selection for restoration.
 * @param node Required borrowed identity whose volume and mount generation remain live.
 * @param old_volume Optional writable previous-selection output; initialized after I/O admission, including before a stale-generation error.
 * @return Zero, -ENOENT for no live volume, -ESTALE for a reused mount, or I/O admission errno.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Non-tmpfs paths may claim task I/O admission. Restore the saved selection on completion; the borrowed node must already retain its mount/provider lifetime.
 */
int storage_select_node_volume(const struct storage_node *node,
                                      struct storage_volume **old_volume)
{
    struct storage_volume *volume = NULL;
    if (node && node->volume_id == STORAGE_SHMEM_VOLUME &&
        (node->flags & STORAGE_NODE_FLAG_TMPFS) && g_shmem_volume.ready)
        volume = &g_shmem_volume;
    else if (node && node->volume_id < STORAGE_MAX_VOLUMES && g_volumes[node->volume_id].ready)
        volume = &g_volumes[node->volume_id];
    if (!volume) {
        return -2;
    }
    /* tmpfs operates on its own inode/page objects under the execution lock.
     * It must not wait for, claim, or overwrite another task's disk command.
     * In particular anonymous-file faults and final inode drops cannot return
     * EAGAIN merely because an unrelated disk read is suspended. */
    if (!(node->flags & STORAGE_NODE_FLAG_TMPFS) || !volume->tmpfs) {
        int ret = storage_acquire_task_io();
        if (ret < 0) return ret;
    }
    if (old_volume) {
        *old_volume = g_active_volume;
    }
    if ((node->flags & STORAGE_NODE_FLAG_EXT_FAMILY) && node->mount_generation &&
        node->mount_generation != volume->mount_generation) return -116;
    g_active_volume = volume;
    return 0;
}

/**
 * @brief Restore a previously borrowed native volume selection.
 * @param old_volume Previous live selection from storage_select_node_volume; NULL leaves selection unchanged.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call.
 */
void storage_restore_volume(struct storage_volume *old_volume)
{
    if (old_volume) {
        g_active_volume = old_volume;
    }
}





/**
 * @brief Translate an allocated FAT-family data cluster using the selected volume geometry.
 * @param cluster Validated data cluster at least two, within the selected filesystem.
 * @return Physical sector address computed from the selected partition and data-area offsets.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. No I/O, allocation, range validation or reference acquisition.
 */
uint64_t cluster_to_lba(uint32_t cluster)
{
    return g_storage.esp_start_lba + g_storage.data_start_sector +
           (cluster - 2u) * g_storage.sectors_per_cluster;
}

/**
 * @brief Locate the FAT32 sector containing an allocated cluster entry.
 * @param cluster Validated cluster index within the selected FAT.
 * @return Physical sector address of its four-byte FAT entry.
 * @par Context
 * Caller holds storage execution ownership; shared volume/cache state must remain selected and live throughout the call. Geometry, including nonzero bytes_per_sector, must already be validated.
 */
uint64_t fat_sector_for_cluster(uint32_t cluster)
{
    return g_storage.esp_start_lba + g_storage.fat_start_sector +
           ((cluster * 4u) / g_storage.bytes_per_sector);
}

/** @brief Format an on-disk GPT GUID with the UEFI mixed-endian UUID convention. */
void storage_partition_guid_text(const uint8_t guid[16], char uuid[37])
{
    static const uint8_t order[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    static const char hex[] = "0123456789abcdef";
    uint32_t pos = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) uuid[pos++] = '-';
        uuid[pos++] = hex[guid[order[i]] >> 4];
        uuid[pos++] = hex[guid[order[i]] & 15];
    }
    uuid[pos] = 0;

}

/** @brief Preserve resumable I/O errors while diagnosing permanent read failure.
 * @param ret Native transport result; -EAGAIN must remain resumable.
 * @return -EAGAIN unchanged, otherwise -EIO. No allocation or callback.
 */
int storage_read_failure(int ret)
{
    if (ret != -RELIEFOS_EAGAIN)
        console_error("[storage] read failure ret=%d active_volume=%u\n", ret,
                      g_active_volume ? g_active_volume->volume_id : 99u);
    return ret == -RELIEFOS_EAGAIN ? ret : -5;
}

EXPORT_SYMBOL(storage_read_failure);

/* Native filesystem modules share this core-owned interface; CRC/release
 * checks bind it to the exact kernel, without a stable cross-release API. */
EXPORT_SYMBOL(cluster_to_lba);
EXPORT_SYMBOL(fat_sector_for_cluster);
EXPORT_SYMBOL(min_u32);
EXPORT_SYMBOL(min_u64);
EXPORT_SYMBOL(storage_begin_mutation);
EXPORT_SYMBOL(storage_cache_invalidate);
EXPORT_SYMBOL(storage_copy_text);
EXPORT_SYMBOL(storage_dir_index_lookup);
EXPORT_SYMBOL(storage_dir_index_store);
EXPORT_SYMBOL(storage_is_acl_metadata_name);
EXPORT_SYMBOL(storage_memcmp);
EXPORT_SYMBOL(storage_memcpy);
EXPORT_SYMBOL(storage_memzero);
EXPORT_SYMBOL(storage_restore_volume);
EXPORT_SYMBOL(storage_select_node_volume);
EXPORT_SYMBOL(storage_strlen);
EXPORT_SYMBOL(storage_text_eq);
EXPORT_SYMBOL(storage_text_eq_ci);

EXPORT_SYMBOL(storage_async_can_yield);
