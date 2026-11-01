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
#include <reliefnt/lock.h>
#include <reliefnt/panic.h>
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
    bool did_read;
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

/** @brief Count locked virtual bytes owned by the task's shared MM.
 * @param task Borrowed user task under execution ownership.
 * @return Locked bytes, counting each VMA interval once.
 */
uint64_t mm_locked_bytes(struct task *task);

/** @brief Read future-lock policy for a stable CR3 ownership identity.
 * @param as Borrowed live address space. @return Linux MCL_FUTURE/ONFAULT bits.
 */
uint32_t mm_lock_flags(struct address_space *as);

/** @brief Ensure private state exists for one live CR3 and its generation.
 * @param as Borrowed live address space. @return Zero or negative ENOMEM.
 */
int mm_context_ensure(struct address_space *as);

struct mm_identity { uint64_t cr3, generation; };
/** @brief Obtain a generation-bearing identity for a pinned live root.
 * @param as Borrowed address space. @param out Required identity output.
 * @return Zero or negative ENOMEM; identity remains valid until root destruction.
 */
int mm_context_identity(struct address_space *as, struct mm_identity *out);

/* A checked mlock keeps this private record on its reserved kernel stack.
 * Only mm_context_lock_intent_begin/end may link or unlink it. */
struct mm_lock_intent {
    struct mm_lock_intent *next;
    struct mm_identity identity;
    uint64_t start, end;
};
/** @brief Register a transient checked-population interval for a live root.
 * @param as Execution-owned address space. @param start Inclusive page address.
 * @param end Exclusive address. @param intent Caller-owned live stack record.
 * @return Zero or negative ENOMEM. No public lock policy or quota is changed.
 */
int mm_context_lock_intent_begin(struct address_space *as, uint64_t start,
                                 uint64_t end, struct mm_lock_intent *intent);
/** @brief Remove an intent without allocation, even after its root is retired.
 * @param intent Previously registered caller-owned record.
 */
void mm_context_lock_intent_end(struct mm_lock_intent *intent);
/** @brief Check whether a range intersects a transient checked-lock intent.
 * @param as Live execution-owned root. @param start Inclusive address.
 * @param end Exclusive address. @return True when eviction must be excluded.
 * Future eviction providers must query this before retiring a resident leaf;
 * explicit userspace discard remains allowed and advances the MM revision.
 */
bool mm_context_lock_intent_intersects(struct address_space *as, uint64_t start,
                                       uint64_t end);

/* VMA mutation contract: under execution ownership, publish a revision change
 * before writing live VMA slots or VMA permissions. This includes new mappings,
 * merges, splits, lock policy and unmapping; unpublished exec/fork roots have
 * no concurrent walker. A future VMA mutator must follow the same boundary. */
/** @brief Read a live root's population-relevant mutation revision without allocation.
 * @param as Execution-owned address space. @return Revision, zero if absent.
 */
uint64_t mm_context_vma_revision(struct address_space *as);
/** @brief Mark a VMA or resident-leaf mutation for suspended walkers.
 * @param as Execution-owned address space. Writers hold the execution gate;
 * no allocation occurs. VMA creators, splitters, modifiers and removers mark
 * before publication; task_unmap_pages marks resident PTE removal centrally.
 * A protection update can mark an actual PTE change immediately afterward only
 * when no suspension, writer release or reentry-permitting callback occurs
 * between the change and the mark.
 * A missing context means no active walk. Root destruction invalidates identity.
 */
void mm_context_vma_changed(struct address_space *as);

/** @brief Publish future-lock policy on an already ensured MM identity.
 * @param as Borrowed live address space. @param flags Linux future-lock bits.
 */
void mm_context_set_lock_flags(struct address_space *as, uint32_t flags);

/** @brief Retire identity metadata before its CR3 page can be reused.
 * @param as Address space whose root is being destroyed; NULL is ignored.
 */
void mm_context_destroy(struct address_space *as);

/** @brief Publish one root-scoped nonresident slot owner.
 * @param as Live execution-owned root. @param page Aligned user address.
 * @param token Opaque slot with one caller-owned reference.
 * @return Zero after transferring the reference, or negative errno leaving it
 * with the caller. No PTE is changed; publication requires the same writer.
 */
int mm_swap_record_insert(struct address_space *as, uint64_t page, uint64_t token);
/** @brief Insert a reclaim record, allowing its narrowly audited reserve pool.
 * @param as Pinned root. @param page Aligned address. @param token Owned slot.
 * @return Zero on transfer, negative errno retaining caller ownership.
 * Caller holds the sole outer execution writer without other locks.
 */
int mm_swap_record_insert_reclaim(struct address_space *as, uint64_t page, uint64_t token);
/** @brief Borrow the slot token belonging to one root and page.
 * @param as Live execution-owned root. @param page Aligned user address.
 * @return Complete opaque token or zero when absent; no new reference.
 */
uint64_t mm_swap_record_lookup(struct address_space *as, uint64_t page);
/** @brief Find the next swap record in an execution-owned root.
 * @param as Pinned root. @param start Inclusive aligned address.
 * @param page Receives its virtual address. @param token Receives the borrowed
 * full slot identity. @return True when found; no reference survives the writer.
 */
bool mm_swap_record_next(struct address_space *as, uint64_t start,
                         uint64_t *page, uint64_t *token);
/** @brief Remove and release one slot owner, if present.
 * @param as Live execution-owned root. @param page Aligned user address.
 * No PTE is changed; the caller serializes both changes under the writer.
 */
void mm_swap_record_remove(struct address_space *as, uint64_t page);
/** @brief Retain a source slot for an independent child root.
 * @param source Live execution-owned source. @param target New child root.
 * @param page Source and destination virtual page. @return Zero or negative
 * errno; failed insertion releases any newly acquired slot reference. ENOMEM
 * only denotes ordinary child context/record allocation failure, with no I/O.
 */
int mm_swap_clone_leaf(struct address_space *source, struct address_space *target,
                       uint64_t page);
/** @brief Retain a slot owner at a different virtual page or root.
 * @param source Live source root. @param target Live destination root.
 * @param source_page Aligned source page. @param target_page Aligned destination.
 * @return Zero after transfer or negative errno with no target record published.
 */
int mm_swap_copy_record(struct address_space *source, struct address_space *target,
                        uint64_t source_page, uint64_t target_page);

/** @brief Give a new shared-anonymous interval a RAM-backed sharing object.
 * @param as Pinned live root. @param start First page. @param end Exclusive end.
 * @return Zero or negative ENOMEM; requires exclusive execution ownership.
 */
int mm_shared_create(struct address_space *as, uint64_t start, uint64_t end);

/** @brief Remove shared-anonymous interval owners after preflighting any split.
 * @param as Pinned live root. @param start First page. @param end Exclusive end.
 * @return Zero or negative ENOMEM without modification; execution-owned context.
 */
int mm_shared_forget(struct address_space *as, uint64_t start, uint64_t end);

/** @brief Retain source shared-anonymous intervals for a new fork root.
 * @param src Pinned source root. @param dst New destination root.
 * @return Zero or negative ENOMEM only for ordinary context/span allocation;
 * no provider callback or I/O occurs, and partial state is retired with dst.
 */
int mm_shared_clone(struct address_space *src, struct address_space *dst);

/** @brief Populate one shared-anonymous PTE from its RAM backing object.
 * @param as Pinned root. @param page Aligned address. @param flags PTE flags.
 * @return Zero on success, -ENOENT for an unregistered interval or -ENOMEM.
 */
int mm_shared_fault(struct address_space *as, uint64_t page, uint64_t flags);

/** @brief Test whether a shared-anonymous backing page currently exists.
 * @param as Pinned root. @param page Aligned address. @return True if resident.
 */
bool mm_shared_resident(struct address_space *as, uint64_t page);

/** @brief Alias shared backing spans into an already reserved destination.
 * @param as Pinned root. @param source Old first page. @param target New first page.
 * @param length Page-aligned byte count. @return Zero or negative ENOMEM.
 */
int mm_shared_alias(struct address_space *as, uint64_t source, uint64_t target,
                    uint64_t length);

/** @brief Populate one admitted page for an ordinary memory lock.
 * @param task Execution-owned user MM. @param page Aligned page address.
 * @return Zero, negative EFAULT for inaccessible pages, or negative ENOMEM
 *         for allocation failure; private writable COW is made exclusive.
 */
int task_populate_lock_page(struct task *task, uint64_t page);

/** @brief Populate a page using its already located, execution-owned VMA.
 * @param task Current execution-owned user MM. @param vma Live covering slot;
 * the caller revalidates it after every execution suspension.
 * @param page Aligned page address. @return Zero or the fault/provider errno.
 * Performs no VMA-store search; no pointer is retained after return.
 */
int task_populate_lock_vma_page(struct task *task, struct task_vma *vma, uint64_t page);

/** @brief Populate an implicit-stack page already verified to have no VMA.
 * @param task Execution-owned user MM. @param page Aligned page address.
 * @return Zero or the fault/allocator errno. Performs no VMA-store search.
 */
int task_populate_lock_stack_page(struct task *task, uint64_t page);

/** @brief Cooperatively populate an already locked, page-aligned interval.
 * @param start First address. @param end Exclusive end. @param checked Report
 * population/reentry failures instead of leaving best-effort pages lazy.
 * @return Zero or a translated mlock errno. Each batch drops outer execution
 * ownership only after releasing subordinate locks; no VMA/MM pointer survives.
 */
int mm_populate_locked_range(uint64_t start, uint64_t end, bool checked);

/** @brief Admit and populate the faulting part of newly locked stack growth.
 * @param task Execution-owned user MM. @param low New inclusive low page.
 * @param old_low Previous stack low bound. @param mode MCL_FUTURE policy.
 * @return Zero or negative ENOMEM; failure rolls back new PTEs and metadata.
 */
int mm_lock_future_stack(struct task *task, uint64_t low, uint64_t old_low, uint32_t mode);

/** @brief Transfer an already-admitted lock policy to a remap destination.
 * @param task Execution-owned MM. @param start First page. @param end Exclusive end.
 * @param onfault Preserve deferred population. @return Zero or negative errno.
 * The caller checks final net quota and rolls back the destination on failure.
 */
int mm_lock_remap(struct task *task, uint64_t start, uint64_t end, bool onfault);

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

/** @brief Lock a rounded user interval under capability and memlock limits.
 * @param addr First byte. @param len Byte count. @param flags Low 32-bit mlock2 flags.
 * @return Zero or negative EINVAL, EPERM, ENOMEM or EAGAIN.
 */
int64_t syscall_mm_mlock(uint64_t addr, uint64_t len, uint64_t flags);

/** @brief Remove lock policy from the rounded user interval.
 * @param addr First byte. @param len Byte count. @return Zero or negative errno.
 */
int64_t syscall_mm_munlock(uint64_t addr, uint64_t len);

/** @brief Apply current and/or future shared-MM lock policy.
 * @param flags Low 32-bit Linux MCL flags. @return Zero or negative errno.
 */
int64_t syscall_mm_mlockall(uint64_t flags);

/** @brief Remove all current and future shared-MM lock policy.
 * @return Zero or negative EINVAL when there is no current task.
 */
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

/** @brief Copy one residency bit per rounded page, including backing caches.
 * @param addr Aligned user address. @param len Byte count.
 * @param vec Writable user vector containing one byte per page.
 * @return Zero or negative EINVAL, ENOMEM or EFAULT.
 */
int64_t syscall_mm_mincore(uint64_t addr, uint64_t len, uint64_t vec);

/** @brief Apply a memory hint or discard mapped pages after TLB retirement.
 * @param addr Aligned user address. @param len Byte count.
 * @param advice Low 32-bit Linux advice value. @return Zero or negative errno.
 */
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
