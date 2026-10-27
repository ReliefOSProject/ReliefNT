#ifndef RELIEFNT_KERNEL_SYSCALL_MM_C_INTERNAL_H
#define RELIEFNT_KERNEL_SYSCALL_MM_C_INTERNAL_H

/*
 * ReliefOS memory syscall handlers: VMA metadata, lazy mappings, page faults,
 * and mmap/mprotect/munmap operations.
 */
#include <reliefnt/console.h>
#include <reliefnt/chrdev.h>
#include <reliefnt/device_mapping.h>
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
    const struct task_vma *vma;
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

/** @brief Clear one retired VMA after its user PTEs have been removed.
 * @param vma Owned metadata slot, nullable; all backing references are consumed.
 * Execution-owned task context without graph/module locks. Final device/OFD,
 * executable and inode puts finish before zeroing; callbacks may perform I/O.
 */
void task_vma_clear(struct task_vma *vma);

/** @brief Retain a copied file VMA's canonical OFD and immutable ELF source.
 * @param dst Unowned copied destination, nullable. @param src Stable source, nullable.
 * Execution-owned task context; no VMA bounds or device references are changed.
 * Canonical source references allocate nothing and retain original provider
 * identity/writer exclusion. No graph/module lock or ownership transfer.
 */
void task_vma_retain_file(struct task_vma *dst,
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
 * @param file_offset Offset of the first mapped file byte.
 * @param max_prot Maximum protection authorized by the source description.
 * @param mapping_file Source OFD for mmap, NULL for anonymous/ELF metadata;
 *        adjacent mappings coalesce only when this OFD identity also matches.
 * @return The value or status produced by the operation.
 */
int task_vma_can_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                       uint64_t prot, uint32_t flags,
                                       const struct storage_node *file_node,
                                       uint64_t file_offset, uint32_t max_prot,
                                       struct task_file *mapping_file);

/**
 * Task vma record mapping.
 * @param task Value supplied by the caller.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @param prot Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @param file_node Value supplied by the caller.
 * @param file_offset Offset of the first mapped file byte.
 * @param max_prot Maximum protection authorized by the source description.
 * @param mapping_file Source OFD for mmap, NULL for anonymous/ELF metadata;
 *        adjacent mappings coalesce only when this OFD identity also matches.
 * @return The value or status produced by the operation.
 */
int task_vma_record_mapping(struct task *task, uint64_t start, uint64_t end,
                                   uint64_t prot, uint32_t flags,
                                   const struct storage_node *file_node,
                                   uint64_t file_offset, uint32_t max_prot,
                                       struct task_file *mapping_file);

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

/** @brief Map anonymous/provider bytes or an independently pinned device backing.
 * @param addr Fixed address or placement hint, checked against the user aperture.
 * @param len Nonzero byte length, rounded to complete pages without overflow.
 * @param prot Linux PROT bits, constrained by descriptor/provider access and W^X.
 * @param flags Linux mmap flags, including validated fixed/no-replace semantics.
 * @param fd Original OFD admitted by the syscall, ignored for anonymous mappings.
 * @param offset Page-aligned file offset or provider mapping selector.
 * @return User address or negative errno. Execution-owned current MM required;
 * no graph/module lock spans callbacks, allocation or unmap. On success one
 * provider/device reference transfers to the VMA; unpublished failures drain
 * installed PTEs and release each temporary owner before returning.
 */
int64_t syscall_mm_mmap(uint64_t addr, uint64_t len, uint64_t prot,
                        uint64_t flags, uint64_t fd, uint64_t offset);

int64_t syscall_mm_brk(uint64_t requested);

/** @brief Resize anonymous memory or shrink/move a movable retained device VMA.
 * @param old_addr Page-aligned source address in the current execution-owned MM.
 * @param old_len Nonzero source bytes. @param new_len Nonzero destination bytes.
 * @param flags Linux MAYMOVE/FIXED flags; FIXED requires MAYMOVE.
 * @param new_addr Aligned destination for FIXED, ignored otherwise.
 * @return Resulting user address or negative errno. Device mappings never grow
 * beyond their initial PFN mapping (EFAULT); fixed moves require the complete
 * VMA and resolve each actual provider page. A failed destination leaves source
 * PTEs/backing retained. No graph/module lock spans allocation or final puts.
 */
int64_t syscall_mm_mremap(uint64_t old_addr, uint64_t old_len, uint64_t new_len,
                          uint64_t flags, uint64_t new_addr);

int64_t syscall_mm_mlock(uint64_t addr, uint64_t len, uint64_t flags);

int64_t syscall_mm_munlock(uint64_t addr, uint64_t len);

int64_t syscall_mm_mlockall(uint64_t flags);

int64_t syscall_mm_munlockall(void);

/** @brief Protect a covered user range while preserving split backing references.
 * @param addr Page-aligned start in the execution-owned current user MM.
 * @param len Byte length rounded to pages; zero is a no-op.
 * @param prot Linux PROT bits bounded by every affected VMA's maximum access.
 * @return Zero or negative EINVAL/ENOMEM/EACCES/EOPNOTSUPP. Metadata slots are
 * reserved before PTE changes; device permission changes synchronously flush
 * remote user TLBs. No graph/module lock spans heap growth or backing release;
 * successful splits acquire independent core references to the original lease.
 */
int64_t syscall_mm_mprotect(uint64_t addr, uint64_t len, uint64_t prot);

/** @brief Remove user PTEs and release fully retired provider/device references.
 * @param addr Page-aligned start in the execution-owned current user MM.
 * @param len Nonzero byte length rounded to pages without aperture overflow.
 * @return Zero or negative errno. Split-capability and free metadata are checked
 * before modifying the VMA store; retained slices preserve provider offsets.
 * Final backing/OFD puts may perform I/O and run without graph/module locks.
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
