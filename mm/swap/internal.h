/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MM_SWAP_INTERNAL_H
#define RELIEFNT_MM_SWAP_INTERNAL_H
#include <reliefnt/swap.h>
#include <reliefnt/lock.h>
#define SWAP_SLOTS_PER_BLOCK 256u

enum swap_slot_state { SWAP_SLOT_FREE, SWAP_SLOT_RESERVED, SWAP_SLOT_WRITING,
    SWAP_SLOT_VALID, SWAP_SLOT_READING, SWAP_SLOT_DRAINING, SWAP_SLOT_BAD };
struct swap_slot { uint32_t refs, next; uint8_t state; };
struct swap_slot_block { struct swap_slot slots[SWAP_SLOTS_PER_BLOCK]; };
struct swap_area {
    struct swap_area *next;
    struct swap_slot_block **blocks;
    uint32_t block_count, initialized_blocks, last_page, free_head, serial;
    uint64_t total, free, last_allocation;
    int32_t priority;
    bool published, draining;
    void *backend;
};
/** @brief Copy one published area's listing counters under the slot lock.
 * @param area Execution-owned published area. @param total Usable capacity.
 * @param used Allocated/in-flight slots. @param priority Signed priority.
 * @return Zero or -EINVAL; outputs change only on success. No I/O/allocation.
 */
int swap_area_report(struct swap_area *area, uint64_t *total, uint64_t *used,
                     int32_t *priority);

/* Only scalar identities cross a checkpoint; the task/MM is reacquired. */
struct swap_drain_cursor {
    uint32_t task_index, pid;
    uint64_t cr3, generation, next_page, restored;
    bool pass_progress;
};

/** @brief Evacuate an exclusive draining area's referenced leaves in a batch.
 * @param area Published draining area with retained backing.
 * @param cursor Persistent scalar-only cursor, initially zero. Its restored
 * count covers successful materializations across all passes.
 * @param budget Maximum candidate/completion units, between 2 and 32.
 * @param work Receives consumed units. @return One after detaching an empty
 * area, zero when another batch is needed, or a negative terminal errno.
 * The caller holds the outer writer, no storage token, and no unrelated lock.
 */
int swap_drain_step(struct swap_area *area, struct swap_drain_cursor *cursor,
                    uint32_t budget, uint32_t *work);

/** @brief Validate a swap v1 header. @param header Readable bytes.
 * @param length Available bytes. @param capacity Complete backing pages.
 * @param last Required last index output. @param bad_count Required count output.
 * @return Zero or negative errno; outputs change only on success.
 */
int swap_header_validate(const void *header,uint64_t length,uint64_t capacity,
                         uint32_t *last,uint32_t *bad_count);
/** @brief Prepare all slot metadata before publication.
 * @param header Complete v1 header. @param length Available bytes.
 * @param capacity Backing pages. @param priority Allocation priority.
 * @param out Required owned area output. @return Zero or negative errno.
 */
int swap_area_create(const void *header,uint64_t length,uint64_t capacity,
                     int32_t priority,struct swap_area **out);
/** @brief Prepare metadata with bounded execution-transaction checkpoints.
 * @param header Complete v1 header. @param length Available bytes.
 * @param capacity Backing pages. @param priority Area priority.
 * @param out Required owned result. @param checkpoint Optional checkpoint.
 * @param context Borrowed checkpoint state. @return Zero or negative errno.
 * On failure all metadata is freed; cleanup also invokes the checkpoint.
 */
int swap_area_create_checkpoint(const void *header,uint64_t length,uint64_t capacity,
                                int32_t priority,struct swap_area **out,
                                int (*checkpoint)(void *),void *context);
/** @brief Publish a completely prepared area. @param area Owned unpublished area.
 * @return Zero or negative errno; the caller retains area ownership.
 */
int swap_area_publish(struct swap_area *area);
/** @brief Stop allocations when no slots remain referenced.
 * @param area Published area. @return Zero or -EBUSY/-EINVAL.
 */
int swap_area_unpublish(struct swap_area *area);
/** @brief Stop new slot allocation while retaining an active area's backing.
 * @param area Published area retained by its exclusive deactivation owner.
 * @return Zero, -EBUSY if already draining, or -EINVAL if not published.
 * Existing slot references and transfers remain valid until explicitly freed.
 */
int swap_area_begin_drain(struct swap_area *area);
/** @brief Restore allocation after an unsuccessful evacuation.
 * @param area The deactivation owner's still-published area, nullable.
 * Leaves every remaining slot and backing reference unchanged.
 */
void swap_area_abort_drain(struct swap_area *area);
/** @brief Unpublish an exclusively draining area once all slots are released.
 * @param area The deactivation owner's published draining area.
 * @return Zero, -EBUSY while slots remain, or -EINVAL for an invalid state.
 */
int swap_area_finish_drain(struct swap_area *area);
/** @brief Release an unpublished, drained area and its slot metadata.
 * @param area Owned unpublished area, NULL ignored.
 */
void swap_area_destroy(struct swap_area *area);
/** @brief Release an unpublished area in bounded batches.
 * @param area Owned unpublished area. @param checkpoint Optional checkpoint.
 * @param context Borrowed checkpoint state. Errors do not prevent cleanup.
 */
void swap_area_destroy_checkpoint(struct swap_area *area,int (*checkpoint)(void *),void *context);
/** @brief Transfer exactly one slot through the area's fixed backing.
 * @param area Published retained area. @param page Backing page index.
 * @param physical Caller-retained aligned page. @param write Transfer direction.
 * @return Zero or terminal errno. No allocation or filesystem activity.
 */
int swap_backend_page_io(struct swap_area *area,uint32_t page,uint64_t physical,bool write);
struct storage_inode_ref;
struct storage_node;
/** @brief Detect unsupported existing mappings, cache and admitted writers.
 * @param inode Owned exact inode. @param node Its borrowed cache key.
 * @return Zero or -EBUSY/-EINVAL. Caller retains execution ownership across
 * the subsequent claim so no new mapping or writer can enter between checks.
 */
int swap_file_check_in_use(struct storage_inode_ref *inode,const struct storage_node *node);
struct block_device_ref;
struct swap_backend;
/** @brief Create a backend and consume an owned exact device on success.
 * @param out Required empty result. @param device In/out owned device token.
 * @param first First permitted device block. @param count Nonzero range blocks.
 * @return Zero or negative errno; ownership stays with caller on error.
 */
int swap_backend_create(struct swap_backend **out,struct block_device_ref *device,
                        uint64_t first,uint64_t count);
/** @brief Append one logically contiguous and physically unique fixed extent.
 * @param backend Unpublished backend. @param logical First logical block.
 * @param physical First device block. @param length Number of blocks.
 * @return Zero or negative errno. Allocates only during area preparation.
 */
int swap_backend_add_extent(struct swap_backend *backend,uint64_t logical,
                            uint64_t physical,uint64_t length);
/** @brief Append an extent, yielding between bounded physical-alias scans.
 * @param backend Unpublished backend. @param logical Next logical block.
 * @param physical First device block. @param length Number of blocks.
 * @param checkpoint Optional safe checkpoint. @param context Callback state.
 * @return Zero or negative errno; no partial extent is installed on failure.
 */
int swap_backend_add_extent_checkpoint(struct swap_backend *backend,uint64_t logical,
    uint64_t physical,uint64_t length,int (*checkpoint)(void *),void *context);
/** @brief Consume backend metadata and its exact device/code reference.
 * @param backend Unpublished or drained backend, nullable.
 */
void swap_backend_destroy(struct swap_backend *backend);
/** @brief Consume a drained backend in bounded batches.
 * @param backend Owned backend, nullable. @param checkpoint Optional yield.
 * @param context Callback state; failure does not interrupt cleanup.
 */
void swap_backend_destroy_checkpoint(struct swap_backend *backend,int (*checkpoint)(void *),void *context);
#endif
