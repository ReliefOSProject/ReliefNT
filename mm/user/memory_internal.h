#ifndef RELIEFNT_KERNEL_SYSCALL_MM_C_INTERNAL_H
#define RELIEFNT_KERNEL_SYSCALL_MM_C_INTERNAL_H

/*
 * ReliefOS memory syscall handlers: VMA metadata, lazy mappings, page faults,
 * and mmap/mprotect/munmap operations.
 */
#include <reliefnt/console.h>
#include <reliefnt/audio.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/drm.h>
#include <reliefnt/mm.h>
#include <reliefnt/smp.h>
#include <reliefnt/paging.h>
#include <reliefnt/page_cache.h>
#include <linux/mount.h>
#include <reliefnt/sched.h>
#include <reliefnt/storage.h>
#include <reliefnt/syscall.h>
#include <reliefnt/syscall_internal.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/userland.h>
#include <reliefnt/sysv_shm.h>
#include <linux/mman.h>

#define PAGE_SIZE 4096ULL
/* Linux MAP_STACK inhibits THP, not automatic growth. User mappings here use
 * only 4 KiB pages, so the ordinary mapping/protection path already enforces it. */
#define LINUX_MAP_SUPPORTED (LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED_NOREPLACE | LINUX_MAP_NORESERVE | LINUX_MAP_STACK)
_Static_assert(LINUX_PROT_READ == TASK_VMA_PROT_READ &&
               LINUX_PROT_WRITE == TASK_VMA_PROT_WRITE &&
               LINUX_PROT_EXEC == TASK_VMA_PROT_EXEC, "VMA protection encoding");

























































/* Linux anonymous mmap reserves virtual address space first.  Physical pages
 * are allocated by the page-fault path, so a large allocator arena does not
 * consume RAM until the process actually touches it. */


/**
 * Load file cache page.
 * @param task Value supplied by the caller.
 * @param vma Value supplied by the caller.
 * @param page Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct file_page_load_context {
    const struct storage_node *node;
    uint64_t offset;
    uint64_t limit;
};




/**
 * Align up page.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t align_up_page(uint64_t value);

/**
 * Align down page.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t align_down_page(uint64_t value);

/**
 * Align user len.
 * @param len Maximum number of elements available in the related buffer.
 * @param out Output storage updated by the function.
 * @return The value or status produced by the operation.
 */
int align_user_len(uint64_t len, uint64_t *out);

/**
 * Task vma free slot.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct task_vma *task_vma_free_slot(struct task *task);

/**
 * Find a free VMA slot without retaining a pointer across metadata growth.
 * @param task User task whose VMA store is searched.
 * @param index_out Receives the stable VMA slot index.
 * @return Zero on success or a negative errno when the store cannot grow.
 */
int task_vma_free_slot_index(struct task *task, uint32_t *index_out);

/**
 * Task vma clear.
 * @param vma Value supplied by the caller.
 */
void task_vma_clear(struct task_vma *vma);

/**
 * @brief Give a copied DRM VMA segment independent backing and OFD pins.
 * @param dst Output pointer to the copied destination VMA whose DRM references
 *        do not yet own pins; may be null, in which case no fields are changed.
 * @param src Borrowed input pointer to a stable source VMA; its optional BO and
 *        task_file references are retained for dst. It may be null, in which
 *        case no fields are changed.
 * @return No value; installs independent backing and OFD pins in dst in place.
 *
 * No VMA-store lock is acquired and no VMA address bounds are changed here; the
 * caller keeps the source metadata stable and sets the destination segment's
 * range. This helper only replaces copied DRM pointers with independently
 * retained pins.
 */
void task_vma_retain_drm(struct task_vma *dst,
                                const struct task_vma *src);

/**
 * Task vma containing.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
struct task_vma *task_vma_containing(struct task *task, uint64_t start, uint64_t end);

/**
 * Task vma can record mapping.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_can_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                       uint64_t prot, uint32_t flags,
                                       const struct storage_node *file_node,
                                       uint64_t file_offset, uint32_t max_prot);

/**
 * Task vma record mapping.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                   uint64_t prot, uint32_t flags,
                                   const struct storage_node *file_node,
                                   uint64_t file_offset, uint32_t max_prot);

/**
 * Task vma overlaps.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_vma_overlaps(const struct task *task, uint64_t start, uint64_t end);

/**
 * Task vma count.
 * @param task Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t task_vma_count(const struct task *task);

uint64_t task_vma_total_bytes(const struct task *task);

/** @brief Check net virtual growth before MAP_FIXED destroys any existing VMA. */
bool task_address_space_can_map(const struct task *task, uint64_t start, uint64_t end);

/**
 * Task unmap pages.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 */
void task_unmap_pages(struct task *task, uint64_t start, uint64_t end);

void syscall_mm_truncate_file(struct task *task, const struct storage_node *node, uint64_t size);

/**
 * Task map anonymous pages.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param page_flags Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int task_map_anonymous_pages(struct task *task, uint64_t start, uint64_t end,
                                    uint64_t page_flags);

/** @brief Distinguish a missing mapping from denied access to an existing VMA. */
int syscall_page_fault_signal_code(struct task *task, uint64_t address);

int syscall_handle_private_anon_fault(struct task *task, uint64_t fault_addr, uint64_t error);

int syscall_handle_task_page_fault(struct task *task, uint64_t fault_addr, uint64_t error);

/** @brief Resolve a user fault in the current task's address space. */
int syscall_handle_user_page_fault(uint64_t fault_addr, uint64_t error);

/**
 * Syscall mm mmap.
 * @param addr Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param fd Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset);

int64_t syscall_mm_brk(uint64_t requested);

int64_t syscall_mm_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len,
                          uint64_t flags, uint64_t new_addr);

int64_t syscall_mm_mlock(uint64_t addr, uint64_t len, uint64_t flags);

int64_t syscall_mm_munlock(uint64_t addr, uint64_t len);

int64_t syscall_mm_mlockall(uint64_t flags);

int64_t syscall_mm_munlockall(void);

/**
 * @brief Apply explicit R/W/X permissions without exceeding existing VMA limits.
 * @param addr Page-aligned first user byte of the protected interval.
 * @param len Byte length rounded up to whole pages; zero is a no-op.
 * @param prot Linux PROT_NONE or a combination of READ, WRITE and EXEC.
 * @return Zero on success; negative EINVAL, ENOMEM, EACCES or EOPNOTSUPP.
 */
int64_t syscall_mm_mprotect(uint64_t addr, uint64_t len, uint64_t prot);

/**
 * Syscall mm munmap.
 * @param addr Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int64_t syscall_mm_munmap(uint64_t addr, uint64_t len);

int64_t syscall_mm_msync(uint64_t addr, uint64_t len, uint64_t flags);

int64_t syscall_mm_mincore(uint64_t addr, uint64_t len, uint64_t vec);

int64_t syscall_mm_madvise(uint64_t addr, uint64_t len, uint64_t advice);

/** @brief Publish one SysV attachment using the existing VMA and PTE allocator.
 * @param task Current user MM, pinned by execution ownership.
 * @param address Exact aligned address, or zero to find a free range.
 * @param length Page-rounded bytes. @param prot Initial protection.
 * @param max_prot Ceiling for later mprotect. @param remap Replace existing range.
 * @param attachment Prepared identity, borrowed until successful publication.
 * @param pages Registry-owned backing; each PTE obtains a physical page reference.
 * @return Address or negative errno. All PTE refs acquired here are rolled back
 * on failure; registry backing and attachment ownership stay with the caller.
 */
int64_t syscall_mm_map_sysv_shm(struct task *task, uint64_t address, uint64_t length,
    uint32_t prot, uint32_t max_prot, bool remap,
    struct sysv_shm_attachment *attachment, const uint64_t *pages);

#endif
