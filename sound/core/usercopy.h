/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_AUDIO_USERCOPY_H
#define RELIEFNT_AUDIO_USERCOPY_H

#include <reliefnt/sched.h>
#include <reliefnt/heap.h>
#include <reliefnt/paging.h>
#include <reliefnt/swap.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>

struct audio_control_file;
/** @brief Copy at most one pending control event after bounded preparation.
 * @param task User caller or NULL for a kernel buffer. @param file Live OFD.
 * @param buffer Destination. @param count Capacity in bytes.
 * @return Event bytes, normal errno, or prerequisite-contention marker.
 */
int audio_control_read_user(struct task *task, struct audio_control_file *file,
                            void *buffer, uint32_t count);

/** @brief Prepare one actual sound operand outside device and wait-queue locks.
 * @param task Execution-owned caller; NULL or a kernel task uses kernel bytes.
 * @param address First operand byte. @param length At most 32768 bytes.
 * @param output True when the device writes to the operand.
 * @return Zero, EFAULT, or the private prerequisite-contention marker.
 * A successful result remains valid only while the caller retains the outer
 * execution writer; callers must not retain user addresses across a retry.
 */
static inline int audio_user_prepare(struct task *task, uint64_t address,
                                     uint64_t length, bool output)
{
#ifdef CONFIG_SWAP
    if (task && task->kind == TASK_KIND_USER && length) {
        int rc = swap_prepare_user_range(task, address, length, output);
        if (rc == SWAP_STORAGE_CONTENDED) return KERNEL_SYSCALL_SWAP_CONTENDED;
        if (rc < 0 || !(output ? user_range_writable(address, length) :
                                 user_range_ok(address, length))) return -EFAULT;
    }
#else
    (void)task; (void)address; (void)length; (void)output;
#endif
    return 0;
}

/** @brief Check a complete declared user operand without faulting its pages.
 * @param address First byte. @param length Counted size.
 * @return True for an empty operand or a nonwrapping user-aperture span.
 */
static inline bool audio_user_span_valid(uint64_t address, uint64_t length)
{
    return !length || (address >= RELIEFNT_USER_BASE && address < RELIEFNT_USER_TOP &&
                       length <= RELIEFNT_USER_TOP - address);
}

/** @brief Import a small ioctl input, retaining its exact bytes across retry.
 * @param task Live current user task at the outer execution writer.
 * @param address User operand. @param length Nonzero bounded length.
 * @param output Caller-owned destination of at least length bytes.
 * @return Zero, ordinary errno, or prerequisite-contention marker. A matching
 * syscall scope owns the snapshot until common cancellation; no pointer into
 * user memory or borrowed module object survives a scheduling boundary.
 */
static inline int audio_user_import(struct task *task, uint64_t address,
                                    uint32_t length, void *output)
{
#ifdef CONFIG_SWAP
    struct task_user_text snapshot = {0};
    int rc = task_lookup_user_bytes(task, address, length, &snapshot);
    if (rc) return rc;
    __builtin_memcpy(output, snapshot.text, length);
    kernel_free(snapshot.allocation);
    return 0;
#else
    (void)task;
    __builtin_memcpy(output, (const void *)(uintptr_t)address, length);
    return 0;
#endif
}

#endif
