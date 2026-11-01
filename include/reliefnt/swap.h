/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SWAP_H
#define RELIEFNT_SWAP_H
#include <reliefnt/types.h>
#include <linux/errno.h>

/* Values count usable pages; reserved header and bad pages are excluded. */
struct swap_stats { uint64_t total, free, used; };
struct address_space;
struct task;
/* Prerequisite contention only: no slot transfer was submitted or queued. */
#define SWAP_STORAGE_CONTENDED (-4100)
#ifdef CONFIG_SWAP
/** @brief Evict one proven exclusive private user page.
 * @param as Live root held under the outer execution writer. @param va User page.
 * @return One if a physical page was freed, zero if ineligible, or negative
 * terminal errno. Caller must hold no unrelated spin/reader/loader/usercopy lock;
 * the audited backend completes one transfer synchronously under this gate.
 */
int swap_try_evict(struct address_space *as, uint64_t va);
/** @brief Resolve one slot-backed fault at an audited outer-writer boundary.
 * @param task Live user task. @param address Faulting user address.
 * @param error x86 fault flags. @return One if resolved, zero if not swap,
 * SWAP_STORAGE_CONTENDED when another task owns the transport before submission,
 * otherwise a terminal negative errno. No unrelated spin/reader/loader lock.
 */
int swap_resolve_fault(struct task *task, uint64_t address, uint64_t error);
/** @brief Restore one slot for swapoff without changing leaf access rights.
 * @param task Unreleased user MM owner under the outer execution writer.
 * @param address User address. @return One after restoring a leaf, zero when
 * it is not swapped, contention before submission, or a terminal negative errno.
 * The caller holds no unrelated spin, reader, loader or storage-owner lock.
 */
int swap_restore_leaf(struct task *task, uint64_t address);
/** @brief Bring swapped leaves of one bounded syscall slice into memory.
 * @param task Live execution-owned user task. @param address First user byte.
 * @param length At most 32768 bytes; zero is a no-op. @param write_access True
 * for output buffers. @return Zero, SWAP_STORAGE_CONTENDED before submission,
 * or a terminal negative errno. The caller proves outer writer depth one and
 * no unrelated provider/usercopy/spin lock;
 * no pointer or operation survives a gate release, and no lazy page is filled.
 */
int swap_prepare_user_range(struct task *task, uint64_t address, uint64_t length,
                            bool write_access);
/** @brief Prepare one deferred-exit operand using actual scheduler ownership.
 * @param task Unreleased MM-owning target. @param address User address.
 * @param length At most 32768 bytes. @param write_access Select output access.
 * @return As swap_prepare_user_range; any existing provider is a prerequisite
 * contention. Scope and token end before returning, including for PID zero.
 */
int swap_prepare_user_cleanup(struct task *task, uint64_t address, uint64_t length,
                              bool write_access);
/** @brief Reserve one opaque slot on the highest-priority available area.
 * @return Nonzero token with one reference, or zero if no slot is available.
 * IRQ-safe short metadata lock; no allocation or I/O.
 */
uint64_t swap_slot_alloc(void);
/** @brief Acquire another reference to an extant allocated slot.
 * @param token Live opaque token. @return Zero or negative errno, including
 * -EOVERFLOW when the reference count cannot grow. No allocation or I/O.
 */
int swap_slot_get(uint64_t token);
/** @brief Release one held slot reference; the last returns it to its area.
 * @param token Owned opaque token, zero ignored. No allocation or I/O.
 */
void swap_slot_put(uint64_t token);
/** @brief Write a reserved page to its slot, retaining caller data on error.
 * @param token Held reserved slot. @param physical Retained aligned physical page.
 * @return Zero complete, SWAP_STORAGE_CONTENDED before submission, otherwise
 * terminal negative errno. Task context only; acquires the execution lock and
 * may synchronously poll the audited provider. Caller keeps the page/reference.
 */
int swap_slot_submit_write(uint64_t token,uint64_t physical);
/** @brief Read a valid slot without releasing its durable contents.
 * @param token Held valid slot. @param physical Retained aligned destination page.
 * @return Zero complete, SWAP_STORAGE_CONTENDED before submission, otherwise
 * terminal negative errno. Task context only; acquires the execution lock and
 * may synchronously poll. Destination contents are undefined after failure.
 */
int swap_slot_submit_read(uint64_t token,uint64_t physical);
/** @brief Advance at most budget owned deferred I/O requests.
 * @param budget Maximum requests to progress; zero is a no-op.
 */
void swap_io_poll(uint32_t budget);
/** @brief Snapshot total, free and used usable system swap pages.
 * @param out Required destination, NULL ignored. Draining areas contribute
 * only their still-used slots to total and no free capacity. No allocation/I/O.
 */
void swap_get_stats(struct swap_stats *out);
#else
/** @brief Disabled swap has no eviction candidates. @param as Ignored root.
 * @param va Ignored user address. @return Always zero. */
static inline int swap_try_evict(struct address_space *as, uint64_t va)
{ (void)as; (void)va; return 0; }
/** @brief Disabled swap has no nonresident slots. @param task Ignored task.
 * @param address Ignored address. @param error Ignored fault flags. @return Zero. */
static inline int swap_resolve_fault(struct task *task, uint64_t address, uint64_t error)
{ (void)task; (void)address; (void)error; return 0; }
/** @brief Disabled swap has no leaf to restore. @param task Ignored owner.
 * @param address Ignored address. @return Always zero. */
static inline int swap_restore_leaf(struct task *task, uint64_t address)
{ (void)task; (void)address; return 0; }
/** @brief Disabled swap leaves bounded preparation to ordinary usercopy.
 * @param task Ignored task. @param address Ignored start. @param length Ignored
 * length. @param write_access Ignored direction. @return Always zero.
 */
static inline int swap_prepare_user_range(struct task *task, uint64_t address,
                                          uint64_t length, bool write_access)
{ (void)task; (void)address; (void)length; (void)write_access; return 0; }
/** @brief Disabled swap needs no cleanup page-in. @param task Ignored owner.
 * @param address Ignored start. @param length Ignored count. @param write_access
 * Ignored direction. @return Always zero.
 */
static inline int swap_prepare_user_cleanup(struct task *task, uint64_t address,
                                            uint64_t length, bool write_access)
{ return swap_prepare_user_range(task, address, length, write_access); }
/** @brief Disabled swap cannot reserve a slot. @return Always zero. */
static inline uint64_t swap_slot_alloc(void) { return 0; }
/** @brief Disabled swap cannot retain a slot. @param token Ignored token.
 * @return Always -ENOSYS. */
static inline int swap_slot_get(uint64_t token) { (void)token;return -ENOSYS; }
/** @brief Disabled swap has no reference to release. @param token Ignored token. */
static inline void swap_slot_put(uint64_t token) { (void)token; }
/** @brief Disabled swap cannot write. @param token Ignored. @param physical Ignored.
 * @return Always -ENOSYS. */
static inline int swap_slot_submit_write(uint64_t token,uint64_t physical)
{ (void)token;(void)physical;return -ENOSYS; }
/** @brief Disabled swap cannot read. @param token Ignored. @param physical Ignored.
 * @return Always -ENOSYS. */
static inline int swap_slot_submit_read(uint64_t token,uint64_t physical)
{ (void)token;(void)physical;return -ENOSYS; }
/** @brief Disabled swap has no queued work. @param budget Ignored. */
static inline void swap_io_poll(uint32_t budget) { (void)budget; }
/** @brief Report zero capacity when swap is disabled. @param out Optional output. */
static inline void swap_get_stats(struct swap_stats *out)
{ if(out)*out=(struct swap_stats){0}; }
#endif
#endif
