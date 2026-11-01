/*
 * ReliefOS paging interface: defines address-space and user-page operations.
 * Exposes page mapping, protection, unmapping, and memory-layout constants.
 */
#ifndef RELIEFNT_PAGING_H
#define RELIEFNT_PAGING_H

#include <reliefnt/types.h>

struct mm_topology;

#define RELIEFNT_USER_BASE 0x0000000000200000ULL
/* The exclusive end of x86_64's canonical low user-address half. */
#define RELIEFNT_USER_TOP  0x0000800000000000ULL
#define RELIEFNT_USER_MMAP_BASE 0x0000000008000000ULL
#define RELIEFNT_USER_HEAP_BASE 0x0000000001000000ULL
#define RELIEFNT_USER_HEAP_LIMIT RELIEFNT_USER_MMAP_BASE
#define RELIEFNT_USER_STACK_PAGES 16u
/* Native user-stack growth window: 64 MiB, leaving the mmap arena below.
 * Linux's default soft RLIMIT_STACK is 8 MiB and its hard default is
 * RLIM_INFINITY; the window is a platform bound, not a rlimit substitute. */
#define RELIEFNT_USER_STACK_MAX_PAGES 16384u
/* Supervisor-only sparse physical map, shared by every process root. Its
 * virtual capacity is independent of installed CPU width or actual RAM. */
#ifndef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0xffff800000000000ULL
#endif
#ifndef RELIEFNT_KERNEL_DIRECT_MAP_SIZE
#define RELIEFNT_KERNEL_DIRECT_MAP_SIZE (64ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL)
#endif
/* The current loader places the kernel at 128 MiB.  Keep a
 * supervisor-only hole in every user CR3 so creating user page tables never
 * replaces those identity-mapped kernel PDEs. */
#define RELIEFNT_KERNEL_HOLE_START 0x0000000008000000ULL
#define RELIEFNT_KERNEL_HOLE_END   0x000000000c000000ULL
_Static_assert(RELIEFNT_KERNEL_HOLE_END < RELIEFNT_USER_TOP &&
               RELIEFNT_USER_HEAP_LIMIT <= RELIEFNT_KERNEL_HOLE_START,
               "user heap, kernel hole and mmap arena must stay ordered");

#define RELIEFNT_PAGE_PRESENT 0x001ULL
#define RELIEFNT_PAGE_WRITABLE 0x002ULL
#define RELIEFNT_PAGE_USER 0x004ULL
#define RELIEFNT_PAGE_PWT 0x008ULL
#define RELIEFNT_PAGE_PCD 0x010ULL
#define RELIEFNT_PAGE_DIRTY 0x040ULL
#define RELIEFNT_PAGE_PAT 0x080ULL
/* x86_64 makes bits 9-11 of a present PTE available to software.  A COW
 * mapping is deliberately read-only; the page-fault handler copies it before
 * restoring write permission for the faulting address space. */
#define RELIEFNT_PAGE_COW 0x200ULL
#define RELIEFNT_PAGE_DEVICE 0x400ULL
/* In a nonpresent user PTE bit 53 denotes a swap leaf.  Its
 * complete opaque slot token is owned separately, never packed into the PFN. */
#define RELIEFNT_PAGE_SWAP (1ULL << 53)
/* Software-only bit outside the physical address field. Shared RAM is owned,
 * unlike borrowed device pages, and must not become COW during fork. */
#define RELIEFNT_PAGE_SHARED (1ULL << 52)
/* A shared cache leaf with logical write permission traps its first actual
 * write so dirty state is committed before hardware write permission. */
#define RELIEFNT_PAGE_WRITE_NOTIFY (1ULL << 54)
/* A non-present leaf still owns its backing page while PROT_NONE is active. */
#define RELIEFNT_PAGE_PROTNONE 0x800ULL
#define RELIEFNT_PAGE_BACKED (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_PROTNONE)
#define RELIEFNT_PAGE_NOEXEC (1ULL << 63)
#define RELIEFNT_PHYS_ADDR_MASK 0x000ffffffffff000ULL
/* Software ownership marker in non-leaf entries, ignored by x86 hardware. */
#define RELIEFNT_PAGE_TABLE_OWNED 0x200ULL
#define RELIEFNT_PAGE_TABLE_STAGED 0x400ULL

struct address_space {
    uint64_t *pml4;
    uint64_t cr3;
    uint64_t user_page_count;
};

/** @brief Ownership state of one user leaf without resolving a fault. */
enum paging_user_entry_kind {
    PAGING_USER_ENTRY_EMPTY,
    PAGING_USER_ENTRY_RESIDENT,
    PAGING_USER_ENTRY_PROTNONE,
    PAGING_USER_ENTRY_SWAP,
};

/** @brief Classify a user leaf without interpreting a swap entry as a PFN.
 * @param as Address space held stable by the caller, nullable.
 * @param va User byte address within the leaf.
 * @return EMPTY, RESIDENT, PROTNONE or SWAP; an invalid address is EMPTY.
 * Caller serializes page-table mutations. No reference or ownership changes.
 */
enum paging_user_entry_kind paging_user_entry_kind(const struct address_space *as,
                                                   uint64_t va);
/** @brief Inspect at most one user leaf or skip one absent page-table branch.
 * @param as Execution-owned root. @param cursor In/out aligned user cursor.
 * @param page Receives the resident candidate only when the result is two.
 * @return Two for a resident candidate, one for an empty branch or ineligible
 * leaf, zero at the user end or on invalid input. No pointer survives the call.
 */
int paging_user_pressure_step(struct address_space *as, uint64_t *cursor, uint64_t *page);

/**
 * @brief Query the CPU's architectural physical-address width.
 * @return CPUID physical width from 32 through 52, or zero when unavailable/invalid.
 * Safe before the PMM starts; no allocation or ownership transfer.
 */
uint8_t paging_physical_bits(void);
/**
 * @brief Install temporary low/direct aliases and the user kernel-hole template.
 * BSP before bootstrap allocation or users; enables NX and loads the kernel CR3.
 * Sparse direct-map publication replaces the temporary branch afterward.
 */
void paging_init_user_identity(void);
/**
 * @brief Construct and activate sparse shared direct-map branches from boot RAM.
 * @param topology Published bootstrap topology, mutable by table reservations.
 * @return 0 on success or negative errno. BSP only, before PMM and user roots;
 * preserves explicitly retained boot and device resources across the switch.
 */
int paging_direct_map_init(const struct mm_topology *topology);
/**
 * Enable per-CPU paging features required by user address spaces.
 * The NXE bit lives in an MSR and must be configured independently on every
 * AP; it is not inherited from the BSP when an AP starts.
 */
void paging_init_cpu(void);
/**
 * @brief Return the physical address of the kernel page-table root (the CR3 value).
 */
uint64_t paging_kernel_cr3(void);
/** @brief Load a page-table root and remember the prior mapping sequence.
 * @param cr3 Physical page-table root, or zero to select the kernel root.
 * @return None. Caller must remain on this CPU and serialize its CR3/cache
 * state through the load; no page ownership or reference is transferred.
 * The acquired sequence snapshot precedes the load, so concurrent later
 * mapping changes remain visible to the next conditional reload.
 */
void paging_load_cr3(uint64_t cr3);
/** @brief Reload CR3 when the root or global mapping sequence has changed.
 * @param cr3 Physical page-table root, or zero to select the kernel root.
 * @return None. Caller must remain on this CPU and serialize its local
 * CR3/cache state; the global sequence is read with acquire ordering.
 */
void paging_load_cr3_if_changed(uint64_t cr3);
/** @brief Publish a mapping change for conservative CR3 invalidation.
 * @return None. Concurrent mapping owners may call this after their page-table
 * writes; a release atomic increment publishes those writes to readers.
 */
void paging_note_mm_change(void);
/** @brief Read the globally published mapping-change sequence.
 * @return Sequence acquired atomically, wrapping modulo 2^64. Pure read;
 * it transfers no page ownership and does not itself reload or invalidate.
 */
uint64_t paging_mm_change_seq(void);
/**
 * @brief Check physical representability independently of RAM type or mapping.
 * @param phys First physical byte.
 * @param len Byte length, including zero; addition must not overflow.
 * @return True when the entire interval fits the CPU's physical-address width.
 */
bool paging_phys_addressable(uint64_t phys, uint64_t len);
/**
 * @brief Check actual identity-mapped coverage in the shared kernel direct map.
 * @param phys First physical byte.
 * @param len Byte length, including zero; addition must not overflow.
 * @return True only when every covered byte resolves to its requested physical
 * byte within CPU and direct-map limits. Does not establish RAM ownership.
 */
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len);
/** @brief Read the cache bits of a direct-mapped supervisor page.
 * @param phys Aligned physical page. @param flags Receives PWT/PCD/PAT bits.
 * @return True only for the matching present supervisor alias; no allocation.
 * The execution writer must serialize later use with cache-policy changes.
 */
bool paging_kernel_direct_cache_flags(uint64_t phys,uint64_t *flags);
/** An owner-embedded physical cache-policy claim, serialized by the MMIO lock. */
struct paging_cache_claim {
    struct paging_cache_claim *next;
    struct paging_cache_claim **previous;
    struct paging_cache_claim *left, *right;
    uint64_t start, end, flags, restore_flags, subtree_end;
    int tree_height;
    bool active, direct, restore_pending;
};
/** @brief Select and own the physical cache policy for one DEVICE page.
 * @param claim Empty owner-embedded claim, retained until explicit release.
 * @param physical Nonzero aligned representable physical page.
 * @param flags Receives its exact PWT/PCD/PAT policy.
 * @return True on success; false on invalid input or inconsistent existing owners.
 * Task context under execution ownership; no allocation, callback or I/O.
 */
bool paging_cache_claim_select(struct paging_cache_claim *claim,uint64_t physical,
                               uint64_t *flags);
/** @brief Claim an exact cache policy for a high supervisor mapping.
 * @param claim Empty owner-embedded claim. @param physical Aligned first page.
 * @param bytes Nonzero aligned length. @param flags Exact PWT/PCD/PAT policy.
 * @return True only if the entire range is outside the direct map and compatible
 * with every overlapping owner. No allocation, callback or I/O; task context.
 */
bool paging_cache_claim_exact(struct paging_cache_claim *claim,uint64_t physical,
                              uint64_t bytes,uint64_t flags);
/** @brief Merge adjacent equal-policy claims without an unprotected interval.
 * @param first Earlier active claim. @param second Later active claim, consumed
 * only on success. @return True when merged; false with both claims unchanged.
 * Direct-map claims stay page-sized to preserve exact deferred restoration.
 * Owner exclusion is required; no allocation, callback or I/O.
 */
bool paging_cache_claim_merge(struct paging_cache_claim *first,
                              struct paging_cache_claim *second);
/** @brief Release a physical-policy owner after its last mapping is retired.
 * @param claim Previously acquired owner-embedded node; inactive is a no-op.
 * Deferred direct-map attributes are restored after the last overlapping claim,
 * with a TLB shootdown. No physical backing is acquired or freed.
 */
void paging_cache_claim_release(struct paging_cache_claim *claim);
/**
 * @brief Find the first contiguous identity-mapped physical run in an interval.
 * @param start Inclusive page-aligned physical search start.
 * @param end Exclusive page-aligned physical search end.
 * @param mapped_start Required output for the first mapped page.
 * @param mapped_end Required output for its exclusive contiguous mapped end.
 * @return True for a nonempty mapped run, false when none exists or input is
 * invalid. This is a coverage query, not a RAM-type or ownership assertion.
 */
bool paging_kernel_direct_map_next(uint64_t start, uint64_t end,
                                   uint64_t *mapped_start, uint64_t *mapped_end);
/**
 * @brief Translate a physical byte to its supervisor-only shared direct-map alias.
 * @param phys Physical byte address to translate.
 * @return Borrowed alias only when the installed leaf maps that byte identically;
 * otherwise NULL. This does not assert RAM type or transfer ownership.
 */
void *paging_kernel_direct_map(uint64_t phys);
/** @brief Map validated PCI BAR pages in the independent device virtual window.
 * @param physical Page-aligned device base with no RAM or direct-map alias.
 * @param bytes Nonzero page-aligned device length, entirely representable by PTEs.
 * @return Owned supervisor UC/RW/NX mapping or NULL on a cache-policy conflict
 * or other failure, shared by existing and future process CR3s. Task context
 * after heap/MM initialization; no physical RAM ownership is transferred.
 * Caller validates the BAR and drains device/users before exact unmap.
 */
void *paging_ioremap_uc(uint64_t physical, uint64_t bytes);
/** @brief Map one independently owned device page with its user cache policy.
 * @param physical Aligned retained page outside the kernel direct map.
 * @param cache_bits PWT/PCD/PAT bits from the admitted user leaf.
 * @return Owned supervisor RW/NX page or NULL on invalid input, policy conflict
 * or allocation failure.
 * No physical ownership is transferred. Retire it with paging_iounmap(page,4096)
 * before releasing the provider backing and its cache-policy ownership.
 */
void *paging_ioremap_device_page(uint64_t physical,uint64_t cache_bits);
/** @brief Remove one exact owned high PCI or DEVICE alias after users stop.
 * @param pointer Original base returned by either high mapping helper.
 * @param bytes Original full page-aligned mapping length.
 * @return True if retired, false for an unknown base/length. Task context, all CPU
 * TLBs synchronized before owned table pages and cache-policy claim are
 * released; no device page is freed.
 */
bool paging_iounmap(void *pointer, uint64_t bytes);
/** @brief Switch later MMIO page-table splits to the physical allocator.
 * @return None. Call once after successful physical allocator initialization;
 * the early page-table reserve then remains pinned in the kernel image.
 */
void paging_mmio_allocator_ready(void);
/**
 * @brief Validate that a device range can be exposed with uncached semantics.
 * @param phys Physical start of the device range.
 * @param len Byte length of the range.
 * @return True when addressable and UC/NX page attributes are installed.
 * Task context under execution transaction/pre-SMP. Compatibility mappings
 * retain permanent attributes; repeated permanent claims are idempotent and
 * no mapping reference is transferred. Returns false if the bounded range
 * registry or page-table preparation cannot represent the request.
 */
bool paging_mmio_uncached(uint64_t phys, uint64_t len);
/** @brief Permanently pin an exact page-aligned default-cache device range WB/NX.
 * @param phys Page-aligned physical start.
 * @param len Page-aligned nonzero length.
 * @return True only when every page is a present writable supervisor identity
 * mapping with default PWT/PCD/PAT attributes and no conflicting UC/WB
 * interval exists. Task context after device quiescence; repeated identical
 * WB claims are idempotent.
 */
bool paging_mmio_writeback_nx(uint64_t phys, uint64_t len);
/** @brief Acquire one UC/NX reference per device page after BAR validation.
 * @param phys Page-aligned physical base.
 * @param len Page-aligned nonzero bytes.
 * @return True on success; each call owns one exact-range lease that must be
 * released after quiescence. False on an incompatible retained cache policy,
 * invalid input, NX absence, allocation, or bounded registry exhaustion.
 * Task context under execution transaction/
 * pre-SMP; flushes every CPU and preserves original flags for every page.
 */
bool paging_acquire_mmio(uint64_t phys, uint64_t len);
/** @brief Release owned device pages and restore last-reference attributes.
 * @param phys Exact aligned physical base passed to one successful acquire.
 * @param len Exact aligned byte length passed to that acquire.
 * @return None; unmatched ranges have no effect. Task context under execution
 * transaction/pre-SMP; TLB sync; page-table split storage remains kernel owned,
 * device RAM is never freed. Original attributes are restored only after the
 * last overlapping DEVICE cache-policy owner also retires.
 */
void paging_release_mmio(uint64_t phys, uint64_t len);

/** @brief Map an allocator-owned RAM page RW/NX in the shared supervisor module window.
 * @param address Page-aligned module virtual address.
 * @param physical Owned aligned RAM page below the direct-map limit.
 * @param original_alias Required output for original direct-map PTE flags.
 * @return True on success; task context, synchronizes CPUs, no RAM ownership transfer.
 */
bool paging_module_map_page(uint64_t address, uint64_t physical, uint64_t *original_alias);
/** @brief Query an existing supervisor module mapping under the shared-map lock.
 * @param address Aligned virtual page in the module window.
 * @param physical Optional output for physical backing.
 * @param flags Optional output for leaf flags excluding physical address.
 * @return True for a mapped page; no allocation or ownership transfer.
 */
bool paging_module_page(uint64_t address, uint64_t *physical, uint64_t *flags);
/** @brief Set module and alias permissions with a globally synchronized RO/NX transition.
 * @param address Aligned mapped module page; caller excludes execution during transitions.
 * @param flags Zero for RX, NOEXEC for RO/NX, WRITABLE|NOEXEC for RW/NX.
 * @return True on success; rejects USER/W+X/unknown flags without modification.
 */
bool paging_module_protect_page(uint64_t address, uint64_t flags);
/** @brief Remove a quiescent module leaf and reclaim empty private branch tables.
 * @param address Aligned module page with no external users or execution pins.
 * @param original_alias Flags returned by the matching successful map.
 * @return Removed physical page or zero; caller retains/frees RAM after TLB synchronization.
 */
uint64_t paging_module_unmap_page(uint64_t address, uint64_t original_alias);

/**
 * @brief Allocate and initialize an empty address space; true on success.
 */
bool address_space_create(struct address_space *as);
/**
 * @brief Clones a user address space using copy-on-write mappings.
 * @param source Existing user address space; writable pages become read-only COW mappings.
 * @param destination Zeroed output address space that receives independent page tables.
 * @return True on success; false after rolling back every destination mapping on failure.
 */
bool address_space_clone_cow(struct address_space *source, struct address_space *destination);
/** @brief Clone an unpublished fork MM and report ordinary allocation failures.
 * @param source Parent MM. @param destination Empty child MM.
 * @param allocation_exhausted Receives true only for a failed ordinary root,
 * table, alias-copy or native swap/shared metadata allocation before parent
 * COW protection. Failure destroys the child and leaves parent PTEs unchanged.
 * This hint carries no ownership and is not a userspace ABI.
 * @param failure_free_pages Receives total free pages at that specific failure.
 * @return True on success.
 */
bool address_space_clone_cow_fork(struct address_space *source, struct address_space *destination,
                                  bool *allocation_exhausted, uint64_t *failure_free_pages);
/** @brief Count physical child-table and staging pages and swap leaves for fork.
 * @param source Writer-pinned parent. @param pages Required table/copy pages.
 * @param swapped Required count of child swap records. @return False for an
 * invalid or unrepresentable snapshot; no allocation or reference is taken.
 */
bool address_space_fork_page_cost(const struct address_space *source,
                                  uint32_t *pages, uint64_t *swapped);
/**
 * @brief Free every page table and page owned by as.
 */
void address_space_destroy(struct address_space *as);
/**
 * @brief Prepare missing page tables atomically for a pinned user address space.
 * @param as Address space held under execution ownership.
 * @param start Inclusive first user byte. @param end Exclusive range end.
 * @return True after all tables are ready; false frees all newly allocated
 * tables without changing existing mappings.
 */
bool address_space_prepare_user_range(struct address_space *as, uint64_t start,
                                      uint64_t end);
/**
 * @brief Map a user page with VMA-validated independent write/NX permissions.
 * @param as Address space receiving the owned leaf mapping.
 * @param vaddr User address, aligned down to a page.
 * @param phys Aligned physical backing page; ownership transfers on success.
 * @param flags PTE protection/ownership flags; explicit W|X is permitted.
 * @return True on success; false for invalid addresses, NX absence, allocation
 *         failure or an existing mapping, leaving backing ownership to caller.
 */
bool address_space_map_user_page(struct address_space *as, uint64_t vaddr,
                                 uint64_t phys, uint64_t flags);
/** @brief Install a nonresident leaf after its slot owner has been prepared.
 * @param as Execution-owned address space. @param vaddr User byte address.
 * @param flags Validated leaf protections, excluding PRESENT and swap marker.
 * @return True when installed, false on invalid input, an occupied leaf or
 * table allocation failure. Caller owns and rolls back the slot record.
 */
bool address_space_map_user_swap(struct address_space *as, uint64_t vaddr,
                                 uint64_t flags);
/** @brief Write-protect one resident leaf and synchronize user TLBs.
 * @param as Execution-owned root. @param vaddr User byte address.
 * @return Original complete PTE, or zero when it is not a resident user leaf.
 * Caller retains the frame and restores or commits it before releasing writer.
 */
uint64_t paging_user_swap_freeze(struct address_space *as, uint64_t vaddr);
/** @brief Restore the permissions of an uncommitted swap snapshot.
 * @param as Execution-owned root. @param vaddr User byte address.
 * @param original PTE returned by paging_user_swap_freeze.
 * @return True when the same resident frame remains and was restored.
 */
bool paging_user_swap_restore(struct address_space *as, uint64_t vaddr, uint64_t original);
/** @brief Replace a frozen resident leaf with an already-owned slot marker.
 * @param as Execution-owned root. @param vaddr User byte address.
 * @param original PTE returned by paging_user_swap_freeze.
 * @return True after publication and TLB synchronization; caller then frees
 * the original frame. False leaves the resident mapping and its owner intact.
 */
bool paging_user_swap_commit(struct address_space *as, uint64_t vaddr, uint64_t original);
/** @brief Install a newly read page in place of a nonresident swap leaf.
 * @param as Execution-owned root. @param vaddr User byte address.
 * @param phys Owned aligned RAM page. @param writable Resolve a writable COW fault.
 * @return True on ownership transfer; false leaves the page with the caller.
 */
bool paging_user_swap_in(struct address_space *as, uint64_t vaddr, uint64_t phys,
                         bool writable);
/** @brief Materialize a swap leaf without changing its protection or COW state.
 * @param as Execution-owned root. @param vaddr User byte address.
 * @param phys Owned aligned RAM page. @return True on ownership transfer;
 * false leaves the page and the original swap marker with their callers.
 */
bool paging_user_swap_materialize(struct address_space *as, uint64_t vaddr,
                                  uint64_t phys);
/**
 * @brief Replace user protection flags while preserving backing ownership/COW.
 * @param as Address space containing the existing backed leaf.
 * @param vaddr User address within the mapping.
 * @param flags VMA-validated protection bits with independent write/NX state.
 * @return True on success; false for invalid or unbacked addresses.
 */
bool address_space_protect_user_page(struct address_space *as, uint64_t vaddr,
                                     uint64_t flags);
/**
 * @brief Remove the user mapping at vaddr and return the physical page it held.
 */
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t vaddr);
/**
 * @brief Return the physical address backing user vaddr, or 0 if unmapped.
 */
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t vaddr);

/* Embedded in a private owner that will write an original physical page
 * after a wait. The execution writer serializes registration and fork. */
struct paging_writable_alias {
    struct paging_writable_alias *next;
    uint64_t physical;
};
/** @brief Register an independently retained writable physical alias.
 * @param alias Empty owner-embedded node, valid until removed.
 * @param physical Aligned page whose backing the caller already owns.
 * Execution writer required. No allocation, I/O or backing reference occurs.
 */
void paging_writable_alias_add(struct paging_writable_alias *alias,uint64_t physical);
/** @brief Remove an owner's registered alias before releasing its backing.
 * @param alias Previously registered owner-embedded node.
 * Execution writer required; removal performs no allocation, I/O or puts.
 */
void paging_writable_alias_remove(struct paging_writable_alias *alias);
bool address_space_user_page_readable(const struct address_space *as, uint64_t vaddr);
/**
 * @brief Return the raw software and hardware flags for a user page mapping.
 * @param as Address space whose leaf page table is queried.
 * @param vaddr Address within the queried page.
 * @return Leaf flags, or zero if the address is unmapped or outside user range.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t vaddr);
/**
 * @brief Find the first backed user leaf in a sparse virtual range.
 * @param as Address space whose owned page tables are searched.
 * @param start Inclusive low-canonical address; a reserved supervisor hole is skipped.
 * @param end Exclusive range end, no greater than RELIEFNT_USER_TOP.
 * @param out_page Required output for the page-aligned virtual address.
 * @param out_phys Optional output for the physical backing page.
 * @param out_flags Optional output for the complete PTE flags.
 * @return True when a backed user leaf exists; false for an empty or invalid range.
 * The walker visits only present owned table branches and backed leaves.
 */
bool address_space_user_page_next(const struct address_space *as, uint64_t start,
                                  uint64_t end, uint64_t *out_page,
                                  uint64_t *out_phys, uint64_t *out_flags);
/** @brief Find the first resident or swapped user leaf in a sparse interval.
 * @param as Address space held stable by the caller.
 * @param start Inclusive user address. @param end Exclusive user address.
 * @param out_page Required output for the aligned leaf address.
 * @param out_phys Optional physical output, set to zero for a swap leaf.
 * @param out_flags Optional output for the leaf's software/hardware flags.
 * @return True when any owned user leaf was found, false on absence or invalid
 * input. No references are acquired, and a swap marker is never used as a PFN.
 */
bool address_space_user_entry_next(const struct address_space *as, uint64_t start,
                                   uint64_t end, uint64_t *out_page,
                                   uint64_t *out_phys, uint64_t *out_flags);
/**
 * @brief Test whether a valid user interval has no backed page-table leaves.
 * @param as Address space whose owned page tables are searched.
 * @param start Inclusive low-canonical user byte address.
 * @param end Exclusive range end, no greater than RELIEFNT_USER_TOP.
 * @return True only for a valid interval with no backed user leaf.
 */
bool address_space_user_range_free(const struct address_space *as, uint64_t start,
                                   uint64_t end);
/**
 * Retry a non-present fault if another CPU has since installed a user PTE
 * with the permissions required by the faulting access.
 */
bool address_space_retry_mapped_user_page(const struct address_space *as,
                                          uint64_t vaddr, uint64_t fault_error);
/**
 * @brief Check a present user mapping's write permission without resolving COW.
 * @param as Address space, or NULL for an invalid mapping.
 * @param vaddr Address within the queried page.
 * @return True for a present user leaf with hardware write permission or a
 * shared-cache first-write notification; the latter is hardware read-only.
 * Caller stabilizes the address space. No COW resolution or pin is performed.
 */
bool address_space_user_page_writable(const struct address_space *as, uint64_t vaddr);

/** @brief Complete an admitted shared-cache write or stale-translation retry.
 * @param as Address space stabilized by the execution writer, nullable.
 * @param vaddr Faulting user byte address in a currently writable shared-file VMA.
 * @return True after dirty-before-writable publication or local invalidation of
 * an already-writable cache leaf; false for an ineligible branch, leaf or frame.
 * Caller validates the current VMA and ordinary present/write fault. PTE pins the
 * frame; cache lookup/notification uses its IRQ-safe lock. No allocation,
 * provider I/O, sleep, reference transfer or remote TLB flush is performed.
 */
bool address_space_shared_write_fault(struct address_space *as, uint64_t vaddr);
bool address_space_user_page_is_device(const struct address_space *as, uint64_t vaddr);
/**
 * @brief Report the address space's accounted user page mappings.
 * @param as Stable address space to inspect; may be NULL.
 * @return Mapped page count in KiB, or zero for NULL.
 */
uint64_t address_space_user_memory_kib(const struct address_space *as);
/**
 * @brief Count resident user RAM mappings, excluding borrowed device PFNs.
 * @param as Address space stable under the execution lock; may be NULL.
 * @return Backed user RAM mappings in KiB, including PROT_NONE backing; zero
 * for an absent address space. Shared pages count once per mapping.
 */
uint64_t address_space_user_resident_kib(const struct address_space *as);
/**
 * @brief Map the initial user stack pages ending at stack_top; true on success.
 */
bool address_space_map_user_stack(struct address_space *as, uint64_t stack_top);
bool address_space_map_user_stack_page(struct address_space *as, uint64_t page);
/**
 * @brief Resolves a write protection fault on a copy-on-write user page.
 * @param as Faulting process address space.
 * @param vaddr User virtual address that raised the write fault.
 * @return True when the page was made private and writable, otherwise false.
 */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t vaddr);

#endif
