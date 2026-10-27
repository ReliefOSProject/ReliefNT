#include <reliefnt/export.h>
/*
 * ReliefOS x86_64 paging: builds kernel and per-process address spaces.
 * Maps user pages, enforces NX/W^X permissions, and handles page protection.
 */
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/smp.h>
#include <reliefnt/lock.h>
#include <reliefnt/heap.h>
#include "internal.h"

#define PAGE_SIZE 4096ULL
#define PAGE_SIZE_2M 0x200000ULL
#define PAGE_SIZE_FLAG 0x080ULL
#define USER_PDPT_INDEX 0
#define LOW_PD_INDEX 0
#define KERNEL_DIRECT_PML4_INDEX 511u
#define KERNEL_PD_COUNT 16u /* 16 GiB identity map using 2 MiB leaves. */
#define MMIO_EARLY_PT_TABLE_COUNT 1024u
#define X86_EFER_MSR 0xc0000080u
#define X86_EFER_NXE (1ULL << 11)

static uint64_t kernel_pml4[512] __attribute__((aligned(4096)));
static uint64_t kernel_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kernel_pd[KERNEL_PD_COUNT][512] __attribute__((aligned(4096)));
/* User CR3s keep only the supervisor low-identity branch containing the
 * physically linked kernel image; all other low identity leaves are omitted. */
static uint64_t user_kernel_pdpt[512] __attribute__((aligned(4096)));
static uint64_t user_kernel_pd[512] __attribute__((aligned(4096)));
static uint64_t kernel_hole_pml4e;
/* Before mm_init, framebuffer BAR setup can split up to 514 huge PDEs for
 * independent unaligned 512 MiB VRAM and FIFO ranges. 1024 pinned image pages
 * also leave room for the boot scanout aperture; successful mm_init hands
 * subsequent splits to the ordinary physical allocator. */
static uint64_t mmio_early_pt_tables[MMIO_EARLY_PT_TABLE_COUNT][512]
    __attribute__((aligned(4096)));
static uint32_t mmio_early_pt_table_count;
static bool mmio_dynamic_tables_ready;

extern void x86_64_load_cr3(uint64_t cr3);
extern void x86_64_invlpg(uint64_t addr);

/* All first-GiB kernel leaves are split before any user CR3 copies its PDEs.
 * Later BAR attribute changes remain visible through every copied low PDE. */
static uint64_t kernel_low_pt[512][512] __attribute__((aligned(4096)));
#define MMIO_RUN_MAX 256U
#define MMIO_LEASE_MAX 128U
enum mmio_cache_policy {
    MMIO_CACHE_POLICY_UC = 1,
    MMIO_CACHE_POLICY_WB = 2,
};
struct mmio_run {
    uint64_t start, end, original_flags;
    uint32_t refs, permanent, policy;
};
struct mmio_lease { uint64_t start, end; uint32_t refs; };
static struct mmio_run mmio_runs[MMIO_RUN_MAX], mmio_work[MMIO_RUN_MAX];
static struct mmio_lease mmio_leases[MMIO_LEASE_MAX], mmio_lease_work[MMIO_LEASE_MAX];
static uint32_t mmio_run_count, mmio_lease_count;
static struct kernel_spinlock mmio_lock = KERNEL_SPINLOCK_INIT;
static bool nx_enabled;

/**
 * @brief Copies one physical page without relying on the user virtual mapping.
 * @param destination Allocated physical destination page.
 * @param source Existing physical source page.
 */
static void copy_page(uint64_t destination, uint64_t source)
{
    uint8_t *dst = (uint8_t *)mm_phys_to_virt(destination);
    const uint8_t *src = (const uint8_t *)mm_phys_to_virt(source);
    if (!dst || !src) return;
    for (uint32_t i = 0; i < PAGE_SIZE; ++i) {
        dst[i] = src[i];
    }
}

/**
 * Paging enable nx.
 */
static void paging_enable_nx(void)
{
    uint32_t max_extended = 0;
    uint32_t regs[4];
    __asm__ volatile("cpuid"
                     : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(0x80000000u), "c"(0));
    max_extended = regs[0];
    if (max_extended < 0x80000001u) {
        return;
    }
    __asm__ volatile("cpuid"
                     : "=a"(regs[0]), "=b"(regs[1]), "=c"(regs[2]), "=d"(regs[3])
                     : "a"(0x80000001u), "c"(0));
    if (!(regs[3] & (1u << 20))) {
        return;
    }
    uint32_t lo;
    uint32_t hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(X86_EFER_MSR));
    uint64_t efer = ((uint64_t)hi << 32) | lo;
    efer |= X86_EFER_NXE;
    lo = (uint32_t)efer;
    hi = (uint32_t)(efer >> 32);
    __asm__ volatile("wrmsr" : : "c"(X86_EFER_MSR), "a"(lo), "d"(hi));
    nx_enabled = true;
}

/**
 * Align down.
 * @param value Value supplied by the caller.
 * @param align Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint64_t align_down(uint64_t value, uint64_t align)
{
    return value & ~(align - 1);
}

/**
 * Zero table.
 * @param table Value supplied by the caller.
 */
static void zero_table(uint64_t *table)
{
    for (uint32_t i = 0; i < 512; ++i) {
        table[i] = 0;
    }
}

/**
 * @brief Returns the flags used for identity-mapped kernel pages.
 * @param addr Virtual address being mapped; currently reserved for future policy.
 * @return Present, writable, large-page mapping flags.
 */
static uint64_t kernel_page_flags_for(uint64_t addr)
{
    (void)addr;
    return RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE | PAGE_SIZE_FLAG;
}

/**
 * Paging init user identity.
 */
void paging_init_user_identity(void)
{
    paging_enable_nx();
    zero_table(kernel_pml4);
    zero_table(kernel_pdpt);

    for (uint64_t table = 0; table < KERNEL_PD_COUNT; ++table) {
        for (uint64_t i = 0; i < 512; ++i) {
            uint64_t addr = (table << 30) + i * PAGE_SIZE_2M;
            uint64_t flags = kernel_page_flags_for(addr);
            kernel_pd[table][i] = flags ? addr | flags : 0;
            if (!table) {
                for (uint32_t page = 0; page < 512; ++page)
                    kernel_low_pt[i][page] = (addr + page * PAGE_SIZE) | (flags & ~PAGE_SIZE_FLAG);
                kernel_pd[table][i] = (uint64_t)(uintptr_t)kernel_low_pt[i] |
                    RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
            }
        }
    }

    kernel_pml4[0] = (uint64_t)(uintptr_t)kernel_pdpt | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    /* User address spaces replace portions of PML4[0]'s low identity map.
     * Keep a second supervisor-only view at a canonical high address so
     * kernel code can safely read reserved Multiboot modules regardless of
     * where GRUB placed them in low physical memory. */
    kernel_pml4[KERNEL_DIRECT_PML4_INDEX] =
        (uint64_t)(uintptr_t)kernel_pdpt | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    for (uint64_t i = 0; i < KERNEL_PD_COUNT; ++i) {
        kernel_pdpt[i] = (uint64_t)(uintptr_t)kernel_pd[i] | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    }

    /* Kernel text/data remain linked in the low physical window. User roots
     * need that exact supervisor mapping while executing syscalls/interrupts,
     * but inheriting the full low identity map collides with lazy user VMAs.
     * Build a template that preserves only the explicitly reserved kernel
     * hole; every other low-half slot remains available for user mappings. */
    zero_table(user_kernel_pdpt);
    zero_table(user_kernel_pd);
    for (uint32_t pde = (uint32_t)(RELIEFNT_KERNEL_HOLE_START >> 21);
         pde < (uint32_t)(RELIEFNT_KERNEL_HOLE_END >> 21); ++pde) {
        user_kernel_pd[pde] = kernel_pd[0][pde];
    }
    user_kernel_pdpt[0] = (uint64_t)(uintptr_t)user_kernel_pd |
                          RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;
    kernel_hole_pml4e = (uint64_t)(uintptr_t)user_kernel_pdpt |
                        RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE;

    x86_64_load_cr3((uint64_t)(uintptr_t)kernel_pml4);
}

void paging_init_cpu(void)
{
    paging_enable_nx();
}

/**
 * Paging kernel cr3.
 * @return The value or status produced by the operation.
 */
uint64_t paging_kernel_cr3(void)
{
    return (uint64_t)(uintptr_t)kernel_pml4;
}

/**
 * Paging load cr3.
 * @param cr3 Value supplied by the caller.
 */
static volatile uint64_t mm_change_seq;
static uint64_t cr3_loaded[SMP_MAX_CPUS];
static uint64_t cr3_loaded_seq[SMP_MAX_CPUS];

void paging_load_cr3(uint64_t cr3)
{
    uint64_t value = cr3 ? cr3 : paging_kernel_cr3();
    x86_64_load_cr3(value);
    uint32_t cpu = smp_current_cpu();
    cr3_loaded[cpu] = value;
    cr3_loaded_seq[cpu] = mm_change_seq;
}

/**
 * @brief Record that a mapping changed so the next user return reloads CR3.
 */
void paging_note_mm_change(void)
{
    ++mm_change_seq;
}

/**
 * @brief Current mm-change sequence, for the conditional CR3 reload check.
 */
uint64_t paging_mm_change_seq(void)
{
    return mm_change_seq;
}

/**
 * @brief Reload CR3 on return-to-user only when the address space changed.
 *
 * The per-page invlpg at each mapping site already drops the specific stale
 * translation; a return to the same task that mapped nothing keeps its live
 * TLB.  The sequence check reloads conservatively after any mapping change,
 * preserving the full non-global flush the lazy loader and munmap rely on.
 */
void paging_load_cr3_if_changed(uint64_t cr3)
{
    uint64_t value = cr3 ? cr3 : paging_kernel_cr3();
    uint32_t cpu = smp_current_cpu();
    if (value == cr3_loaded[cpu] && mm_change_seq == cr3_loaded_seq[cpu])
        return;
    paging_load_cr3(value);
}

/** @brief Check a physical extent against the inherited supervisor RAM window.
 * @param phys Physical start, not a CPU pointer. @param len Bytes including zero.
 * @return True iff the complete extent fits, without overflow. Pure metadata
 * check; does not prove allocation/RAM ownership or admit high MMIO mappings.
 */
bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    return phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE &&
           len <= RELIEFNT_KERNEL_DIRECT_MAP_SIZE - phys;
}

/** @brief Translate an admitted physical byte into the shared supervisor window.
 * @param phys Physical address, not an arbitrary CPU or MMIO pointer.
 * @return Borrowed mapping or NULL outside the window; no allocation/reference
 * or page ownership transfer. Caller owns physical backing for all accesses.
 */
void *paging_kernel_direct_map(uint64_t phys)
{
    if (!paging_kernel_direct_map_range(phys, 1)) {
        return NULL;
    }
    return (void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + phys);
}

/** @brief Make future MMIO splits use allocator-owned table pages.
 * @return None. Boot calls this only after mm_init has initialized its bitmap.
 * Existing early tables remain pinned in the kernel image for every CR3.
 */
void paging_mmio_allocator_ready(void)
{
    mmio_dynamic_tables_ready = true;
}

/** @brief Allocate a split table from pinned early BSS or the physical allocator.
 * @param physical Required output for the table page's physical address.
 * @return Kernel direct-map pointer, or NULL if the selected source is exhausted.
 * The early reserve is never returned to the allocator; its storage is part of
 * the image range reserved by mm_init. Dynamic allocation begins only after
 * the one-way readiness handoff.
 */
static uint64_t *mmio_allocate_split_table(uint64_t *physical)
{
    uint64_t page;
    uint64_t *table;
    if (mmio_dynamic_tables_ready) {
        page = mm_alloc_page();
        if (!page) return NULL;
        table = (uint64_t *)mm_phys_to_virt(page);
        if (!table) {
            mm_free_page(page);
            return NULL;
        }
    } else {
        if (mmio_early_pt_table_count >= MMIO_EARLY_PT_TABLE_COUNT) return NULL;
        page = (uint64_t)(uintptr_t)mmio_early_pt_tables[mmio_early_pt_table_count];
        table = (uint64_t *)mm_phys_to_virt(page);
        if (!table) return NULL;
        ++mmio_early_pt_table_count;
    }
    *physical = page;
    return table;
}

/** @brief Obtain a shared 4 KiB kernel leaf, splitting only its original PDE.
 * @param phys Physical address in the kernel map.
 * @return Borrowed PTE pointer, or NULL on allocation failure. mmio_lock held.
 * Newly split page tables belong to the kernel map until shutdown, not a BAR.
 */
static uint64_t *mmio_leaf(uint64_t phys)
{
    uint64_t *pde = &kernel_pd[phys >> 30][(phys >> 21) & 511];
    if (*pde & PAGE_SIZE_FLAG) {
        uint64_t page;
        uint64_t *pt = mmio_allocate_split_table(&page);
        if (!pt) return NULL;
        uint64_t base = *pde & RELIEFNT_PHYS_ADDR_MASK & ~(PAGE_SIZE_2M - 1);
        uint64_t flags = *pde & ~RELIEFNT_PHYS_ADDR_MASK & ~PAGE_SIZE_FLAG;
        /* Large-page PAT is bit 12; in a 4 KiB PTE it is bit 7. */
        if (*pde & (1ULL << 12)) flags |= RELIEFNT_PAGE_PAT;
        for (uint32_t i = 0; i < 512; ++i) pt[i] = (base + i * PAGE_SIZE) | flags;
        __atomic_store_n(pde, page | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE, __ATOMIC_RELEASE);
    }
    if (!(*pde & RELIEFNT_PAGE_PRESENT)) return NULL;
    uint64_t *pt = (uint64_t *)mm_phys_to_virt(*pde & RELIEFNT_PHYS_ADDR_MASK);
    return pt ? &pt[(phys >> 12) & 511] : NULL;
}
/** @brief Return a leaf only when its PDE has already been split.
 * @param phys Physical address in the kernel direct map.
 * @return Borrowed leaf PTE or NULL. mmio_lock held; does not allocate.
 */
static uint64_t *mmio_existing_leaf(uint64_t phys)
{
    uint64_t *pde = &kernel_pd[phys >> 30][(phys >> 21) & 511];
    if (!(*pde & RELIEFNT_PAGE_PRESENT) || (*pde & PAGE_SIZE_FLAG)) return NULL;
    uint64_t *pt = (uint64_t *)mm_phys_to_virt(*pde & RELIEFNT_PHYS_ADDR_MASK);
    return pt ? &pt[(phys >> 12) & 511] : NULL;
}

/** @brief Append a sorted run, coalescing only identical adjacent states.
 * @param run Sorted candidate record.
 * @return True if the bounded workspace had room.
 */
static bool mmio_run_append(const struct mmio_run *run, uint32_t *count)
{
    if (*count && mmio_work[*count - 1].end == run->start) {
        struct mmio_run *previous = &mmio_work[*count - 1];
        if (previous->original_flags == run->original_flags &&
            previous->refs == run->refs && previous->permanent == run->permanent &&
            previous->policy == run->policy) {
            previous->end = run->end;
            return true;
        }
    }
    if (*count >= MMIO_RUN_MAX) return false;
    mmio_work[(*count)++] = *run;
    return true;
}

/** @brief Add an unowned page-table gap as runs of equal original PTE flags.
 * @param start Aligned inclusive physical address.
 * @param end Aligned exclusive physical address.
 * @param permanent Bootstrap claim state.
 * @param count Candidate run count, updated on success.
 * @return True if every run fit. Page-table leaves were preflighted already.
 */
static bool mmio_append_new_range(uint64_t start, uint64_t end, bool permanent,
                                  enum mmio_cache_policy policy, uint32_t *count)
{
    uint64_t run_start = start, original_flags = 0;
    bool have_run = false;
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t *pte = mmio_existing_leaf(p);
        if (!pte) return false;
        uint64_t flags = *pte & ~RELIEFNT_PHYS_ADDR_MASK;
        if (policy == MMIO_CACHE_POLICY_WB) {
            if (!(flags & RELIEFNT_PAGE_PRESENT) ||
                !(flags & RELIEFNT_PAGE_WRITABLE) ||
                (flags & RELIEFNT_PAGE_USER) ||
                (*pte & RELIEFNT_PHYS_ADDR_MASK) != p ||
                (flags & (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT)))
                return false;
        }
        if (have_run && flags != original_flags) {
            struct mmio_run run = {run_start, p, original_flags,
                                   permanent ? 0u : 1u, permanent ? 1u : 0u,
                                   (uint32_t)policy};
            if (!mmio_run_append(&run, count)) return false;
            run_start = p;
            original_flags = flags;
        } else if (!have_run) {
            run_start = p;
            original_flags = flags;
            have_run = true;
        }
    }
    if (have_run) {
        struct mmio_run run = {run_start, end, original_flags,
                               permanent ? 0u : 1u, permanent ? 1u : 0u,
                               (uint32_t)policy};
        if (!mmio_run_append(&run, count)) return false;
    }
    return true;
}

/** @brief Build an acquisition result without changing the live registry.
 * @param start Inclusive aligned range start.
 * @param end Exclusive aligned range end.
 * @param permanent Whether this is an idempotent permanent bootstrap claim.
 * @param count Output candidate run count.
 * @return False on reference or interval-capacity exhaustion.
 */
static bool mmio_plan_acquire(uint64_t start, uint64_t end, bool permanent,
                              enum mmio_cache_policy policy,
                              uint32_t *count)
{
    uint32_t out = 0;
    uint64_t cursor = start;
    for (uint32_t i = 0; i < mmio_run_count; ++i) {
        const struct mmio_run *old = &mmio_runs[i];
        if (old->end <= start || old->start >= end) {
            if (old->start >= end && cursor < end) {
                if (!mmio_append_new_range(cursor, end, permanent, policy, &out)) return false;
                cursor = end;
            }
            if (!mmio_run_append(old, &out)) return false;
            continue;
        }

        uint64_t first = old->start < start ? start : old->start;
        uint64_t last = old->end > end ? end : old->end;
        if (cursor < first) {
            if (!mmio_append_new_range(cursor, first, permanent, policy, &out)) return false;
        }
        if (old->start < first) {
            struct mmio_run prefix = *old;
            prefix.end = first;
            if (!mmio_run_append(&prefix, &out)) return false;
        }

        struct mmio_run overlap = *old;
        overlap.start = first;
        overlap.end = last;
        if (overlap.policy != (uint32_t)policy) return false;
        if (permanent) {
            overlap.permanent = 1;
        } else {
            if (overlap.refs == UINT32_MAX) return false;
            ++overlap.refs;
        }
        if (!mmio_run_append(&overlap, &out)) return false;

        if (last < old->end) {
            struct mmio_run suffix = *old;
            suffix.start = last;
            if (!mmio_run_append(&suffix, &out)) return false;
        }
        cursor = last;
    }
    if (cursor < end && !mmio_append_new_range(cursor, end, permanent, policy, &out)) return false;
    *count = out;
    return true;
}

/** @brief Build a release result without changing the live registry.
 * @param start Inclusive aligned range start.
 * @param end Exclusive aligned range end.
 * @param count Output candidate run count.
 * @return False only when the bounded workspace cannot represent the result.
 */
static bool mmio_plan_release(uint64_t start, uint64_t end, uint32_t *count)
{
    uint32_t out = 0;
    for (uint32_t i = 0; i < mmio_run_count; ++i) {
        const struct mmio_run *old = &mmio_runs[i];
        if (old->end <= start || old->start >= end) {
            if (!mmio_run_append(old, &out)) return false;
            continue;
        }

        uint64_t first = old->start < start ? start : old->start;
        uint64_t last = old->end > end ? end : old->end;
        if (old->start < first) {
            struct mmio_run prefix = *old;
            prefix.end = first;
            if (!mmio_run_append(&prefix, &out)) return false;
        }
        struct mmio_run overlap = *old;
        overlap.start = first;
        overlap.end = last;
        if (overlap.refs) --overlap.refs;
        if (overlap.refs || overlap.permanent) {
            if (!mmio_run_append(&overlap, &out)) return false;
        }
        if (last < old->end) {
            struct mmio_run suffix = *old;
            suffix.start = last;
            if (!mmio_run_append(&suffix, &out)) return false;
        }
    }
    *count = out;
    return true;
}

/** @brief Prepare a private lease-table update before PTE attributes change.
 * @param start Inclusive aligned range start.
 * @param end Exclusive aligned range end.
 * @param acquire True to add a lease; false to release its exact matching range.
 * @param count Output candidate lease count.
 * @return False on capacity/refcount exhaustion or unmatched release.
 */
static bool mmio_plan_lease(uint64_t start, uint64_t end, bool acquire,
                            uint32_t *count)
{
    for (uint32_t i = 0; i < mmio_lease_count; ++i) mmio_lease_work[i] = mmio_leases[i];
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < mmio_lease_count; ++i)
        if (mmio_lease_work[i].start == start && mmio_lease_work[i].end == end) {
            found = i;
            break;
        }
    uint32_t out = mmio_lease_count;
    if (acquire) {
        if (found != UINT32_MAX) {
            if (mmio_lease_work[found].refs == UINT32_MAX) return false;
            ++mmio_lease_work[found].refs;
        } else {
            if (out >= MMIO_LEASE_MAX) return false;
            mmio_lease_work[out++] = (struct mmio_lease){start, end, 1};
        }
    } else {
        if (found == UINT32_MAX) return false;
        if (--mmio_lease_work[found].refs == 0) {
            for (uint32_t i = found + 1; i < out; ++i)
                mmio_lease_work[i - 1] = mmio_lease_work[i];
            --out;
        }
    }
    *count = out;
    return true;
}

/** @brief Count unique live lease endpoints that would split an existing run.
 * @param runs Candidate sorted run table.
 * @param run_count Candidate run count.
 * @param leases Candidate live lease table.
 * @param lease_count Candidate live lease count.
 * @return Number of additional run slots needed to represent all endpoints.
 */
static uint32_t mmio_missing_boundaries(const struct mmio_run *runs, uint32_t run_count,
                                        const struct mmio_lease *leases, uint32_t lease_count)
{
    uint32_t missing = 0;
    for (uint32_t i = 0; i < lease_count; ++i) {
        uint64_t endpoints[2] = {leases[i].start, leases[i].end};
        for (uint32_t side = 0; side < 2; ++side) {
            uint64_t point = endpoints[side];
            bool inside = false, seen = false;
            for (uint32_t r = 0; r < run_count; ++r) {
                if (point == runs[r].start || point == runs[r].end) break;
                if (point > runs[r].start && point < runs[r].end) { inside = true; break; }
                if (runs[r].start > point) break;
            }
            if (!inside) continue;
            for (uint32_t j = 0; j <= i && !seen; ++j) {
                uint32_t limit = j == i ? side : 2;
                for (uint32_t previous = 0; previous < limit; ++previous) {
                    uint64_t value = previous == 0 ? leases[j].start : leases[j].end;
                    if (value == point) { seen = true; break; }
                }
            }
            if (!seen) ++missing;
        }
    }
    return missing;
}

/** @brief Check whether candidate runs plus reserved lease split points fit.
 * @return True if every currently live lease can still be released safely.
 */
static bool mmio_registry_fits(uint32_t run_count, uint32_t lease_count)
{
    if (run_count > MMIO_RUN_MAX) return false;
    uint32_t missing = mmio_missing_boundaries(mmio_work, run_count,
                                                mmio_lease_work, lease_count);
    return missing <= MMIO_RUN_MAX - run_count;
}

/** @brief Validate a page-aligned range without allowing end-address wrap.
 */
static bool mmio_valid_aligned_range(uint64_t phys, uint64_t len)
{
    return len && !(phys & (PAGE_SIZE - 1)) && !(len & (PAGE_SIZE - 1)) &&
           len <= UINT64_MAX - phys && paging_kernel_direct_map_range(phys, len);
}

/** @brief Verify that every leaf in a range is already allocated.
 * @param start Inclusive aligned range start.
 * @param end Exclusive aligned range end.
 * @return True when no later PTE update can allocate or fail.
 */
static bool mmio_verify_leaves(uint64_t start, uint64_t end)
{
    for (uint64_t p = start; p < end; p += PAGE_SIZE)
        if (!mmio_existing_leaf(p)) return false;
    return true;
}

/** @brief Set precise device-page UC/NX attributes and claim mapping references.
 * @param phys Page-aligned physical base; BAR validation belongs to PCI service.
 * @param len Page-aligned nonzero length, bounded to the shared direct map.
 * @param permanent Nonzero for bootstrap compatibility mappings.
 * @return True on success, false on malformed range/capacity/ENOMEM/NX absence.
 * Task context, execution transaction or pre-SMP boot; never from IRQ. The
 * sorted interval registry preserves original flags and coalesces identical
 * runs; page-table splits may remain after a failed preflight.
 */
static bool mmio_acquire(uint64_t phys, uint64_t len, bool permanent,
                         enum mmio_cache_policy policy)
{
    if (!nx_enabled || !mmio_valid_aligned_range(phys, len)) return false;
    uint64_t end = phys + len;
    kernel_spin_lock(&mmio_lock);
    uint32_t lease_count = mmio_lease_count;
    if (permanent) {
        for (uint32_t i = 0; i < mmio_lease_count; ++i) mmio_lease_work[i] = mmio_leases[i];
    } else if (!mmio_plan_lease(phys, end, true, &lease_count)) {
        kernel_spin_unlock(&mmio_lock);
        return false;
    }

    /* Split all required PDEs before recording ownership or changing flags. */
    for (uint64_t p = phys; p < end; p += PAGE_SIZE) if (!mmio_leaf(p)) {
        smp_flush_user_tlb();
        kernel_spin_unlock(&mmio_lock);
        return false;
    }
    uint32_t run_count = 0;
    if (!mmio_plan_acquire(phys, end, permanent, policy, &run_count) ||
        !mmio_registry_fits(run_count, lease_count) || !mmio_verify_leaves(phys, end)) {
        smp_flush_user_tlb();
        kernel_spin_unlock(&mmio_lock);
        return false;
    }

    for (uint64_t p = phys; p < end; p += PAGE_SIZE) {
        uint64_t *pte = mmio_existing_leaf(p);
        if (policy == MMIO_CACHE_POLICY_UC)
            *pte = (*pte & ~RELIEFNT_PAGE_PAT) | RELIEFNT_PAGE_PWT |
                   RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC;
        else
            *pte |= RELIEFNT_PAGE_NOEXEC;
    }
    for (uint32_t i = 0; i < run_count; ++i) mmio_runs[i] = mmio_work[i];
    mmio_run_count = run_count;
    if (!permanent) {
        for (uint32_t i = 0; i < lease_count; ++i) mmio_leases[i] = mmio_lease_work[i];
        mmio_lease_count = lease_count;
    }
    smp_flush_user_tlb();
    kernel_spin_unlock(&mmio_lock);
    return true;
}
/** @brief Claim a precise shared UC/NX device mapping.
 * @param phys Aligned physical device base, validated against PCI BAR by caller.
 * @param len Aligned mapping length.
 * @return True on success. Task context under execution transaction/pre-SMP;
 * caller owns one reference per page and must release it after device quiescence.
 */
bool paging_acquire_mmio(uint64_t phys, uint64_t len)
{
    return mmio_acquire(phys, len, false, MMIO_CACHE_POLICY_UC);
}
/** @brief Drop owned mapping references and restore the last owner's attributes.
 * @param phys Page-aligned base of a previously acquired mapping.
 * @param len Page-aligned length, matched exactly by the resource owner.
 * @return None. Task context under execution transaction/pre-SMP, no callback.
 * Flushes all online CPU TLBs before return; shared split tables stay kernel-owned.
 */
void paging_release_mmio(uint64_t phys, uint64_t len)
{
    if (!mmio_valid_aligned_range(phys, len)) return;
    uint64_t end = phys + len;
    kernel_spin_lock(&mmio_lock);
    uint32_t lease_count;
    if (!mmio_plan_lease(phys, end, false, &lease_count)) {
        kernel_spin_unlock(&mmio_lock);
        return;
    }
    uint32_t run_count = 0;
    if (!mmio_plan_release(phys, end, &run_count) ||
        !mmio_registry_fits(run_count, lease_count)) {
        kernel_spin_unlock(&mmio_lock);
        return;
    }

    for (uint32_t i = 0; i < mmio_run_count; ++i) {
        const struct mmio_run *run = &mmio_runs[i];
        if (run->refs != 1 || run->permanent || run->end <= phys || run->start >= end) continue;
        uint64_t first = run->start < phys ? phys : run->start;
        uint64_t last = run->end > end ? end : run->end;
        if (!mmio_verify_leaves(first, last)) {
            kernel_spin_unlock(&mmio_lock);
            return;
        }
    }
    for (uint32_t i = 0; i < mmio_run_count; ++i) {
        const struct mmio_run *run = &mmio_runs[i];
        if (run->refs != 1 || run->permanent || run->end <= phys || run->start >= end) continue;
        uint64_t first = run->start < phys ? phys : run->start;
        uint64_t last = run->end > end ? end : run->end;
        for (uint64_t p = first; p < last; p += PAGE_SIZE) {
            uint64_t *pte = mmio_existing_leaf(p);
            *pte = (p & RELIEFNT_PHYS_ADDR_MASK) | run->original_flags;
        }
    }
    for (uint32_t i = 0; i < run_count; ++i) mmio_runs[i] = mmio_work[i];
    mmio_run_count = run_count;
    for (uint32_t i = 0; i < lease_count; ++i) mmio_leases[i] = mmio_lease_work[i];
    mmio_lease_count = lease_count;
    smp_flush_user_tlb();
    kernel_spin_unlock(&mmio_lock);
}
/** @brief Preserve bootstrap MMIO validation API with precise permanent UC/NX.
 * @param phys Physical device start; covering pages are rounded outward.
 * @param len Nonzero byte length; must not wrap or exceed direct map.
 * @return True on success. Execution transaction/pre-SMP only; no ownership
 * transfer. Repeated compatibility validation never adds owned references.
 */
bool paging_mmio_uncached(uint64_t phys, uint64_t len)
{
    if (!len || len > UINT64_MAX - phys || !paging_kernel_direct_map_range(phys, len)) return false;
    uint64_t byte_end = phys + len;
    if (byte_end > UINT64_MAX - (PAGE_SIZE - 1)) return false;
    uint64_t start = phys & ~(PAGE_SIZE - 1);
    uint64_t end = (byte_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    return end > start && mmio_acquire(start, end - start, true, MMIO_CACHE_POLICY_UC);
}

/** @brief Pin an exact page-aligned default-cache device aperture WB/NX.
 * @param phys Page-aligned physical base.
 * @param len Page-aligned nonzero bytes.
 * @return True after an idempotent permanent claim; false if any PTE already
 * is not a present writable supervisor identity mapping with default-cache
 * attributes, a UC interval overlaps, NX is unavailable, or validation and
 * bounded registry planning fail. Pages are never changed before the complete
 * policy/range plan is valid. Task context after device quiescence.
 */
bool paging_mmio_writeback_nx(uint64_t phys, uint64_t len)
{
    return mmio_acquire(phys, len, true, MMIO_CACHE_POLICY_WB);
}

/** @brief Allocate one zeroed page-table page and return its physical address.
 * @return Physical page address, or zero when the allocator is exhausted.
 */
static uint64_t alloc_table(void)
{
    return mm_alloc_page();
}

/** @brief Resolve a page-table entry's physical address through the kernel alias.
 * @param entry Present non-leaf entry.
 * @return Mapped table pointer, or NULL when outside the direct-map resource.
 */
static uint64_t *table_from_entry(uint64_t entry)
{
    if (!(entry & RELIEFNT_PAGE_PRESENT)) return NULL;
    return (uint64_t *)mm_phys_to_virt(entry & RELIEFNT_PHYS_ADDR_MASK);
}

/** @brief Return true when a user address is in the supported low canonical range.
 * @param address Virtual byte address.
 * @return True for a non-reserved low-half address below USER_TOP.
 */
static bool user_address_valid(uint64_t address)
{
    return address >= RELIEFNT_USER_BASE && address < RELIEFNT_USER_TOP &&
           !(address >= RELIEFNT_KERNEL_HOLE_START &&
             address < RELIEFNT_KERNEL_HOLE_END);
}

/** @brief Return the user leaf table for an address without allocating.
 * @param as Address space being inspected.
 * @param address Low canonical virtual address.
 * @return PTE pointer or NULL for a missing, large, or inaccessible path.
 */
static uint64_t *address_space_pte(const struct address_space *as, uint64_t address)
{
    if (!as || !as->pml4 || !user_address_valid(address)) return NULL;
    uint32_t pml4_index = (uint32_t)((address >> 39) & 511u);
    uint32_t pdpt_index = (uint32_t)((address >> 30) & 511u);
    uint32_t pd_index = (uint32_t)((address >> 21) & 511u);
    uint64_t pml4e = as->pml4[pml4_index];
    uint64_t *pdpt = table_from_entry(pml4e);
    if (!pdpt || !(pml4e & RELIEFNT_PAGE_USER)) return NULL;
    uint64_t pdpte = pdpt[pdpt_index];
    if (!(pdpte & RELIEFNT_PAGE_PRESENT) || (pdpte & PAGE_SIZE_FLAG) ||
        !(pdpte & RELIEFNT_PAGE_USER)) return NULL;
    uint64_t *pd = table_from_entry(pdpte);
    if (!pd) return NULL;
    uint64_t pde = pd[pd_index];
    if (!(pde & RELIEFNT_PAGE_PRESENT) || (pde & PAGE_SIZE_FLAG) ||
        !(pde & RELIEFNT_PAGE_USER)) return NULL;
    uint64_t *pt = table_from_entry(pde);
    if (!pt) return NULL;
    return &pt[(address >> 12) & 511u];
}

/** @brief Search one owned page-table level for its first backed user leaf.
 * @param table Table page at @p level; borrowed through the kernel direct map.
 * @param level 0 for PML4, 1 for PDPT, 2 for PD, or 3 for PT.
 * @param base Virtual base address covered by @p table.
 * @param start Inclusive search start.
 * @param end Exclusive search end.
 * @param out_page Required output for a found page-aligned user address.
 * @param out_phys Optional output for the mapped physical page.
 * @param out_flags Optional output for the full leaf flags.
 * @return True when a backed user page was found; false otherwise.
 * Branches are visited in virtual-address order, and absent or shared kernel
 * branches are skipped without scanning their virtual coverage.
 */
static bool find_user_leaf_in_table(const uint64_t *table, uint32_t level,
                                    uint64_t base, uint64_t start, uint64_t end,
                                    uint64_t *out_page, uint64_t *out_phys,
                                    uint64_t *out_flags)
{
    static const uint8_t index_shifts[] = {39, 30, 21, 12};
    uint32_t shift;
    uint64_t entry_span;
    uint64_t table_end;
    uint64_t first;
    uint64_t last;
    if (!table || level > 3 || start >= end) return false;
    shift = index_shifts[level];
    entry_span = 1ULL << shift;
    table_end = base + (entry_span << 9);
    if (start >= table_end || end <= base) return false;
    first = start <= base ? 0 : (start - base) >> shift;
    last = end - 1 <= base ? 0 : (end - 1 - base) >> shift;
    if (first > 511) return false;
    if (last > 511) last = 511;
    for (uint64_t index = first; index <= last; ++index) {
        uint64_t entry = table[index];
        uint64_t child_base = base + (index << shift);
        if (level < 3) {
            uint64_t *child;
            if ((entry & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER |
                          RELIEFNT_PAGE_TABLE_OWNED)) !=
                (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER |
                 RELIEFNT_PAGE_TABLE_OWNED) || (entry & PAGE_SIZE_FLAG))
                continue;
            child = table_from_entry(entry);
            if (child && find_user_leaf_in_table(child, level + 1, child_base,
                                                 start, end, out_page, out_phys,
                                                 out_flags)) return true;
            continue;
        }
        if (!(entry & RELIEFNT_PAGE_BACKED) || !(entry & RELIEFNT_PAGE_USER)) continue;
        if (child_base < start || child_base >= end) continue;
        *out_page = child_base;
        if (out_phys) *out_phys = entry & RELIEFNT_PHYS_ADDR_MASK;
        if (out_flags) *out_flags = entry & ~RELIEFNT_PHYS_ADDR_MASK;
        return true;
    }
    return false;
}

/** @brief Find the first mapped user PTE in a validated address-space interval.
 * @param as Address space whose private user hierarchy is searched.
 * @param start Inclusive user address.
 * @param end Exclusive end no greater than USER_TOP.
 * @param out_page Required output for the aligned virtual page.
 * @param out_phys Optional output for the physical leaf page.
 * @param out_flags Optional output for all leaf attributes.
 * @return True when a user leaf owns backing in the range; false when empty
 *         or invalid. Only allocated table branches are traversed.
 */
bool address_space_user_page_next(const struct address_space *as, uint64_t start,
                                  uint64_t end, uint64_t *out_page,
                                  uint64_t *out_phys, uint64_t *out_flags)
{
    if (!as || !as->pml4 || !out_page || start < RELIEFNT_USER_BASE ||
        start >= end || end > RELIEFNT_USER_TOP)
        return false;
    return find_user_leaf_in_table(as->pml4, 0, 0, start, end, out_page,
                                   out_phys, out_flags);
}

/** @brief Check a valid interval for any backed user leaf.
 * @param as Address space whose private user hierarchy is searched.
 * @param start Inclusive user address.
 * @param end Exclusive end no greater than USER_TOP.
 * @return True when no backed leaf exists; false for an occupied or invalid range.
 */
bool address_space_user_range_free(const struct address_space *as, uint64_t start,
                                   uint64_t end)
{
    uint64_t page;
    if (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START)
        return false;
    return !address_space_user_page_next(as, start, end, &page, NULL, NULL);
}

/** @brief Copy a physical page table into another allocated page.
 * @param destination New zeroed table page.
 * @param source Existing table entry, or zero for an empty table.
 * @return True after the table is reachable through the direct map.
 */
static bool copy_table_entries(uint64_t *destination, uint64_t source)
{
    uint64_t *source_table;
    if (!destination) return false;
    if (!(source & RELIEFNT_PAGE_PRESENT)) {
        for (uint32_t i = 0; i < 512; ++i) destination[i] = 0;
        return true;
    }
    source_table = table_from_entry(source);
    if (!source_table) return false;
    for (uint32_t i = 0; i < 512; ++i) destination[i] = source_table[i];
    return true;
}

/** @brief Split a 1 GiB leaf into equivalent 2 MiB PDEs.
 * @param destination Newly allocated page-directory page.
 * @param large_entry Existing 1 GiB leaf.
 */
static void split_1g_leaf(uint64_t *destination, uint64_t large_entry)
{
    const uint64_t base_mask = 0x000fffffc0000000ULL;
    uint64_t base = large_entry & base_mask;
    uint64_t flags = large_entry & ~RELIEFNT_PHYS_ADDR_MASK;
    bool large_pat = (large_entry & (1ULL << 12)) != 0;
    flags |= PAGE_SIZE_FLAG;
    if (large_pat) flags |= (1ULL << 12);
    for (uint32_t i = 0; i < 512; ++i)
        destination[i] = (base + (uint64_t)i * PAGE_SIZE_2M) | flags;
}

/** @brief Split a 2 MiB leaf into equivalent 4 KiB PTEs.
 * @param destination Newly allocated page-table page.
 * @param large_entry Existing 2 MiB leaf.
 */
static void split_2m_leaf(uint64_t *destination, uint64_t large_entry)
{
    const uint64_t base_mask = 0x000fffffffe00000ULL;
    uint64_t base = large_entry & base_mask;
    uint64_t flags = large_entry & ~RELIEFNT_PHYS_ADDR_MASK;
    bool large_pat = (large_entry & (1ULL << 12)) != 0;
    flags &= ~PAGE_SIZE_FLAG;
    if (large_pat) flags |= RELIEFNT_PAGE_PAT;
    for (uint32_t i = 0; i < 512; ++i)
        destination[i] = (base + (uint64_t)i * PAGE_SIZE) | flags;
}

/** @brief Ensure one private child table exists in a transaction's staged tree.
 * @param parent Parent table in the staged tree.
 * @param parent_level 0 for PML4, 1 for PDPT, or 2 for PD.
 * @param index Entry index to prepare.
 * @return Private child table pointer, or NULL after freeing failed allocation.
 */
static uint64_t *ensure_staged_child(uint64_t *parent, uint32_t parent_level,
                                     uint32_t index)
{
    uint64_t old_entry = parent[index];
    uint64_t *child;
    uint64_t phys;
    bool large = (old_entry & RELIEFNT_PAGE_PRESENT) &&
                 parent_level != 0 && (old_entry & PAGE_SIZE_FLAG);
    if ((old_entry & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED |
                      RELIEFNT_PAGE_TABLE_STAGED)) ==
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED |
         RELIEFNT_PAGE_TABLE_STAGED) && !large) {
        return table_from_entry(old_entry);
    }
    phys = alloc_table();
    if (!phys) return NULL;
    child = (uint64_t *)mm_phys_to_virt(phys);
    if (!child) {
        mm_free_page(phys);
        return NULL;
    }
    if ((old_entry & RELIEFNT_PAGE_PRESENT) && !large) {
        if (!copy_table_entries(child, old_entry)) {
            mm_free_page(phys);
            return NULL;
        }
    } else if (large && parent_level == 1) {
        split_1g_leaf(child, old_entry);
    } else if (large && parent_level == 2) {
        split_2m_leaf(child, old_entry);
    } else {
        for (uint32_t i = 0; i < 512; ++i) child[i] = 0;
    }
    parent[index] = phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE |
                    RELIEFNT_PAGE_USER | RELIEFNT_PAGE_TABLE_OWNED |
                    RELIEFNT_PAGE_TABLE_STAGED;
    /* Earlier staged copies retain all their other children, so only the
     * superseded table page itself is now unreachable. */
    if ((old_entry & RELIEFNT_PAGE_TABLE_STAGED) && !large)
        mm_free_page(old_entry & RELIEFNT_PHYS_ADDR_MASK);
    return child;
}

/** @brief Test whether all page-table levels for a 2 MiB region are already private.
 * @param root PML4 root to inspect.
 * @param address Any address in the region.
 * @return True when a private normal 4 KiB leaf table is ready.
 */
static bool user_region_prepared(uint64_t *root, uint64_t address)
{
    uint64_t pml4e = root[(address >> 39) & 511u];
    uint64_t *pdpt;
    uint64_t pdpte;
    uint64_t *pd;
    uint64_t pde;
    if ((pml4e & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) !=
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) return false;
    pdpt = table_from_entry(pml4e);
    if (!pdpt) return false;
    pdpte = pdpt[(address >> 30) & 511u];
    if ((pdpte & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) !=
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED) ||
        (pdpte & PAGE_SIZE_FLAG)) return false;
    pd = table_from_entry(pdpte);
    if (!pd) return false;
    pde = pd[(address >> 21) & 511u];
    if ((pde & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) !=
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED) ||
        (pde & PAGE_SIZE_FLAG)) return false;
    return table_from_entry(pde) != NULL;
}

/** @brief Discard staged page-table pages after a failed range transaction.
 * @param entry Staged branch entry.
 * @param table_level Level of the table named by entry, in [1,3].
 */
static void discard_staged_branch(uint64_t entry, uint32_t table_level)
{
    uint64_t *table;
    if (!(entry & RELIEFNT_PAGE_TABLE_STAGED) ||
        !(entry & RELIEFNT_PAGE_PRESENT)) return;
    table = table_from_entry(entry);
    if (table && table_level < 3) {
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t child = table[i];
            if ((child & RELIEFNT_PAGE_TABLE_STAGED) &&
                !(child & PAGE_SIZE_FLAG))
                discard_staged_branch(child, table_level + 1);
        }
    }
    mm_free_page(entry & RELIEFNT_PHYS_ADDR_MASK);
}

/** @brief Remove staging metadata from newly published table branches.
 * @param parent Parent table whose entries are being visited.
 * @param parent_level Parent level, from 0 through 2.
 */
static void publish_staged_children(uint64_t *parent, uint32_t parent_level)
{
    if (parent_level >= 3) return;
    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t entry = parent[i];
        if (!(entry & RELIEFNT_PAGE_TABLE_STAGED)) continue;
        parent[i] &= ~RELIEFNT_PAGE_TABLE_STAGED;
        if (!(parent_level != 0 && (entry & PAGE_SIZE_FLAG))) {
            uint64_t *child = table_from_entry(entry);
            if (child) publish_staged_children(child, parent_level + 1);
        }
    }
}

/** @brief Reclaim replaced owned table pages after the new root is globally visible.
 * @param old_entry Former branch entry.
 * @param new_entry Published branch entry at the same index.
 * @param table_level Level of the table referenced by old_entry.
 */
static void release_replaced_tables(uint64_t old_entry, uint64_t new_entry,
                                    uint32_t table_level)
{
    uint64_t old_phys = old_entry & RELIEFNT_PHYS_ADDR_MASK;
    uint64_t new_phys = new_entry & RELIEFNT_PHYS_ADDR_MASK;
    uint64_t *old_table;
    uint64_t *new_table = NULL;
    if (!(old_entry & RELIEFNT_PAGE_PRESENT) ||
        !(old_entry & RELIEFNT_PAGE_TABLE_OWNED) || old_phys == new_phys)
        return;
    old_table = table_from_entry(old_entry);
    if (!old_table) return;
    if ((new_entry & RELIEFNT_PAGE_PRESENT) &&
        (table_level >= 3 || !(new_entry & PAGE_SIZE_FLAG)))
        new_table = table_from_entry(new_entry);
    if (table_level < 3) {
        for (uint32_t i = 0; i < 512; ++i) {
            uint64_t old_child = old_table[i];
            if (old_child & PAGE_SIZE_FLAG) continue;
            release_replaced_tables(old_child, new_table ? new_table[i] : 0,
                                    table_level + 1);
        }
    }
    mm_free_page(old_phys);
}

/** @brief Create an empty user address space with the reserved low kernel window and canonical high-half kernel mappings.
 * @param as Output address space, initialized to zero on entry.
 * @return True after allocating an independent PML4 root.
 * The bootstrap root maps much of low physical RAM as identity-mapped
 * supervisor pages, overlapping legal user addresses and blocking lazy
 * faults. User roots therefore inherit only the separately built supervisor
 * kernel-image window, plus canonical high-half mappings for kernel data and
 * the physical direct map.
 */
bool address_space_create(struct address_space *as)
{
    uint64_t phys;
    if (!as) return false;
    for (size_t i = 0; i < sizeof(*as); ++i) ((uint8_t *)as)[i] = 0;
    phys = alloc_table();
    if (!phys) return false;
    as->pml4 = (uint64_t *)mm_phys_to_virt(phys);
    if (!as->pml4) {
        mm_free_page(phys);
        as->pml4 = NULL;
        return false;
    }
    for (uint32_t i = 0; i < 512; ++i) {
        if (i == 0) as->pml4[i] = kernel_hole_pml4e;
        else as->pml4[i] = i >= 256 ? kernel_pml4[i] : 0;
    }
    as->cr3 = phys;
    return true;
}

/** @brief Create child mappings with private page tables and shared COW backing.
 * @param source Existing parent address space.
 * @param destination Empty output address space.
 * @return True on a complete clone; failure destroys the child and leaves the
 *         parent PTEs unchanged.
 */
bool address_space_clone_cow(struct address_space *source,
                             struct address_space *destination)
{
    if (!source || !destination || !source->pml4 || !address_space_create(destination))
        return false;
    for (uint32_t pi = 0; pi < 256; ++pi) {
        uint64_t pml4e = source->pml4[pi];
        uint64_t *src_pdpt;
        if (!(pml4e & RELIEFNT_PAGE_PRESENT) ||
            !(pml4e & RELIEFNT_PAGE_TABLE_OWNED)) continue;
        src_pdpt = table_from_entry(pml4e);
        if (!src_pdpt) goto fail;
        for (uint32_t di = 0; di < 512; ++di) {
            uint64_t pdpte = src_pdpt[di];
            uint64_t *src_pd;
            uint64_t region_start = ((uint64_t)pi << 39) | ((uint64_t)di << 30);
            if (!(pdpte & RELIEFNT_PAGE_PRESENT) || (pdpte & PAGE_SIZE_FLAG) ||
                !(pdpte & RELIEFNT_PAGE_TABLE_OWNED)) continue;
            src_pd = table_from_entry(pdpte);
            if (!src_pd) goto fail;
            for (uint32_t ti = 0; ti < 512; ++ti) {
                uint64_t pde = src_pd[ti];
                uint64_t region_2m = region_start + (uint64_t)ti * PAGE_SIZE_2M;
                uint64_t *src_pt;
                if (!(pde & RELIEFNT_PAGE_PRESENT) || (pde & PAGE_SIZE_FLAG) ||
                    !(pde & RELIEFNT_PAGE_TABLE_OWNED)) continue;
                src_pt = table_from_entry(pde);
                if (!src_pt) goto fail;
                bool have_user_leaf = false;
                for (uint32_t si = 0; si < 512; ++si) {
                    uint64_t leaf = src_pt[si];
                    if ((leaf & RELIEFNT_PAGE_BACKED) && (leaf & RELIEFNT_PAGE_USER)) {
                        have_user_leaf = true;
                        break;
                    }
                }
                if (!have_user_leaf) continue;
                if (!address_space_prepare_user_range(destination, region_2m,
                                                     region_2m + PAGE_SIZE_2M))
                    goto fail;
                uint64_t *dst_pdpt = table_from_entry(destination->pml4[pi]);
                uint64_t *dst_pd = dst_pdpt ? table_from_entry(dst_pdpt[di]) : NULL;
                uint64_t *dst_leaf_table = dst_pd ? table_from_entry(dst_pd[ti]) : NULL;
                if (!dst_leaf_table) goto fail;
                for (uint32_t si = 0; si < 512; ++si) {
                    uint64_t leaf = src_pt[si];
                    uint64_t flags;
                    uint64_t phys;
                    if (!(leaf & RELIEFNT_PAGE_BACKED) ||
                        !(leaf & RELIEFNT_PAGE_USER)) continue;
                    phys = leaf & RELIEFNT_PHYS_ADDR_MASK;
                    flags = leaf & ~(RELIEFNT_PHYS_ADDR_MASK | RELIEFNT_PAGE_USER |
                                     RELIEFNT_PAGE_PRESENT);
                    if (!(leaf & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED)) &&
                        ((leaf & RELIEFNT_PAGE_WRITABLE) ||
                         (leaf & RELIEFNT_PAGE_COW))) {
                        flags &= ~RELIEFNT_PAGE_WRITABLE;
                        flags |= RELIEFNT_PAGE_COW;
                    }
                    if (!(leaf & RELIEFNT_PAGE_DEVICE) &&
                        page_cache_retain(phys) != 0) mm_retain_page(phys);
                    dst_leaf_table[si] = phys | RELIEFNT_PAGE_USER | flags |
                        (leaf & RELIEFNT_PAGE_PRESENT);
                    ++destination->user_page_count;
                }
            }
        }
    }
    /* Protect the parent only after every child allocation/reference succeeded. */
    for (uint32_t pi = 0; pi < 256; ++pi) {
        uint64_t pml4e = source->pml4[pi];
        uint64_t *pdpt = table_from_entry(pml4e);
        if (!pdpt || !(pml4e & RELIEFNT_PAGE_TABLE_OWNED)) continue;
        for (uint32_t di = 0; di < 512; ++di) {
            uint64_t *pd = table_from_entry(pdpt[di]);
            if (!pd || !(pdpt[di] & RELIEFNT_PAGE_TABLE_OWNED) ||
                (pdpt[di] & PAGE_SIZE_FLAG)) continue;
            for (uint32_t ti = 0; ti < 512; ++ti) {
                uint64_t *pt = table_from_entry(pd[ti]);
                if (!pt || !(pd[ti] & RELIEFNT_PAGE_TABLE_OWNED) ||
                    (pd[ti] & PAGE_SIZE_FLAG)) continue;
                for (uint32_t si = 0; si < 512; ++si) {
                    uint64_t *leaf = &pt[si];
                    if (!(*leaf & RELIEFNT_PAGE_BACKED) ||
                        !(*leaf & RELIEFNT_PAGE_USER) ||
                        (*leaf & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED))) continue;
                    if ((*leaf & RELIEFNT_PAGE_WRITABLE) || (*leaf & RELIEFNT_PAGE_COW))
                        *leaf = (*leaf & ~RELIEFNT_PAGE_WRITABLE) | RELIEFNT_PAGE_COW;
                }
            }
        }
    }
    paging_note_mm_change();
    smp_flush_user_tlb();
    return true;
fail:
    address_space_destroy(destination);
    return false;
}

/** @brief Destroy an owned user page-table branch and release mapped user pages.
 * @param entry Owned branch entry to release.
 * @param table_level Level of the table named by entry, from 1 through 3.
 */
static void destroy_user_branch(uint64_t entry, uint32_t table_level)
{
    uint64_t *table;
    if (!(entry & RELIEFNT_PAGE_PRESENT) ||
        !(entry & RELIEFNT_PAGE_TABLE_OWNED)) return;
    table = table_from_entry(entry);
    if (!table) return;
    for (uint32_t i = 0; i < 512; ++i) {
        uint64_t child = table[i];
        if (table_level < 3) {
            if ((child & RELIEFNT_PAGE_PRESENT) && !(child & PAGE_SIZE_FLAG))
                destroy_user_branch(child, table_level + 1);
        } else if ((child & RELIEFNT_PAGE_BACKED) &&
                   (child & RELIEFNT_PAGE_USER)) {
            uint64_t phys = child & RELIEFNT_PHYS_ADDR_MASK;
            if (child & RELIEFNT_PAGE_DEVICE) continue;
            if (page_cache_owns(phys)) page_cache_release(phys);
            else mm_free_page(phys);
        }
    }
    mm_free_page(entry & RELIEFNT_PHYS_ADDR_MASK);
}

/** @brief Release all private user tables and leaf ownership, then clear as.
 * @param as Address space to destroy; NULL is ignored.
 */
void address_space_destroy(struct address_space *as)
{
    if (!as) return;
    if (as->pml4) {
        for (uint32_t i = 0; i < 256; ++i)
            destroy_user_branch(as->pml4[i], 1);
        mm_free_page(as->cr3);
    }
    for (size_t i = 0; i < sizeof(*as); ++i) ((uint8_t *)as)[i] = 0;
}

/** @brief Prepare a user range with all private table pages committed atomically.
 * @param as Address space protected by the caller's execution ownership.
 * @param start Inclusive first byte of the range.
 * @param end Exclusive end, no greater than the canonical low-half boundary.
 * @return True when all 4 KiB leaf tables are privately writable; false leaves
 *         existing mappings untouched and frees every staged table page.
 */
bool address_space_prepare_user_range(struct address_space *as, uint64_t start,
                                      uint64_t end)
{
    uint64_t first_region;
    uint64_t end_region;
    uint64_t stage_phys;
    uint64_t *stage_root;
    uint64_t old_root[512];
    bool needs_change = false;
    if (!as || !as->pml4 || start < RELIEFNT_USER_BASE || start >= end ||
        end > RELIEFNT_USER_TOP ||
        (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START))
        return false;
    first_region = align_down(start, PAGE_SIZE_2M);
    end_region = align_down(end - 1, PAGE_SIZE_2M) + PAGE_SIZE_2M;
    for (uint64_t region = first_region; region < end_region; region += PAGE_SIZE_2M) {
        if (!user_region_prepared(as->pml4, region)) {
            needs_change = true;
            break;
        }
    }
    if (!needs_change) return true;
    stage_phys = alloc_table();
    if (!stage_phys) return false;
    stage_root = (uint64_t *)mm_phys_to_virt(stage_phys);
    if (!stage_root) {
        mm_free_page(stage_phys);
        return false;
    }
    for (uint32_t i = 0; i < 512; ++i) {
        stage_root[i] = as->pml4[i];
        old_root[i] = as->pml4[i];
    }
    for (uint64_t region = first_region; region < end_region; region += PAGE_SIZE_2M) {
        uint32_t pml4_index = (uint32_t)((region >> 39) & 511u);
        uint32_t pdpt_index = (uint32_t)((region >> 30) & 511u);
        uint32_t pd_index = (uint32_t)((region >> 21) & 511u);
        uint64_t *pdpt;
        uint64_t *pd;
        if (user_region_prepared(stage_root, region)) continue;
        pdpt = ensure_staged_child(stage_root, 0, pml4_index);
        if (!pdpt) goto rollback;
        pd = ensure_staged_child(pdpt, 1, pdpt_index);
        if (!pd) goto rollback;
        if (!ensure_staged_child(pd, 2, pd_index)) goto rollback;
    }
    for (uint32_t i = 0; i < 512; ++i) as->pml4[i] = stage_root[i];
    publish_staged_children(as->pml4, 0);
    mm_free_page(stage_phys);
    paging_note_mm_change();
    smp_flush_user_tlb();
    for (uint32_t i = 0; i < 256; ++i)
        release_replaced_tables(old_root[i], as->pml4[i], 1);
    return true;
rollback:
    for (uint32_t i = 0; i < 256; ++i)
        discard_staged_branch(stage_root[i], 1);
    mm_free_page(stage_phys);
    return false;
}

/** @brief Install one owned user leaf with validated W^X and backing flags.
 * @param as Address space receiving the mapping.
 * @param vaddr User byte address; page-aligned internally.
 * @param phys Aligned physical backing page.
 * @param flags Mapping protection and software ownership flags.
 * @return True when an empty leaf is installed; false leaves caller ownership.
 */
bool address_space_map_user_page(struct address_space *as, uint64_t vaddr,
                                 uint64_t phys, uint64_t flags)
{
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    uint64_t *pte;
    if (!as || !phys || (phys & ~RELIEFNT_PHYS_ADDR_MASK) ||
        (phys & (PAGE_SIZE - 1)) || !user_address_valid(page) ||
        ((flags & RELIEFNT_PAGE_WRITABLE) && !(flags & RELIEFNT_PAGE_NOEXEC)) ||
        !nx_enabled) return false;
    if (!address_space_prepare_user_range(as, page, page + PAGE_SIZE)) return false;
    pte = address_space_pte(as, page);
    if (!pte || (*pte & RELIEFNT_PAGE_BACKED)) return false;
    *pte = phys | RELIEFNT_PAGE_USER | flags |
           ((flags & RELIEFNT_PAGE_PROTNONE) ? 0 : RELIEFNT_PAGE_PRESENT);
    ++as->user_page_count;
    x86_64_invlpg(page);
    paging_note_mm_change();
    return true;
}

/** @brief Change permissions while retaining the current mapping's ownership.
 * @param as Address space with the mapped leaf.
 * @param vaddr Address within the leaf page.
 * @param flags New protection and software permission bits.
 * @return True after an existing backed mapping changes; false on invalid input.
 */
bool address_space_protect_user_page(struct address_space *as, uint64_t vaddr,
                                     uint64_t flags)
{
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    uint64_t *pte;
    uint64_t entry;
    if (!as || !user_address_valid(page) ||
        ((flags & RELIEFNT_PAGE_WRITABLE) && !(flags & RELIEFNT_PAGE_NOEXEC)))
        return false;
    pte = address_space_pte(as, page);
    if (!pte || !(*pte & RELIEFNT_PAGE_BACKED)) return false;
    entry = *pte;
    if ((flags & RELIEFNT_PAGE_WRITABLE) && !(entry & RELIEFNT_PAGE_WRITABLE) &&
        !(entry & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED)))
        flags = (flags & ~RELIEFNT_PAGE_WRITABLE) | RELIEFNT_PAGE_COW;
    flags |= entry & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED | RELIEFNT_PAGE_PWT |
                      RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT);
    *pte = (entry & RELIEFNT_PHYS_ADDR_MASK) | RELIEFNT_PAGE_USER | flags |
           ((flags & RELIEFNT_PAGE_PROTNONE) ? 0 : RELIEFNT_PAGE_PRESENT);
    x86_64_invlpg(page);
    paging_note_mm_change();
    return true;
}

/** @brief Remove a user leaf without releasing its backing reference.
 * @param as Address space containing the leaf.
 * @param vaddr Address within the leaf page.
 * @return Physical backing page to the caller, or zero when absent.
 */
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t vaddr)
{
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    uint64_t *pte;
    uint64_t entry;
    if (!as || !user_address_valid(page)) return 0;
    pte = address_space_pte(as, page);
    if (!pte || !(*pte & RELIEFNT_PAGE_BACKED)) return 0;
    entry = *pte;
    *pte = 0;
    if (as->user_page_count) --as->user_page_count;
    x86_64_invlpg(page);
    paging_note_mm_change();
    return entry & RELIEFNT_PHYS_ADDR_MASK;
}

/** @brief Return the physical backing page for a mapped user address.
 * @param as Address space to inspect.
 * @param vaddr Address within the mapping.
 * @return Aligned physical page, or zero when no backing exists.
 */
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t vaddr)
{
    uint64_t *pte = address_space_pte(as, vaddr);
    return pte && (*pte & RELIEFNT_PAGE_BACKED) ?
           (*pte & RELIEFNT_PHYS_ADDR_MASK) : 0;
}

/** @brief Return the user leaf's hardware/software flags without its PFN.
 * @param as Address space to inspect.
 * @param vaddr Address within the queried page.
 * @return Leaf flags, or zero if unmapped or outside the user range.
 */
uint64_t address_space_user_page_flags(const struct address_space *as, uint64_t vaddr)
{
    uint64_t *pte = address_space_pte(as, vaddr);
    return pte ? (*pte & ~RELIEFNT_PHYS_ADDR_MASK) : 0;
}

/** @brief Test whether a user leaf is present and readable.
 * @param as Address space to inspect.
 * @param vaddr Address within the queried page.
 * @return True for a present user PTE.
 */
bool address_space_user_page_readable(const struct address_space *as, uint64_t vaddr)
{
    uint64_t *pte = address_space_pte(as, vaddr);
    return pte && ((*pte & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER)) ==
                   (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER));
}

/** @brief Retry a fault after another thread installed adequate page permissions.
 * @param as Address space sharing the mapping.
 * @param vaddr Fault address.
 * @param fault_error x86 page-fault error flags.
 * @return True after invalidating a now-usable stale translation.
 */
bool address_space_retry_mapped_user_page(const struct address_space *as,
                                          uint64_t vaddr, uint64_t fault_error)
{
    uint64_t *pte;
    uint64_t entry;
    if (!as || (fault_error & (1ULL | 8ULL)) || !user_address_valid(vaddr))
        return false;
    pte = address_space_pte(as, vaddr);
    if (!pte) return false;
    entry = *pte;
    if ((entry & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER)) !=
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER)) return false;
    if ((fault_error & 2ULL) && !(entry & RELIEFNT_PAGE_WRITABLE)) return false;
    if ((fault_error & 16ULL) && (entry & RELIEFNT_PAGE_NOEXEC)) return false;
    x86_64_invlpg(align_down(vaddr, PAGE_SIZE));
    paging_note_mm_change();
    return true;
}

/** @brief Test whether a present user mapping allows writes without resolving COW.
 * @param as Address space, or NULL for an invalid mapping.
 * @param vaddr Address within the queried page.
 * @return True only for a present, user-accessible, writable page.
 */
bool address_space_user_page_writable(const struct address_space *as, uint64_t vaddr)
{
    uint64_t *pte = address_space_pte(as, vaddr);
    uint64_t required = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER |
                        RELIEFNT_PAGE_WRITABLE;
    return pte && ((*pte & required) == required);
}

/** @brief Test whether a user leaf is a borrowed device PFN mapping.
 * @param as Address space to inspect.
 * @param vaddr Address within the mapping.
 * @return True for a backed leaf marked as device memory.
 */
bool address_space_user_page_is_device(const struct address_space *as, uint64_t vaddr)
{
    uint64_t *pte = address_space_pte(as, vaddr);
    return pte && (*pte & RELIEFNT_PAGE_BACKED) &&
           (*pte & RELIEFNT_PAGE_DEVICE);
}

/** @brief Count mapped user bytes in KiB, including inaccessible backed leaves.
 * @param as Address space to inspect.
 * @return Per-address-space mapped byte count divided by 1024.
 */
uint64_t address_space_user_memory_kib(const struct address_space *as)
{
    return as ? as->user_page_count * (PAGE_SIZE / 1024ULL) : 0;
}

/** @brief Count resident owned user RAM pages, excluding borrowed device PFNs.
 * @param as Address space to inspect.
 * @return Resident user bytes in KiB.
 */
uint64_t address_space_user_resident_kib(const struct address_space *as)
{
    uint64_t pages = 0;
    if (!as || !as->pml4) return 0;
    for (uint32_t pi = 0; pi < 256; ++pi) {
        uint64_t *pdpt = table_from_entry(as->pml4[pi]);
        if (!pdpt || !(as->pml4[pi] & RELIEFNT_PAGE_TABLE_OWNED)) continue;
        for (uint32_t di = 0; di < 512; ++di) {
            uint64_t *pd = table_from_entry(pdpt[di]);
            if (!pd || !(pdpt[di] & RELIEFNT_PAGE_TABLE_OWNED) ||
                (pdpt[di] & PAGE_SIZE_FLAG)) continue;
            for (uint32_t ti = 0; ti < 512; ++ti) {
                uint64_t *pt = table_from_entry(pd[ti]);
                if (!pt || !(pd[ti] & RELIEFNT_PAGE_TABLE_OWNED) ||
                    (pd[ti] & PAGE_SIZE_FLAG)) continue;
                for (uint32_t si = 0; si < 512; ++si) {
                    uint64_t leaf = pt[si];
                    if ((leaf & RELIEFNT_PAGE_BACKED) &&
                        (leaf & RELIEFNT_PAGE_USER) &&
                        !(leaf & RELIEFNT_PAGE_DEVICE)) ++pages;
                }
            }
        }
    }
    return pages * (PAGE_SIZE / 1024ULL);
}

/** @brief Map the initial stack pages ending at stack_top, rolling back failures.
 * @param as Address space receiving the stack.
 * @param stack_top Exclusive stack end, at or below USER_TOP.
 * @return True when all initial pages are mapped.
 */
bool address_space_map_user_stack(struct address_space *as, uint64_t stack_top)
{
    uint64_t first;
    uint32_t mapped = 0;
    if (!as || stack_top <= (uint64_t)RELIEFNT_USER_STACK_PAGES * PAGE_SIZE ||
        stack_top > RELIEFNT_USER_TOP || (stack_top & (PAGE_SIZE - 1))) return false;
    first = stack_top - (uint64_t)RELIEFNT_USER_STACK_PAGES * PAGE_SIZE;
    if (first < RELIEFNT_USER_BASE ||
        (first < RELIEFNT_KERNEL_HOLE_END && stack_top > RELIEFNT_KERNEL_HOLE_START))
        return false;
    for (uint32_t i = 0; i < RELIEFNT_USER_STACK_PAGES; ++i) {
        uint64_t page = first + (uint64_t)i * PAGE_SIZE;
        uint64_t phys = mm_alloc_page();
        if (!phys || !address_space_map_user_page(as, page, phys,
                RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC)) {
            if (phys) mm_free_page(phys);
            while (mapped) {
                uint64_t old = address_space_unmap_user_page(
                    as, first + (uint64_t)(--mapped) * PAGE_SIZE);
                if (old) mm_free_page(old);
            }
            return false;
        }
        ++mapped;
    }
    return true;
}

/** @brief Allocate and map one user stack-growth page.
 * @param as Address space receiving the page.
 * @param page Aligned virtual page address.
 * @return True for an existing or newly mapped stack page.
 */
bool address_space_map_user_stack_page(struct address_space *as, uint64_t page)
{
    uint64_t phys;
    if (!as || (page & (PAGE_SIZE - 1)) || !user_address_valid(page)) return false;
    if (address_space_user_page_phys(as, page)) return true;
    phys = mm_alloc_page();
    if (!phys) return false;
    if (!address_space_map_user_page(as, page, phys,
                                     RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC)) {
        mm_free_page(phys);
        return false;
    }
    return true;
}

/** @brief Resolve a private COW write by copying its backing page.
 * @param as Address space containing the faulting leaf.
 * @param vaddr Faulting virtual byte address.
 * @return True once the new private page is writable.
 */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t vaddr)
{
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    uint64_t *pte;
    uint64_t entry;
    uint64_t old_phys;
    uint64_t new_phys;
    if (!as || !user_address_valid(page)) return false;
    pte = address_space_pte(as, page);
    if (!pte) return false;
    entry = *pte;
    if (!(entry & RELIEFNT_PAGE_PRESENT) || !(entry & RELIEFNT_PAGE_COW))
        return false;
    old_phys = entry & RELIEFNT_PHYS_ADDR_MASK;
    new_phys = mm_alloc_page();
    if (!new_phys) return false;
    copy_page(new_phys, old_phys);
    *pte = new_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER |
           RELIEFNT_PAGE_WRITABLE |
           (entry & (RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PWT |
                     RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT));
    x86_64_invlpg(page);
    paging_note_mm_change();
    if (page_cache_owns(old_phys)) page_cache_release(old_phys);
    else mm_free_page(old_phys);
    return true;
}

/** @brief Find or construct a supervisor leaf in the inherited high-half PDPT under mmio_lock.
 * @param address Validated aligned reserved high-half virtual page (module/device).
 * @param create True to allocate missing PD/PT pages from the physical allocator.
 * @return Borrowed leaf or NULL; failed creation rolls back its new branch.
 */
static uint64_t *kernel_shared_leaf(uint64_t address,bool create)
{
    uint64_t *pdpte=&kernel_pdpt[(address>>30)&511u];
    bool new_pd=false;
    if(!(*pdpte&RELIEFNT_PAGE_PRESENT)) {
        if(!create) return NULL;
        uint64_t physical=mm_alloc_page();
        if(!physical) return NULL;
        uint64_t *table=mm_phys_to_virt(physical);
        if(!table) { mm_free_page(physical); return NULL; }
        zero_table(table);
        *pdpte=physical|RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_TABLE_OWNED;
        new_pd=true;
    }
    if(*pdpte&(PAGE_SIZE_FLAG|RELIEFNT_PAGE_USER)) return NULL;
    uint64_t *pd=mm_phys_to_virt(*pdpte&RELIEFNT_PHYS_ADDR_MASK);
    if(!pd) return NULL;
    uint64_t *pde=&pd[(address>>21)&511u];
    if(!(*pde&RELIEFNT_PAGE_PRESENT)) {
        if(!create) return NULL;
        uint64_t physical=mm_alloc_page();
        uint64_t *table=physical ? mm_phys_to_virt(physical) : NULL;
        if(!table) {
            if(physical) mm_free_page(physical);
            if(new_pd) {
                uint64_t old=*pdpte&RELIEFNT_PHYS_ADDR_MASK;
                *pdpte=0; smp_flush_user_tlb(); mm_free_page(old);
            }
            return NULL;
        }
        zero_table(table);
        *pde=physical|RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_TABLE_OWNED;
    }
    if(*pde&(PAGE_SIZE_FLAG|RELIEFNT_PAGE_USER)) return NULL;
    uint64_t *pt=mm_phys_to_virt(*pde&RELIEFNT_PHYS_ADDR_MASK);
    return pt ? &pt[(address>>12)&511u] : NULL;
}
/** @brief Test whether a dynamically owned shared table has any remaining entries.
 * @param table Stable 512-entry page-table page under mmio_lock.
 * @return True when entirely empty, allowing reclamation after TLB synchronization.
 */
static bool kernel_shared_table_empty(const uint64_t *table)
{
    for(uint32_t i=0;i<512;++i) if(table[i]) return false;
    return true;
}

/** @brief Validate one complete reserved supervisor-only device virtual page.
 * @param address Candidate virtual device page.
 * @return True for an aligned address in the device-only high-half window.
 */
static bool io_page_address(uint64_t address)
{
    return address >= RELIEFNT_IOREMAP_BASE && address < RELIEFNT_IOREMAP_END &&
        !(address & (PAGE_SIZE - 1));
}

/** @brief Install a high device page UC/RW/NX without constructing a RAM alias.
 * @param address Valid aligned reserved device virtual page, currently absent.
 * @param physical Aligned representable physical BAR page beyond the direct map.
 * @return True or false; short map lock spans table allocation. The caller owns
 * the range and synchronizes all CPU TLBs before publication; no device RAM owner.
 */
bool paging_io_map_page(uint64_t address, uint64_t physical)
{
    if (!nx_enabled || !io_page_address(address) ||
        physical < RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        (physical & ~RELIEFNT_PHYS_ADDR_MASK)) return false;
    kernel_spin_lock(&mmio_lock);
    uint64_t *pte = kernel_shared_leaf(address, true);
    if (!pte || *pte) { kernel_spin_unlock(&mmio_lock); return false; }
    *pte = physical | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE |
        RELIEFNT_PAGE_NOEXEC | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PWT;
    kernel_spin_unlock(&mmio_lock);
    return true;
}

/** @brief Retire a complete stopped BAR with one CPU-wide TLB synchronization.
 * @param address Aligned reserved virtual base, all mapping users retired.
 * @param bytes Nonzero page-aligned extent entirely within the device window.
 * @return True if removed, false if invalid/partly absent, without mutation on
 * rejection. Empty owned tables are detached before the single shootdown and
 * freed only after it; physical device pages are never freed or dereferenced.
 */
bool paging_io_unmap_range(uint64_t address, uint64_t bytes)
{
    if (!io_page_address(address) || !bytes || (bytes & (PAGE_SIZE - 1)) ||
        bytes > RELIEFNT_IOREMAP_END - address) return false;
    kernel_spin_lock(&mmio_lock);
    for (uint64_t offset = 0; offset < bytes; offset += PAGE_SIZE) {
        uint64_t *pte = kernel_shared_leaf(address + offset, false);
        if (!pte || !(*pte & RELIEFNT_PAGE_PRESENT)) {
            kernel_spin_unlock(&mmio_lock); return false;
        }
    }
    for (uint64_t offset = 0; offset < bytes; offset += PAGE_SIZE)
        *kernel_shared_leaf(address + offset, false) = 0;
    uint64_t retired = 0;
    for (uint64_t at = address; at < address + bytes;) {
        uint64_t *pdpte = &kernel_pdpt[(at >> 30) & 511u];
        if (*pdpte & RELIEFNT_PAGE_PRESENT) {
            uint64_t pd_phys = *pdpte & RELIEFNT_PHYS_ADDR_MASK;
            uint64_t *pd = mm_phys_to_virt(pd_phys);
            uint64_t *pde = &pd[(at >> 21) & 511u];
            if ((*pde & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) ==
                (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_TABLE_OWNED)) {
                uint64_t pt_phys = *pde & RELIEFNT_PHYS_ADDR_MASK;
                uint64_t *pt = mm_phys_to_virt(pt_phys);
                if (kernel_shared_table_empty(pt)) {
                    *pde = 0;
                    /* Retired tables form an allocation-free physical list.
                     * Aligned next-page values have PRESENT=0, so speculative
                     * old walks cannot interpret list links as valid entries. */
                    pt[0] = retired; retired = pt_phys;
                }
            }
            if ((*pdpte & RELIEFNT_PAGE_TABLE_OWNED) && kernel_shared_table_empty(pd)) {
                *pdpte = 0;
                pd[0] = retired; retired = pd_phys;
            }
        }
        at = (at & ~(PAGE_SIZE_2M - 1)) + PAGE_SIZE_2M;
    }
    smp_flush_user_tlb();
    while (retired) {
        uint64_t *table = mm_phys_to_virt(retired);
        uint64_t next = table[0];
        mm_free_page(retired); retired = next;
    }
    kernel_spin_unlock(&mmio_lock);
    return true;
}

#ifdef CONFIG_MODULES
/* The module window lives in the already-shared high-half PDPT, so adding a
 * module never requires patching existing process PML4s. Physical aliases
 * share the normal identity/direct-map leaves and are protected together. */
#include <reliefnt/module_memory.h>
_Static_assert((RELIEFNT_MODULE_BASE>>39)==(RELIEFNT_KERNEL_DIRECT_MAP_BASE>>39),
               "module mappings must share the inherited kernel PML4 branch");
_Static_assert(RELIEFNT_MODULE_BASE>=RELIEFNT_KERNEL_DIRECT_MAP_BASE+RELIEFNT_KERNEL_DIRECT_MAP_SIZE,
               "module window overlaps physical direct map");
_Static_assert(((RELIEFNT_MODULE_END-1)>>39)==(RELIEFNT_MODULE_BASE>>39),
               "module window crosses the inherited branch");

/** @brief Test an aligned virtual page against the reserved module window.
 * @param address Candidate virtual page.
 * @return True for an aligned complete page in the window; no locks.
 */
static bool module_page_address(uint64_t address)
{
    return address>=RELIEFNT_MODULE_BASE && address<RELIEFNT_MODULE_END &&
           !(address&(PAGE_SIZE-1));
}
/** @brief Map one privately owned physical page RW/NX and remove executable aliases.
 * @param address Aligned virtual module page, not already mapped.
 * @param physical Allocator-owned RAM page within the supervisor direct map.
 * @param original_alias Required output retaining original alias flags for release.
 * @return True on success; does not take ownership of physical RAM. Task context.
 */
bool paging_module_map_page(uint64_t address,uint64_t physical,uint64_t *original_alias)
{
    if(!nx_enabled || !module_page_address(address) || !physical ||
       (physical&(PAGE_SIZE-1)) || !paging_kernel_direct_map_range(physical,PAGE_SIZE) ||
       !original_alias) return false;
    kernel_spin_lock(&mmio_lock);
    for(uint32_t i=0;i<mmio_run_count;++i) if(physical>=mmio_runs[i].start && physical<mmio_runs[i].end) {
        kernel_spin_unlock(&mmio_lock); return false;
    }
    uint64_t *alias=mmio_leaf(physical);
    if(!alias || !(*alias&RELIEFNT_PAGE_PRESENT) ||
       (*alias&(RELIEFNT_PAGE_USER|RELIEFNT_PAGE_PCD|RELIEFNT_PAGE_PWT))) {
        kernel_spin_unlock(&mmio_lock); return false;
    }
    uint64_t *pte=kernel_shared_leaf(address,true);
    if(!pte || *pte) { kernel_spin_unlock(&mmio_lock); return false; }
    *original_alias=*alias&~RELIEFNT_PHYS_ADDR_MASK;
    *alias|=RELIEFNT_PAGE_NOEXEC|RELIEFNT_PAGE_WRITABLE;
    *pte=physical|RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC;
    smp_flush_user_tlb();
    kernel_spin_unlock(&mmio_lock); return true;
}
/** @brief Query a stable module leaf under the shared map lock.
 * @param address Aligned module virtual page.
 * @param physical Optional output for backing page.
 * @param flags Optional output for complete non-address PTE bits.
 * @return True only for an existing present supervisor leaf; no ownership transfer.
 */
bool paging_module_page(uint64_t address,uint64_t *physical,uint64_t *flags)
{
    if(!module_page_address(address)) return false;
    kernel_spin_lock(&mmio_lock);
    uint64_t *pte=kernel_shared_leaf(address,false);
    bool found=pte && (*pte&RELIEFNT_PAGE_PRESENT) && !(*pte&RELIEFNT_PAGE_USER);
    if(found) {
        if(physical) *physical=*pte&RELIEFNT_PHYS_ADDR_MASK;
        if(flags) *flags=*pte&~RELIEFNT_PHYS_ADDR_MASK;
    }
    kernel_spin_unlock(&mmio_lock); return found;
}
/** @brief Seal module and physical aliases using a globally synchronized RO/NX intermediate state.
 * @param address Mapped module virtual page whose execution is excluded by caller.
 * @param flags Final leaf permissions; only W and NX bits are accepted.
 * @return True on success, false without changes for invalid flags/missing mapping.
 * Task context; a CPU cannot retain writable aliases when RX becomes visible.
 */
bool paging_module_protect_page(uint64_t address,uint64_t flags)
{
    if(!module_page_address(address) || flags&~(RELIEFNT_PAGE_WRITABLE|RELIEFNT_PAGE_NOEXEC) ||
       ((flags&RELIEFNT_PAGE_WRITABLE) && !(flags&RELIEFNT_PAGE_NOEXEC))) return false;
    kernel_spin_lock(&mmio_lock);
    uint64_t *pte=kernel_shared_leaf(address,false);
    uint64_t physical=pte ? *pte&RELIEFNT_PHYS_ADDR_MASK : 0;
    uint64_t *alias=physical ? mmio_existing_leaf(physical) : NULL;
    if(!pte || !(*pte&RELIEFNT_PAGE_PRESENT) || !alias) {
        kernel_spin_unlock(&mmio_lock); return false;
    }
    *alias=(*alias|RELIEFNT_PAGE_NOEXEC)&~RELIEFNT_PAGE_WRITABLE;
    *pte=physical|RELIEFNT_PAGE_PRESENT|RELIEFNT_PAGE_NOEXEC;
    smp_flush_user_tlb();
    if(flags!=RELIEFNT_PAGE_NOEXEC) {
        if(flags&RELIEFNT_PAGE_WRITABLE) *alias|=RELIEFNT_PAGE_WRITABLE;
        *pte=physical|RELIEFNT_PAGE_PRESENT|flags;
        smp_flush_user_tlb();
    }
    kernel_spin_unlock(&mmio_lock); return true;
}
/** @brief Unmap quiescent module RAM before restoring aliases and reclaiming empty tables.
 * @param address Aligned module virtual page with no admitted users/execution.
 * @param original_alias Non-address flags saved by successful map.
 * @return Removed physical page, still caller-owned, or zero for absent input.
 */
uint64_t paging_module_unmap_page(uint64_t address,uint64_t original_alias)
{
    if(!module_page_address(address)) return 0;
    kernel_spin_lock(&mmio_lock);
    uint64_t *pte=kernel_shared_leaf(address,false);
    if(!pte || !(*pte&RELIEFNT_PAGE_PRESENT)) {
        kernel_spin_unlock(&mmio_lock); return 0;
    }
    uint64_t physical=*pte&RELIEFNT_PHYS_ADDR_MASK;
    uint64_t *alias=mmio_existing_leaf(physical);
    uint64_t *pdpte=&kernel_pdpt[(address>>30)&511u];
    uint64_t pd_phys=*pdpte&RELIEFNT_PHYS_ADDR_MASK;
    uint64_t *pd=mm_phys_to_virt(pd_phys);
    uint64_t *pde=&pd[(address>>21)&511u];
    uint64_t pt_phys=*pde&RELIEFNT_PHYS_ADDR_MASK;
    uint64_t *pt=mm_phys_to_virt(pt_phys);
    *pte=0;
    bool free_pt=kernel_shared_table_empty(pt),free_pd=false;
    if(free_pt) { *pde=0; free_pd=kernel_shared_table_empty(pd); if(free_pd) *pdpte=0; }
    smp_flush_user_tlb(); /* Retire executable translations before restoring writable aliases. */
    if(alias) *alias=physical|(original_alias&~RELIEFNT_PHYS_ADDR_MASK);
    smp_flush_user_tlb();
    if(free_pt) mm_free_page(pt_phys);
    if(free_pd) mm_free_page(pd_phys);
    kernel_spin_unlock(&mmio_lock); return physical;
}
#endif

EXPORT_SYMBOL(paging_kernel_direct_map);

EXPORT_SYMBOL(paging_kernel_direct_map_range);
