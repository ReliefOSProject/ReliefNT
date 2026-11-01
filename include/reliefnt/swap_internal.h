/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SWAP_INTERNAL_H
#define RELIEFNT_SWAP_INTERNAL_H
#include <reliefnt/types.h>
#include <reliefnt/blkdev.h>

/* Stack-owned only while the caller retains execution ownership; never copy. */
struct swap_storage_token { uint32_t pid; bool async, write_started, active, slot; };

/** @brief Acquire the inherited storage owner and force bounded synchronous I/O.
 * @param token Required empty stack-owned token, stable until end.
 * @return Zero or -EBUSY when any inherited operation owns storage, -EINVAL
 * for invalid token, or -ESRCH without a current task. Caller holds execution
 * ownership without suspension through end. No lock spans a scheduler wait.
 */
int swap_storage_begin(struct swap_storage_token *token);
/** @brief Identify a transport reservation held by a different live task.
 * @return True only for an other-PID storage owner. Caller holds the execution
 * writer from failed admission through this check; no ownership is changed.
 */
bool swap_storage_other_owner(void);
/** @brief Test whether current PID has a non-swap storage reservation.
 * @return True only for the current task's existing provider reservation;
 * execution writer required. This does not cancel or release ownership.
 */
bool swap_storage_current_owner(void);
/** @brief Test whether a synchronous slot transfer owns storage right now.
 * @return True for slot I/O only, never trusted swapon/off preparation.
 */
bool swap_storage_slot_active(void);
/** @brief Test every provider, swap-token and cleanup reservation.
 * @return True while any such ownership exists. Caller holds the execution writer.
 */
bool swap_storage_busy(void);
/** @brief Enter a single synchronous deferred-cleanup preparation scope.
 * @return Zero, or negative errno for invalid/nested execution ownership.
 * Any existing provider owner is competing; actual PID zero may transfer only
 * while this scope and the outer writer remain held. No request may be queued.
 */
int swap_storage_cleanup_enter(void);
/** @brief End the current cleanup scope before releasing execution ownership. */
void swap_storage_cleanup_leave(void);
/** @brief Release one exact transaction and restore its inherited I/O mode.
 * @param token The original active token, consumed on success; NULL is ignored.
 * Caller holds the same execution transaction and all bounded I/O has returned.
 */
void swap_storage_end(struct swap_storage_token *token);
/** @brief Transfer one bounded fixed block command in a synchronous transaction.
 * @param token Original active transaction. @param reference Held device/code.
 * @param first Device-relative first block. @param count Nonzero bounded blocks.
 * @param buffer Stable kernel bytes until return. @param write Select write.
 * @return Zero, -EOPNOTSUPP for unsupported geometry, -EOVERFLOW for sector
 * conversion overflow, or another terminal errno. EAGAIN is never retained.
 * Caller retains execution ownership and exact device/code/buffer. Provider
 * capability requires independent DMA ownership and synchronous return.
 * Before every attempted write, clean physical cache aliases are evicted by
 * nonallocating, non-I/O callbacks; dirty or pinned entries remain owned.
 */
int swap_storage_transfer(const struct swap_storage_token *token,
                          const struct block_device_ref *reference,uint64_t first,
                          uint32_t count,void *buffer,bool write);
/** @brief Complete the audited provider's cache flush in this transaction.
 * @param token Original active transaction. @param reference Held device/code.
 * @return Zero or terminal errno; no swap request remains owned on return.
 */
int swap_storage_flush(const struct swap_storage_token *token,
                       const struct block_device_ref *reference);
/* Borrowed exact block identity remains pinned by the owning area's reference. */
struct swap_storage_claim {
    struct swap_storage_claim *next;
    struct block_device *device;
    uint64_t first, count;
    bool file, active;
};
/** @brief Reserve a device range against conflicting block swap and raw access.
 * @param claim Required empty area-owned record. @param reference Held device.
 * @param first First device block. @param count Nonzero bounded device blocks.
 * @param file True for an existing filesystem, false for raw block swap.
 * @return Zero or negative errno. Execution ownership must be held; file-file
 * claims may share a filesystem range, while block claims exclude every alias.
 */
int swap_storage_claim_range(struct swap_storage_claim *claim,
                             const struct block_device_ref *reference,uint64_t first,
                             uint64_t count,bool file);
/** @brief Remove an exact area-owned claim after all I/O and slots drain.
 * @param claim Original active record, nullable. Execution ownership required.
 */
void swap_storage_release_range(struct swap_storage_claim *claim);
/** @brief Check a raw user alias or partition-table edit against active claims.
 * @param reference Exact held device identity. @param first First block.
 * @param count Nonzero blocks. @param whole True for a whole-disk mutation.
 * @return One on conflict, zero when clear; invalid input is -EINVAL.
 */
int swap_storage_raw_busy(const struct block_device_ref *reference,uint64_t first,
                          uint64_t count,bool whole);
struct storage_node;
/* Physical and logical positions are in 512-byte native transport sectors. */
struct swap_storage_extent { uint64_t logical,physical,length; bool unwritten; };
/** @brief Retain the claimed file's exact physical device and filesystem range.
 * @param node Held native inode. @param out Required empty owned device output.
 * @param first Required first-sector output. @param count Required sector count.
 * @return Zero or negative errno; outputs transfer only on success.
 */
int swap_storage_file_device(const struct storage_node *node,struct block_device_ref *out,
                             uint64_t *first,uint64_t *count);
/** @brief Obtain one direct provider mapping under the active inode claim.
 * @param node Held native inode. @param offset Block-aligned logical byte offset.
 * @param out Required mapping result. @return Zero or negative errno.
 */
int swap_storage_file_extent(const struct storage_node *node,uint64_t offset,
                             struct swap_storage_extent *out);
/** @brief Initialize an existing unwritten extent, preserving its zero semantics.
 * @param node Claimed native inode. @param offset Block-aligned logical byte offset.
 * @param length Nonzero block-aligned byte count within an unwritten run.
 * @return Zero or negative errno. Activation task; provider may allocate/journal.
 */
int swap_storage_file_initialize(const struct storage_node *node,uint64_t offset,uint64_t length);
#endif
