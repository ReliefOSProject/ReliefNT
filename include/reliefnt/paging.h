/*
 * ReliefOS paging interface: defines address-space and user-page operations.
 * Exposes page mapping, protection, unmapping, and memory-layout constants.
 */
#ifndef RELIEFNT_PAGING_H
#define RELIEFNT_PAGING_H

#include <reliefnt/types.h>

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
/* Every address space retains this supervisor-only alias of the kernel's
 * first 16 GiB physical direct map. Kernel physical pointers must go through
 * this alias because sparse user mappings can replace low identity leaves. */
#ifndef RELIEFNT_KERNEL_DIRECT_MAP_BASE
#define RELIEFNT_KERNEL_DIRECT_MAP_BASE 0xffffff8000000000ULL
#endif
#ifndef RELIEFNT_KERNEL_DIRECT_MAP_SIZE
#define RELIEFNT_KERNEL_DIRECT_MAP_SIZE (16ULL * 1024ULL * 1024ULL * 1024ULL)
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
#define RELIEFNT_PAGE_PAT 0x080ULL
/* x86_64 makes bits 9-11 of a present PTE available to software.  A COW
 * mapping is deliberately read-only; the page-fault handler copies it before
 * restoring write permission for the faulting address space. */
#define RELIEFNT_PAGE_COW 0x200ULL
#define RELIEFNT_PAGE_DEVICE 0x400ULL
/* Software-only bit outside the physical address field. Shared RAM is owned,
 * unlike borrowed device pages, and must not become COW during fork. */
#define RELIEFNT_PAGE_SHARED (1ULL << 52)
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

/**
 * @brief Build boot identity mappings, the supervisor direct map and the user kernel-hole template.
 */
void paging_init_user_identity(void);
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
/**
 * @brief Switch the CPU's page table to the root given by cr3.
 */
void paging_load_cr3(uint64_t cr3);
/**
 * @brief Reload CR3 on return-to-user only when the address space changed since the last load.
 */
void paging_load_cr3_if_changed(uint64_t cr3);
/**
 * @brief Record that a mapping changed so the next user return reloads CR3.
 */
void paging_note_mm_change(void);
/**
 * @brief Current mm-change sequence, for the conditional CR3 reload check.
 */
uint64_t paging_mm_change_seq(void);
/**
 * @brief Return true when [phys, phys + len) is reachable through the shared kernel direct map.
 */
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len);
/**
 * @brief Translate a physical address to the supervisor-only shared kernel direct map, or NULL.
 */
void *paging_kernel_direct_map(uint64_t phys);
/** @brief Map validated high PCI BAR pages outside the RAM/module virtual windows.
 * @param physical Page-aligned device base at or beyond the direct-map extent.
 * @param bytes Nonzero page-aligned device length, entirely representable by PTEs.
 * @return Owned supervisor UC/RW/NX mapping or NULL, shared by existing and future
 * process CR3s. Task context after heap/MM initialization, no physical RAM ownership.
 * Caller validates the BAR and drains device/users before exact unmap.
 */
void *paging_ioremap_uc(uint64_t physical, uint64_t bytes);
/** @brief Remove one exact owned high PCI BAR mapping after device/users stop.
 * @param pointer Base returned by paging_ioremap_uc; other/stale bases rejected.
 * @param bytes Original full page-aligned mapping length.
 * @return True if retired, false for an unknown base/length. Task context, all CPU
 * TLBs synchronized before owned table pages are freed; no device page is freed.
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
 * released after quiescence. False on invalid input, NX absence, allocation,
 * or bounded registry exhaustion. Task context under execution transaction/
 * pre-SMP; flushes every CPU and preserves original flags for every page.
 */
bool paging_acquire_mmio(uint64_t phys, uint64_t len);
/** @brief Release owned device pages and restore last-reference attributes.
 * @param phys Exact aligned physical base passed to one successful acquire.
 * @param len Exact aligned byte length passed to that acquire.
 * @return None; unmatched ranges have no effect. Task context under execution
 * transaction/pre-SMP; TLB sync; page-table split storage remains kernel owned,
 * device RAM is never freed.
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
 * @return True only for a present, user-accessible, writable page.
 */
bool address_space_user_page_writable(const struct address_space *as, uint64_t vaddr);
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
