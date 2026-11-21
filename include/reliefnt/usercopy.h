/*
 * ReliefOS user-copy interface: declares checked Ring-3 memory access helpers.
 * Safely transfers buffers and strings between user and kernel address spaces.
 */
#ifndef RELIEFNT_USERCOPY_H
#define RELIEFNT_USERCOPY_H

#include <reliefnt/types.h>
struct task;
struct paging_writable_alias;
/**
 * @brief Copy through the target's page tables rather than the current CR3.
 * @param task Pinned destination task under the kernel execution lock.
 * @param address Start of the destination user range.
 * @param source Kernel bytes to copy.
 * @param size Number of bytes; earlier pages remain written if a later page fails.
 * @return Zero, or -EFAULT for range, demand paging or COW failure.
 */
int user_copy_to_task(struct task *task, uint64_t address, const void *source, uint64_t size);

/**
 * @brief Copy through the target's page tables rather than the current CR3.
 * @param task Pinned source task under the kernel execution lock.
 * @param destination Kernel destination buffer.
 * @param address Start of the source user range.
 * @param size Number of bytes; earlier bytes remain copied if a later page fails.
 * @return Zero, or -EFAULT for range or demand-page failure.
 */
int user_copy_from_task(struct task *task, void *destination, uint64_t address,
                        uint64_t size);

/**
 * @brief Retain the RAM frame of one readable, page-aligned user mapping.
 * @param task Live user task under the kernel execution writer.
 * @param address User page address, with swapped backing prepared by the caller.
 * @param pin Zeroed owner-embedded node, kept until user_unpin_read_page().
 * @return Zero, or -EFAULT for an inaccessible, device or unowned page.
 * The frame survives unmap and swap. Break existing writable COW and use the
 * fork alias registry to keep a subsequent fork from moving the parent's
 * frame on write. Replacing a mapping does not retarget the pin. External
 * readers must be detached before dropping the pin under the same writer.
 */
int user_pin_read_page(struct task *task, uint64_t address,
                       struct paging_writable_alias *pin);
void user_unpin_read_page(struct paging_writable_alias *pin);

/** @brief Validate readable user pages, resolving lazy faults for the current task.
 * @param ptr Inclusive user virtual address. @param len Exact byte length.
 * @return True for an accessible range, false for aperture/overflow/fault errors;
 * zero length succeeds. Execution-owned task context; lazy faults may allocate
 * or synchronously read a retained file provider, so no graph/module lock may
 * span the call. Missing pages are resolved as user data reads, including
 * untouched stack gaps; ordinary stack/VMA permissions and resource limits
 * still apply. The caller owns all buffers/OFDs and performs the copy.
 */
bool user_range_ok(uint64_t ptr, uint64_t len);
#ifdef CONFIG_SWAP
/** Like user_range_ok(), but retain -EAGAIN from a contended lazy file page.
 * Returns zero or -EFAULT otherwise. The caller must prepare swapped pages
 * first and, on -EAGAIN, defer the operation before publishing side effects.
 */
int user_range_readable(uint64_t ptr, uint64_t len);
#endif
/** @brief Prepare current user pages for output, resolving lazy/COW backing.
 * @param ptr User virtual destination. @param len Byte count, zero allowed.
 * @return True for writable pages, false without a user task or on bounds/fault/
 * allocation failure. Execution-owned task context; may allocate/fault backing
 * without a graph/module lock. No buffer/OFD ownership transfers; copyout is
 * performed separately only after the complete range has been validated.
 */
bool user_range_writable(uint64_t ptr, uint64_t len);
/**
 * @brief Return the length of the NUL-terminated user string s, up to max bytes.
 */
size_t user_strlen(const char *s, size_t max);

#endif
