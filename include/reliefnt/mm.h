/*
 * ReliefOS physical-memory interface: declares page allocation and reclamation.
 * Provides memory statistics and reference management for kernel mappings.
 */
#ifndef RELIEFNT_MM_H
#define RELIEFNT_MM_H

#include <reliefnt/multiboot2.h>
#include <reliefnt/types.h>

struct mm_topology;
struct address_space;
struct task;

/** @brief Check one otherwise-unmapped page against implicit stack fault limits.
 * @param task Execution-owned user task. @param page Aligned candidate page.
 * @param end Receives the exclusive stack top on success.
 * @return True if a read fault is eligible; no allocation or I/O is performed.
 * The caller separately rejects overlapping VMAs and nonempty denied PTEs.
 */
bool mm_user_stack_readable_extent(struct task *task,uint64_t page,uint64_t *end);

/** @brief Observe a pinned root's existing private generation without allocation.
 * @param as Address space retained by its actual owner.
 * @return Its generation, or zero when private metadata does not yet exist.
 * The caller owns the root independently; this is an identity, not an MM pin.
 */
uint64_t mm_context_generation_existing(const struct address_space *as);
/** @brief Read the DEVICE-leaf revision for a pinned address-space root.
 * @param as Execution-owned root, nullable. @return Revision, or zero before
 * private metadata exists. No allocation or root reference is acquired.
 */
uint64_t mm_context_device_revision(const struct address_space *as);
/** @brief Invalidate bounded DEVICE-leaf walkers after a device PTE changes.
 * @param as Live root pinned by the caller. No allocation, I/O or ownership
 * transfer occurs; a missing context has no active generation-bound walker.
 */
void mm_context_device_changed(const struct address_space *as);
/** @brief Count existing nonresident leaves in one execution-pinned root.
 * @param as Borrowed root, nullable. @return Indexed slot-owning leaf count,
 * or zero without a context. This snapshot acquires no root or slot reference.
 */
uint64_t mm_swap_count_existing(const struct address_space *as);
/** @brief Bound child swap-record and shared-span metadata pages for fork.
 * @param as Execution-pinned source root. @param records Source swap leaves.
 * @return Fresh-page upper bound or UINT64_MAX, with no retained references.
 */
uint64_t mm_fork_metadata_page_bound(struct address_space *as, uint64_t records);
/** @brief Ensure a live, independently owned root has a nonzero incarnation.
 * @param as Root pinned by the caller's current execution transaction.
 * @param generation Required nonzero identity output on success.
 * @return Zero or negative ENOMEM; the result does not pin the MM lifetime.
 */
int mm_context_ensure_generation(struct address_space *as,uint64_t *generation);
/** @brief Ensure root identity at an audited reclaim boundary using reserve if needed.
 * @param as Execution-owned root. @param generation Required identity output.
 * @return Zero or negative errno. Caller holds the sole outer writer and no
 * other locks; the exceptional reserve page is returned when the root retires.
 */
int mm_context_ensure_reclaim_generation(struct address_space *as, uint64_t *generation);

/**
 * @brief Initialize the physical-memory allocator from the boot map.
 * @param boot Borrowed handoff and reachable map; BSP after the shared map and framebuffer.
 * An invalid map or metadata exhaustion terminates boot rather than guessing free RAM.
 */
void mm_init(const struct boot_info *boot);
/**
 * @brief Publish sparse metadata after reserving all of it from bootstrap.
 * @param topology Current published bootstrap view; allocations may mutate its ranges.
 * @return 0 or negative errno; BSP only before concurrent PMM use, with interrupts disabled.
 */
int mm_runtime_init(const struct mm_topology *topology);
/**
 * @brief Query whether every byte belongs to trusted RAM in the frozen boot topology.
 * @param phys Inclusive physical byte address.
 * @param bytes Nonzero byte length, without exclusive-end overflow.
 * @return True for trusted RAM including reserved or currently unmapped ranges.
 * Available after runtime initialization; no mapping, reference, or ownership transfer.
 */
bool mm_phys_is_ram(uint64_t phys, uint64_t bytes);
/**
 * @brief Snapshot a page's owned references without acquiring one.
 * @param phys Aligned physical page address.
 * @return Reference count, UINT32_MAX when saturated, or zero for absent/free/reserved pages.
 */
uint32_t mm_page_refcount(uint64_t phys);
/**
 * @brief Return initial allocatable physical capacity in KiB.
 * @return Pool capacity after metadata reservation, without ownership transfer.
 */
uint64_t mm_total_memory_kib(void);
/**
 * @brief Report the physical emergency reserve protected from ordinary allocations.
 * @return Page count, immutable after physical allocator initialization.
 * The reserve is 64 pages when feasible, otherwise one quarter of the pool.
 * This is accounting only; it does not allocate, reclaim, or invoke I/O.
 */
uint64_t mm_emergency_reserve_pages(void);
/**
 * @brief Snapshot unallocated physical page-pool capacity under the MM lock.
 * @return Free physical page bytes divided by 1024; excludes reclaimable cached/heap allocations.
 * Task/IRQ-safe snapshot after physical allocator setup; no allocation, callbacks or ownership transfer.
 */
uint64_t mm_free_memory_kib(void);
/**
 * @brief Translate a supported physical address through the supervisor direct map.
 * @param phys Physical byte address in the mapped direct-map range.
 * @return Kernel pointer including the original page offset, or NULL when the
 *         physical byte is outside the extent or lies in an unmapped hole.
 */
void *mm_phys_to_virt(uint64_t phys);
/**
 * @brief Translate a supported supervisor kernel pointer to a physical address.
 * @param address Pointer within the shared direct map or linked kernel hole.
 * @return Physical byte address; zero for user pointers, other low addresses,
 *         addresses outside the direct-map window, and unmapped direct-map holes.
 * The identity-mapped interval is exactly [KERNEL_HOLE_START, KERNEL_HOLE_END).
 * Pure IRQ/task-safe translation; no allocation, lock, I/O or ownership transfer.
 */
uint64_t mm_virt_to_phys(const void *address);
/**
 * @brief Allocate one zeroed physical page.
 * @return Owned physical address or zero on failure; caller releases its reference.
 */
uint64_t mm_alloc_page(void);
/**
 * @brief Allocate a single page for an audited emergency recovery operation.
 * @return Owned zeroed page or zero; caller releases it normally.
 * May consume protected emergency pages. Only explicitly admitted recovery
 * paths may call this no-reclaim, IRQ-safe primitive; kernel mode alone is not
 * reserve authority. It never initiates callbacks, storage I/O or waits.
 */
uint64_t mm_alloc_emergency_page(void);
/**
 * @brief Allocate a contiguous recovery run with explicit emergency authority.
 * @param page_count Nonzero number of 4 KiB pages.
 * @return Owned zeroed run or zero; caller releases it normally. Like the
 * single-page recovery primitive, this never reclaims, waits, or invokes I/O.
 */
uint64_t mm_alloc_emergency_pages(uint32_t page_count);
/**
 * @brief Allocate an explicitly authorized recovery run below a byte mask.
 * @param page_count Nonzero page count. @param mask Inclusive maximum byte.
 * @return Owned zeroed run or zero; no reclaim, callbacks, waits or I/O.
 */
uint64_t mm_alloc_emergency_pages_below(uint32_t page_count, uint64_t mask);
/**
 * @brief Allocate contiguous physical pages, preferring high RAM above DMA32.
 * @param page_count Requested 4 KiB pages.
 * @return Owned zeroed physical base or 0; IRQ-safe allocator lock, caller frees.
 */
uint64_t mm_alloc_pages(uint32_t page_count);
/** @brief Claim an owned zeroed DMA run entirely below an inclusive mask.
 * @param page_count Number of 4 KiB pages, nonzero and overflow checked.
 * @param mask Maximum final byte bus address.
 * @return Physical address, or 0; IRQ-safe scan/claim, no sleeping; caller frees.
 */
uint64_t mm_alloc_pages_below(uint32_t page_count, uint64_t mask);
/**
 * @brief Release one owned physical page reference.
 * @param phys Nonzero aligned physical page; absent, free and reserved pages are ignored.
 */
void mm_free_page(uint64_t phys);
/**
 * @brief Release one owned page reference and report whether it became free.
 * @param phys Nonzero aligned physical page; invalid or pinned pages are ignored.
 * @return True only when this release made the page available to the allocator.
 * IRQ-safe, with no allocation or I/O; caller must retire mappings and DMA first.
 */
bool mm_free_page_reclaimed(uint64_t phys);
/**
 * @brief Consume one owned physical-page reference per page and reclaim pages whose count reaches zero.
 * @param phys Page-aligned base of an owned run; zero, invalid, reserved or out-of-range runs are ignored.
 * @param page_count Number of consecutive 4 KiB pages; zero is a no-op.
 * @par Context
 * IRQ-safe allocator lock; pinned and already-free pages remain unchanged. Caller must first retire every DMA, mapping and callback using the consumed references; no user access or scheduling.
 */
void mm_free_pages(uint64_t phys, uint32_t page_count);
/**
 * @brief Retain an owned page; saturation permanently pins it.
 * @param phys Aligned owned physical page; no-op if absent, free, or reserved.
 */
void mm_retain_page(uint64_t phys);

#endif
