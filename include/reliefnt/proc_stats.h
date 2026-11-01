/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_PROC_STATS_H
#define RELIEFNT_PROC_STATS_H
#include <reliefnt/types.h>
#include <reliefnt/text_stream.h>
struct task;
struct task_file;
/** @brief Emit specialized eventfd/timerfd/epoll details without consuming events.
 * @param file Borrowed canonical OFD. @param s Required output.
 * @return Zero or backing metadata errno, serialized by execution ownership. */
int proc_fd_details(struct task_file *file, struct text_stream *s);
/** @brief Settle local runtime before entering/leaving syscall execution.
 * @param kernel True at entry; false at direct user return. IRQ-safe lock. */
void sched_account_mode(bool kernel);
/** @brief Snapshot user/system/idle ticks in caller-owned per-CPU arrays.
 * @param user User ticks. @param system Kernel ticks. @param idle Idle ticks.
 * @param capacity Array length, clamped to CPU maximum; IRQ-safe, no allocation. */
void sched_cpu_times(uint64_t *user, uint64_t *system, uint64_t *idle, uint32_t capacity);
/** @brief Read one thread or group's user/system CPU ticks under scheduler lock.
 * @param task Borrowed target. @param process Sum group. @param user User output.
 * @param system Kernel output. No allocation or ownership transfer. */
void sched_task_cpu_times(const struct task *task, bool process, uint64_t *user, uint64_t *system);
/** @brief Count live OFDs independently of descriptor aliases or task tables.
 * @return Allocated canonical/inline descriptions, execution-owned snapshot. */
uint64_t proc_open_file_count(void);
/** @brief Account a terminal/temporary OFD ownership change.
 * @param delta Plus/minus one. Execution-owned; no references escape. */
void proc_account_file(int delta);
/** @brief Read enforced pipe resource policies without changing them.
 * @param index Zero=max bytes, one=soft pages, two=hard pages.
 * @return Real configured scalar; serialized by execution ownership. */
uint64_t proc_pipe_policy(unsigned index);
struct sched_proc_stats {
    uint64_t created, switches;
    uint32_t last_pid, tasks, running, blocked;
};
/** @brief Snapshot scheduler counters under its lock, without allocation.
 * @param out Required writable snapshot; caller owns execution metadata.
 */
void sched_proc_snapshot(struct sched_proc_stats *out);
/** @brief Emit live Ethernet/route/socket rows without I/O or ownership changes.
 * @param s Validated streaming output slice. @param leaf Known net filename.
 * @return Zero or ENOENT. Caller holds kernel execution ownership.
 */
int net_proc_emit(struct text_stream *s, const char *leaf);
/** @brief Emit IPv4 UDP rows under caller execution ownership, without pointers.
 * @param s Required stream; no references escape or hardware is polled.
 */
void net_udp_proc_emit(struct text_stream *s);
/** @brief Emit successful local UDP delivery counters without polling devices.
 * @param s Required output stream; caller holds execution ownership.
 */
void net_loopback_proc_emit(struct text_stream *s);
/** @brief Emit UNIX sockets under caller execution ownership, without pointers.
 * @param s Required output stream; abstract names use the Linux @ convention.
 */
void unix_proc_emit(struct text_stream *s);
/** @brief Emit stable registered disks and partition geometry through owned refs.
 * @param s Required output stream. @param statistics Select I/O counts.
 * @return Zero or metadata/transport errno. Task execution ownership; no registry
 * lock spans device callbacks; no provider address is exposed.
 */
int block_proc_emit(struct text_stream *s, bool statistics);
/** @brief Emit per-CPU interrupt vector counts without taking execution locks.
 * @param s Required output stream; atomic approximate IRQ-safe snapshots.
 */
void irq_proc_emit(struct text_stream *s);
/** @brief Sum actual delivered external interrupt vectors across all CPUs.
 * @return Monotonic count modulo uint64_t; no allocation, references or locks.
 */
uint64_t irq_proc_total(void);
struct net_proc_counters {
    uint64_t rx_bytes, rx_packets, rx_errors, rx_drops, tx_bytes, tx_packets, tx_errors;
};
/** @brief Copy core NIC transport counters, serialized by execution ownership.
 * @param out Required scalar output, independent of driver code lifetime. */
void net_device_counters(struct net_proc_counters *out);
/** @brief Emit cached ARP neighbours under execution ownership, without probing.
 * @param s Required output stream. */
void net_arp_proc_emit(struct text_stream *s);
struct mm_proc_stats {
    uint64_t allocated, freed, faults;
};
/** @brief Snapshot successful physical page allocation/release and fault events.
 * @param out Required output snapshot. IRQ-safe, no allocation or transfer.
 */
void mm_proc_snapshot(struct mm_proc_stats *out);
/** @brief Count an actual user page-fault exception, including failed faults.
 * @return None. IRQ-safe atomic update; no task/address-space ownership change.
 */
void mm_proc_fault(void);
/** @brief Snapshot resident and reclaimable read-only cache pages in KiB.
 * @param resident Required output. @param reclaimable Required output.
 * IRQ-safe cache lock; no allocation, provider call or retained reference.
 */
void page_cache_memory_kib(uint64_t *resident, uint64_t *reclaimable);
/** @brief Read live tmpfs data pages across mounted and anonymous instances.
 * @return KiB in allocated tmpfs pages; atomic, no allocation or reference change.
 */
uint64_t tmpfs_memory_kib(void);
/** @brief Record a tmpfs data-page ownership change, delta is +1 or -1.
 * @param delta Signed page change. Atomic, usable by a pinned tmpfs provider. */
void tmpfs_account_pages(int delta);
#endif
