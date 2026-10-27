#include "internal.h"



/**
 * @brief Copy discovered installer disk identities without inventing mount state.
 * @param disks Writable array of capacity entries; NULL permitted at capacity zero.
 * @param capacity Maximum records to copy, clamped to the discovered disk count.
 * @param out_count Required present-disk count, including omitted records.
 * Ephemeral RAM sources are excluded. Stable IDs remain reserved after unload;
 * absent devices are not copied;
 * short-lived record pointers never escape this execution transaction.
 * @return Zero or validation/execution/inventory-allocation errno; no native I/O.
 * @par Context Storage execution ownership; callers retain their output memory.
 * Actual filesystem attachments are queried through VFS, never a disk flag.
 */
int storage_install_list_disks(struct reliefos_install_disk *disks,
                               uint32_t capacity, uint32_t *out_count)
{
    return storage_inventory_snapshot(disks,capacity,out_count,true);
}
