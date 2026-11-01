#include "../../block/core/storage_internal.h"
/* Clean, write-through blocks only. Storage transactions serialize access. */
#define EXT2_CACHE_ENTRIES 128u
#define EXT2_CACHE_BYTES 4096u
struct ext2_cache_entry {
    struct storage_volume *volume;
    uint64_t lba;
    uint32_t sectors;
    uint64_t age;
    uint8_t data[EXT2_CACHE_BYTES];
};
static struct ext2_cache_entry *ext2_cache;
static uint64_t ext2_cache_age;
static uint64_t ext2_cache_physical;

/** @brief Forget one retired volume without retaining dangling cache identities.
 * @param volume Quiescent native instance, caller holds storage exclusion.
 * Clean write-through entries only; does not free the shared cache backing.
 */
void ext2_cache_reset(struct storage_volume *volume)
{
    if (storage_read_cache.volume == volume) storage_read_cache.valid = 0;
    if (!ext2_cache) return;
    for (unsigned i = 0; i < EXT2_CACHE_ENTRIES; ++i)
        if (ext2_cache[i].volume == volume) {
            ext2_cache[i].sectors = 0;
            ext2_cache[i].volume = NULL;
        }
}

/** @brief Check an exact physical alias without acquiring a device reference.
 * @param volume Cached volume, nullable. @param writer Optional borrowed writer volume.
 * @param device Optional borrowed exact block identity. @return True for an alias.
 */
static bool ext2_cache_same_device(const struct storage_volume *volume,
                                   const struct storage_volume *writer,const struct block_device *device)
{
    return volume && (volume==writer || (device && volume->device_ref.device==device));
}

/** @brief Evict only clean overlapping physical aliases of a writer device.
 * @param writer Optional borrowed volume. @param device Optional borrowed identity;
 * at least one must be present and neither is retained.
 * @param lba First physical sector. @param sectors Nonzero sector count.
 * Caller holds storage execution; no references, allocation or I/O are taken.
 */
void ext2_cache_invalidate_range(const struct storage_volume *writer,const struct block_device *device,
                                 uint64_t lba,uint32_t sectors)
{
    if (!sectors || (!writer && !device)) return;
    if (storage_read_cache.valid && ext2_cache_same_device(storage_read_cache.volume,writer,device) &&
        (lba <= storage_read_cache.first_lba
            ? storage_read_cache.first_lba - lba < sectors
            : lba - storage_read_cache.first_lba < storage_read_cache.sector_count))
        storage_read_cache.valid = 0;
    if (!ext2_cache) return;
    for (unsigned i = 0; i < EXT2_CACHE_ENTRIES; ++i) {
        if (ext2_cache[i].sectors && ext2_cache_same_device(ext2_cache[i].volume,writer,device) &&
            (lba <= ext2_cache[i].lba
                ? ext2_cache[i].lba - lba < sectors
                : lba - ext2_cache[i].lba < ext2_cache[i].sectors))
            ext2_cache[i].sectors = 0;
    }
}

int ext2_cache_read(uint64_t lba, uint32_t sectors, void *buffer)
{
    if (!ext2_cache) return 0;
    for (unsigned i = 0; i < EXT2_CACHE_ENTRIES; ++i) {
        if (ext2_cache[i].volume == g_active_volume && ext2_cache[i].sectors &&
            lba >= ext2_cache[i].lba && lba - ext2_cache[i].lba <= ext2_cache[i].sectors &&
            sectors <= ext2_cache[i].sectors - (lba - ext2_cache[i].lba)) {
            storage_memcpy(buffer, ext2_cache[i].data + (lba - ext2_cache[i].lba) * SECTOR_SIZE,
                           sectors * SECTOR_SIZE);
            ext2_cache[i].age = ++ext2_cache_age;
            return 1;
        }
    }
    return 0;
}

/** @brief Publish verified clean bytes, allocating owned physical cache pages lazily.
 * @param lba First cached sector.
 * @param sectors Nonzero extent within EXT2_CACHE_BYTES, otherwise ignored.
 * @param buffer Readable kernel extent sectors*512, required for a valid extent.
 * Caller holds storage exclusion; OOM/map failure disables caching and frees any
 * partial run. The native common module retains physical ownership until release.
 */
void ext2_cache_store(uint64_t lba, uint32_t sectors, const void *buffer)
{
    unsigned victim = 0;
    if (!sectors || sectors > EXT2_CACHE_BYTES / SECTOR_SIZE) return;
    if (!ext2_cache) {
        /* Keep the bounded cache outside the fixed kernel image load region.
         * Allocation failure only disables caching for this I/O. */
        size_t bytes = sizeof(*ext2_cache) * EXT2_CACHE_ENTRIES;
        uint64_t phys = mm_alloc_pages((uint32_t)((bytes + 4095u) / 4096u));
        if (!phys) return;
        ext2_cache = (struct ext2_cache_entry *)mm_phys_to_virt(phys);
        if (!ext2_cache) {
            mm_free_pages(phys, (uint32_t)((bytes + 4095u) / 4096u));
            return;
        }
        storage_memzero(ext2_cache, bytes);
        ext2_cache_physical = phys;
    }
    for (unsigned i = 0; i < EXT2_CACHE_ENTRIES; ++i) {
        if (!ext2_cache[i].sectors) { victim = i; break; }
        if (ext2_cache[i].age < ext2_cache[victim].age) victim = i;
    }
    storage_memcpy(ext2_cache[victim].data, buffer, sectors * SECTOR_SIZE);
    ext2_cache[victim].volume = g_active_volume;
    ext2_cache[victim].lba = lba;
    ext2_cache[victim].age = ++ext2_cache_age;
    ext2_cache[victim].sectors = sectors;
}

/** @brief Free clean write-through cache backing after all provider users drain.
 * @return Zero, including empty/repeated release. No disk I/O; storage execution
 * exclusion and no remaining users are required. Clears pointers before free so
 * a later module load starts with no borrowed volume or physical allocation.
 */
int ext2_cache_release(void)
{
    uint64_t physical = ext2_cache_physical;
    ext2_cache = NULL;
    ext2_cache_physical = ext2_cache_age = 0;
    if (physical) {
        size_t bytes = sizeof(struct ext2_cache_entry) * EXT2_CACHE_ENTRIES;
        mm_free_pages(physical, (uint32_t)((bytes + 4095u) / 4096u));
    }
    return 0;
}
