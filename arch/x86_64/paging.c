/*
 * ReliefOS x86_64 paging: builds kernel and per-process address spaces.
 * Maps user pages, enforces NX/W^X permissions, and handles page protection.
 */
#include <reliefnt/mm.h>
#include <reliefnt/page_cache.h>
#include <reliefnt/paging.h>
#include <reliefnt/smp.h>
#include <reliefnt/lock.h>

#define PAGE_SIZE 4096ULL
#define PAGE_SIZE_2M 0x200000ULL
#define PAGE_SIZE_FLAG 0x080ULL
#define USER_PDPT_INDEX 0
#define LOW_PD_INDEX 0
#define KERNEL_DIRECT_PML4_INDEX 511u
#define KERNEL_PD_COUNT 16u /* 16 GiB identity map using 2 MiB leaves. */
#define X86_EFER_MSR 0xc0000080u
#define X86_EFER_NXE (1ULL << 11)

static uint64_t kernel_pml4[512] __attribute__((aligned(4096)));
static uint64_t kernel_pdpt[512] __attribute__((aligned(4096)));
static uint64_t kernel_pd[KERNEL_PD_COUNT][512] __attribute__((aligned(4096)));

extern void x86_64_load_cr3(uint64_t cr3);
extern void x86_64_invlpg(uint64_t addr);

/* All first-GiB kernel leaves are split before any user CR3 copies its PDEs.
 * Later BAR attribute changes remain visible through every copied low PDE. */
static uint64_t kernel_low_pt[512][512] __attribute__((aligned(4096)));
#define MMIO_RUN_MAX 256U
#define MMIO_LEASE_MAX 128U
struct mmio_run {
    uint64_t start, end, original_flags;
    uint32_t refs, permanent;
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
    uint8_t *dst = (uint8_t *)(uintptr_t)destination;
    const uint8_t *src = (const uint8_t *)(uintptr_t)source;
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

bool paging_kernel_direct_map_range(uint64_t phys, uint64_t len)
{
    return phys < RELIEFNT_KERNEL_DIRECT_MAP_SIZE &&
           len <= RELIEFNT_KERNEL_DIRECT_MAP_SIZE - phys;
}

void *paging_kernel_direct_map(uint64_t phys)
{
    if (!paging_kernel_direct_map_range(phys, 1)) {
        return NULL;
    }
    return (void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + phys);
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
        uint64_t page = mm_alloc_page();
        if (!page) return NULL;
        uint64_t *pt = (void *)(uintptr_t)page;
        uint64_t base = *pde & RELIEFNT_PHYS_ADDR_MASK & ~(PAGE_SIZE_2M - 1);
        uint64_t flags = *pde & ~RELIEFNT_PHYS_ADDR_MASK & ~PAGE_SIZE_FLAG;
        /* Large-page PAT is bit 12; in a 4 KiB PTE it is bit 7. */
        if (*pde & (1ULL << 12)) flags |= RELIEFNT_PAGE_PAT;
        for (uint32_t i = 0; i < 512; ++i) pt[i] = (base + i * PAGE_SIZE) | flags;
        __atomic_store_n(pde, page | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE, __ATOMIC_RELEASE);
    }
    if (!(*pde & RELIEFNT_PAGE_PRESENT)) return NULL;
    return &((uint64_t *)(uintptr_t)(*pde & RELIEFNT_PHYS_ADDR_MASK))[(phys >> 12) & 511];
}
/** @brief Return a leaf only when its PDE has already been split.
 * @param phys Physical address in the kernel direct map.
 * @return Borrowed leaf PTE or NULL. mmio_lock held; does not allocate.
 */
static uint64_t *mmio_existing_leaf(uint64_t phys)
{
    uint64_t *pde = &kernel_pd[phys >> 30][(phys >> 21) & 511];
    if (!(*pde & RELIEFNT_PAGE_PRESENT) || (*pde & PAGE_SIZE_FLAG)) return NULL;
    return &((uint64_t *)(uintptr_t)(*pde & RELIEFNT_PHYS_ADDR_MASK))[(phys >> 12) & 511];
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
            previous->refs == run->refs && previous->permanent == run->permanent) {
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
                                  uint32_t *count)
{
    uint64_t run_start = start, original_flags = 0;
    bool have_run = false;
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t *pte = mmio_existing_leaf(p);
        if (!pte) return false;
        uint64_t flags = *pte & ~RELIEFNT_PHYS_ADDR_MASK;
        if (have_run && flags != original_flags) {
            struct mmio_run run = {run_start, p, original_flags,
                                   permanent ? 0u : 1u, permanent ? 1u : 0u};
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
                               permanent ? 0u : 1u, permanent ? 1u : 0u};
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
                              uint32_t *count)
{
    uint32_t out = 0;
    uint64_t cursor = start;
    for (uint32_t i = 0; i < mmio_run_count; ++i) {
        const struct mmio_run *old = &mmio_runs[i];
        if (old->end <= start || old->start >= end) {
            if (old->start >= end && cursor < end) {
                if (!mmio_append_new_range(cursor, end, permanent, &out)) return false;
                cursor = end;
            }
            if (!mmio_run_append(old, &out)) return false;
            continue;
        }

        uint64_t first = old->start < start ? start : old->start;
        uint64_t last = old->end > end ? end : old->end;
        if (cursor < first) {
            if (!mmio_append_new_range(cursor, first, permanent, &out)) return false;
        }
        if (old->start < first) {
            struct mmio_run prefix = *old;
            prefix.end = first;
            if (!mmio_run_append(&prefix, &out)) return false;
        }

        struct mmio_run overlap = *old;
        overlap.start = first;
        overlap.end = last;
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
    if (cursor < end && !mmio_append_new_range(cursor, end, permanent, &out)) return false;
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
static bool mmio_acquire(uint64_t phys, uint64_t len, bool permanent)
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
    if (!mmio_plan_acquire(phys, end, permanent, &run_count) ||
        !mmio_registry_fits(run_count, lease_count) || !mmio_verify_leaves(phys, end)) {
        smp_flush_user_tlb();
        kernel_spin_unlock(&mmio_lock);
        return false;
    }

    for (uint64_t p = phys; p < end; p += PAGE_SIZE) {
        uint64_t *pte = mmio_existing_leaf(p);
        *pte = (*pte & ~RELIEFNT_PAGE_PAT) | RELIEFNT_PAGE_PWT |
               RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC;
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
    return mmio_acquire(phys, len, false);
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
    return end > start && mmio_acquire(start, end - start, true);
}

/**
 * Alloc table.
 * @return The value or status produced by the operation.
 */
static uint64_t alloc_table(void)
{
    return mm_alloc_page();
}

/**
 * Address space create.
 * @param as Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
bool address_space_create(struct address_space *as)
{
    if (!as) {
        return false;
    }
    for (size_t i = 0; i < sizeof(*as); ++i) {
        ((uint8_t *)as)[i] = 0;
    }

    uint64_t pml4_phys = alloc_table();
    uint64_t pdpt_phys = alloc_table();
    if (!pml4_phys || !pdpt_phys) {
        if (pml4_phys) {
            mm_free_page(pml4_phys);
        }
        if (pdpt_phys) {
            mm_free_page(pdpt_phys);
        }
        return false;
    }

    as->pml4 = (uint64_t *)(uintptr_t)pml4_phys;
    as->pdpt = (uint64_t *)(uintptr_t)pdpt_phys;
    as->cr3 = pml4_phys;
    for (uint32_t i = 0; i < 512; ++i) {
        as->pml4[i] = kernel_pml4[i];
        as->pdpt[i] = kernel_pdpt[i];
    }
    as->pml4[USER_PDPT_INDEX] = pdpt_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_USER;

    uint64_t low_pd_phys = alloc_table();
    if (!low_pd_phys) {
        address_space_destroy(as);
        return false;
    }
    as->pd[LOW_PD_INDEX] = (uint64_t *)(uintptr_t)low_pd_phys;
    for (uint32_t i = 0; i < 512; ++i) {
        as->pd[LOW_PD_INDEX][i] = kernel_pd[0][i];
    }
    as->pdpt[USER_PDPT_INDEX] = low_pd_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_USER;

    return true;
}

/**
 * @brief Clone user mappings and write-protect private pages across all CPUs.
 * Caller holds the kernel execution transaction, including on failure; remote
 * CPUs acknowledge TLB invalidation before the child can become runnable.
 * @param source Parent address space whose writable mappings are write-protected in place.
 * @param destination Empty output address space with independently allocated page tables.
 * @return True if all mappings were cloned; false after releasing the partial destination.
 */
bool address_space_clone_cow(struct address_space *source, struct address_space *destination)
{
    if (!source || !destination || !source->pml4 || !source->pdpt) {
        return false;
    }
    if (!address_space_create(destination)) {
        return false;
    }
    for (uint32_t table = 0; table < RELIEFNT_USER_PD_COUNT; ++table) {
        uint64_t base = RELIEFNT_USER_BASE + (uint64_t)table * RELIEFNT_USER_PD_BYTES;
        if (!source->user_pt[table]) {
            continue;
        }
        for (uint32_t slot = 0; slot < 512; ++slot) {
            uint64_t entry = source->user_pt[table][slot];
            uint64_t flags;
            uint64_t phys;
            uint64_t page;
            int cached;
            if (!(entry & RELIEFNT_PAGE_BACKED)) {
                continue;
            }
            phys = entry & RELIEFNT_PHYS_ADDR_MASK;
            flags = entry & (RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC |
                             RELIEFNT_PAGE_COW | RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_PROTNONE |
                             RELIEFNT_PAGE_SHARED |
                             RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT);
            page = base + (uint64_t)slot * PAGE_SIZE;
            if (!(entry & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED)) &&
                ((entry & RELIEFNT_PAGE_WRITABLE) || (entry & RELIEFNT_PAGE_COW))) {
                flags &= ~RELIEFNT_PAGE_WRITABLE;
                flags |= RELIEFNT_PAGE_COW;
                source->user_pt[table][slot] = phys | (entry & RELIEFNT_PAGE_PRESENT) |
                                               RELIEFNT_PAGE_USER | flags;
                x86_64_invlpg(page); paging_note_mm_change();
            }
            if (entry & RELIEFNT_PAGE_DEVICE) {
                cached = 0;
            } else {
                cached = page_cache_retain(phys) == 0;
                if (!cached) {
                    mm_retain_page(phys);
                }
            }
            if (!address_space_map_user_page(destination, page, phys, flags)) {
                if (entry & RELIEFNT_PAGE_DEVICE) {
                    /* Device pages are borrowed from the framebuffer. */
                } else if (cached) {
                    page_cache_release(phys);
                } else {
                    mm_free_page(phys);
                }
                address_space_destroy(destination);
                /* Earlier entries in the parent may already be read-only. */
                smp_flush_user_tlb();
                return false;
            }
        }
    }
    /* A CLONE_VM sibling may retain writable translations of the parent's
     * newly COW pages. The caller holds the execution transaction; invalidate
     * remote translations before publishing the child to the scheduler. */
    smp_flush_user_tlb();
    return true;
}

/**
 * Address space destroy.
 * @param as Value supplied by the caller.
 */
void address_space_destroy(struct address_space *as)
{
    if (!as) {
        return;
    }
    for (uint32_t table = 0; table < RELIEFNT_USER_PD_COUNT; ++table) {
        if (as->user_pt[table]) {
            for (uint32_t i = 0; i < 512; ++i) {
                uint64_t entry = as->user_pt[table][i];
                if (entry & RELIEFNT_PAGE_BACKED) {
                    if (entry & RELIEFNT_PAGE_DEVICE) {
                        /* Device mappings refer to reserved physical memory. */
                    } else if (page_cache_owns(entry & RELIEFNT_PHYS_ADDR_MASK)) {
                        page_cache_release(entry & RELIEFNT_PHYS_ADDR_MASK);
                    } else {
                        mm_free_page(entry & RELIEFNT_PHYS_ADDR_MASK);
                    }
                }
            }
            mm_free_page((uint64_t)(uintptr_t)as->user_pt[table]);
        }
    }
    if (as->pd[LOW_PD_INDEX]) {
        mm_free_page((uint64_t)(uintptr_t)as->pd[LOW_PD_INDEX]);
    }
    if (as->pdpt) {
        mm_free_page((uint64_t)(uintptr_t)as->pdpt);
    }
    if (as->pml4) {
        mm_free_page((uint64_t)(uintptr_t)as->pml4);
    }
    for (size_t i = 0; i < sizeof(*as); ++i) {
        ((uint8_t *)as)[i] = 0;
    }
}

/**
 * @brief Prepare missing user page tables without partial allocation on failure.
 * @param as Address space pinned by the execution transaction.
 * @param start First user byte in the range.
 * @param end Exclusive range end.
 * @return True after publishing all missing tables; false leaves existing
 * tables unchanged and returns every page allocated by this operation.
 */
bool address_space_prepare_user_range(struct address_space *as, uint64_t start,
                                      uint64_t end)
{
    uint64_t first_page;
    uint64_t last_page;
    uint64_t first_table;
    uint64_t last_table;
    /* The user window fits one directory, bounding this transaction to less
     * than 4 KiB on the 128 KiB syscall stack. Existing tables are borrowed. */
    uint64_t prepared[RELIEFNT_USER_PD_COUNT] = {0};

    if (!as || start < RELIEFNT_USER_BASE || start >= end || end > RELIEFNT_USER_TOP ||
        (start < RELIEFNT_KERNEL_HOLE_END && end > RELIEFNT_KERNEL_HOLE_START)) {
        return false;
    }
    first_page = align_down(start, PAGE_SIZE);
    last_page = align_down(end - 1ULL, PAGE_SIZE);
    first_table = (first_page - RELIEFNT_USER_BASE) / PAGE_SIZE / 512ULL;
    last_table = (last_page - RELIEFNT_USER_BASE) / PAGE_SIZE / 512ULL;
    if (last_table >= RELIEFNT_USER_PD_COUNT) {
        return false;
    }
    for (uint64_t table = first_table; table <= last_table; ++table) {
        if (!as->user_pt[table]) {
            uint64_t pt_phys = alloc_table();
            if (!pt_phys) {
                for (uint64_t prior = first_table; prior < table; ++prior)
                    if (prepared[prior]) mm_free_page(prepared[prior]);
                return false;
            }
            prepared[table] = pt_phys;
        }
    }
    for (uint64_t table = first_table; table <= last_table; ++table) {
        if (prepared[table]) {
            as->user_pt[table] = (uint64_t *)(uintptr_t)prepared[table];
            as->pd[LOW_PD_INDEX][RELIEFNT_USER_PD_START + table] =
                prepared[table] | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_USER;
            x86_64_invlpg(RELIEFNT_USER_BASE + table * RELIEFNT_USER_PD_BYTES);
            paging_note_mm_change();
        }
    }
    return true;
}

/**
 * Address space map user page.
 * @param as Value supplied by the caller.
 * @param vaddr Value supplied by the caller.
 * @param phys Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
bool address_space_map_user_page(struct address_space *as, uint64_t vaddr,
                                 uint64_t phys, uint64_t flags)
{
    if (!as || !phys || (phys & (PAGE_SIZE - 1))) {
        return false;
    }
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP ||
        (page >= RELIEFNT_KERNEL_HOLE_START && page < RELIEFNT_KERNEL_HOLE_END)) {
        return false;
    }
    uint64_t index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    uint64_t slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT) {
        return false;
    }
    if (!as->user_pt[table] &&
        !address_space_prepare_user_range(as, page, page + PAGE_SIZE)) {
        return false;
    }
    if (as->user_pt[table][slot] & RELIEFNT_PAGE_BACKED) {
        return false;
    }
    if ((flags & RELIEFNT_PAGE_WRITABLE) && !(flags & RELIEFNT_PAGE_NOEXEC)) {
        return false;
    }
    if (!nx_enabled) {
        /* Executable user mappings are unsafe without NX because the kernel
         * cannot enforce W^X.  Refuse the process rather than weaken it. */
        return false;
    }
    as->user_pt[table][slot] = phys | RELIEFNT_PAGE_USER | flags |
        ((flags & RELIEFNT_PAGE_PROTNONE) ? 0 : RELIEFNT_PAGE_PRESENT);
    ++as->user_page_count;
    x86_64_invlpg(page); paging_note_mm_change();
    return true;
}

/**
 * Address space protect user page.
 * @param as Value supplied by the caller.
 * @param vaddr Value supplied by the caller.
 * @param flags Identifier or flags controlling the operation.
 * @return The value or status produced by the operation.
 */
bool address_space_protect_user_page(struct address_space *as, uint64_t vaddr,
                                     uint64_t flags)
{
    uint64_t page;
    uint64_t index;
    uint64_t table;
    uint64_t slot;
    uint64_t entry;
    if (!as || ((flags & RELIEFNT_PAGE_WRITABLE) && !(flags & RELIEFNT_PAGE_NOEXEC))) {
        return false;
    }
    page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return false;
    }
    index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    table = index / 512;
    slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) {
        return false;
    }
    entry = as->user_pt[table][slot];
    if (!(entry & RELIEFNT_PAGE_BACKED)) {
        return false;
    }
    /* Read-only and inaccessible pages can still be shared after fork, or
     * borrowed from the file cache. An upgrade must copy before writing. */
    if ((flags & RELIEFNT_PAGE_WRITABLE) && !(entry & RELIEFNT_PAGE_WRITABLE) &&
        !(entry & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED))) {
        flags = (flags & ~RELIEFNT_PAGE_WRITABLE) | RELIEFNT_PAGE_COW;
    }
    flags |= entry & (RELIEFNT_PAGE_DEVICE | RELIEFNT_PAGE_SHARED | RELIEFNT_PAGE_PWT |
                      RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_PAT);
    as->user_pt[table][slot] = (entry & RELIEFNT_PHYS_ADDR_MASK) |
        RELIEFNT_PAGE_USER | flags |
        ((flags & RELIEFNT_PAGE_PROTNONE) ? 0 : RELIEFNT_PAGE_PRESENT);
    x86_64_invlpg(page); paging_note_mm_change();
    return true;
}

/**
 * Address space unmap user page.
 * @param as Value supplied by the caller.
 * @param vaddr Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t address_space_unmap_user_page(struct address_space *as, uint64_t vaddr)
{
    if (!as) {
        return 0;
    }
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return 0;
    }
    uint64_t index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    uint64_t slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) {
        return 0;
    }
    uint64_t entry = as->user_pt[table][slot];
    if (!(entry & RELIEFNT_PAGE_BACKED)) {
        return 0;
    }
    as->user_pt[table][slot] = 0;
    if (as->user_page_count) {
        --as->user_page_count;
    }
    x86_64_invlpg(page); paging_note_mm_change();
    return entry & RELIEFNT_PHYS_ADDR_MASK;
}

/**
 * Address space user page phys.
 * @param as Value supplied by the caller.
 * @param vaddr Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint64_t address_space_user_page_phys(const struct address_space *as, uint64_t vaddr)
{
    if (!as) {
        return 0;
    }
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return 0;
    }
    uint64_t index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    uint64_t slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) {
        return 0;
    }
    uint64_t entry = as->user_pt[table][slot];
    return (entry & RELIEFNT_PAGE_BACKED) ? (entry & RELIEFNT_PHYS_ADDR_MASK) : 0;
}

bool address_space_retry_mapped_user_page(const struct address_space *as,
                                          uint64_t vaddr, uint64_t fault_error)
{
    if (!as || (fault_error & (1ULL | 8ULL)) ||
        vaddr < RELIEFNT_USER_BASE || vaddr >= RELIEFNT_USER_TOP) return false;
    uint64_t index = (vaddr - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) return false;
    uint64_t entry = as->user_pt[table][index % 512];
    if ((entry & (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER)) !=
        (RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER)) return false;
    if ((fault_error & 2ULL) && !(entry & RELIEFNT_PAGE_WRITABLE)) return false;
    if ((fault_error & 16ULL) && (entry & RELIEFNT_PAGE_NOEXEC)) return false;
    /* A peer sharing this mm may have resolved the same fault before this
     * CPU acquired the fault lock. Discard its old translation before retry. */
    x86_64_invlpg(align_down(vaddr, PAGE_SIZE));
    paging_note_mm_change();
    return true;
}

bool address_space_user_page_readable(const struct address_space *as, uint64_t vaddr)
{
    if (!as || vaddr < RELIEFNT_USER_BASE || vaddr >= RELIEFNT_USER_TOP) return false;
    uint64_t index = (vaddr - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) return false;
    uint64_t required = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER;
    return (as->user_pt[table][index % 512] & required) == required;
}

/**
 * @brief Inspect write permission on an existing user page without changing it.
 * @param as Address space, or NULL.
 * @param vaddr Address within the queried page.
 * @return True for present user-writable PTEs; false for COW or absent pages.
 */
bool address_space_user_page_writable(const struct address_space *as, uint64_t vaddr)
{
    if (!as || vaddr < RELIEFNT_USER_BASE || vaddr >= RELIEFNT_USER_TOP) return false;
    uint64_t index = (vaddr - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) return false;
    uint64_t required = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER | RELIEFNT_PAGE_WRITABLE;
    return (as->user_pt[table][index % 512] & required) == required;
}

bool address_space_user_page_is_device(const struct address_space *as, uint64_t vaddr)
{
    if (!as) {
        return false;
    }
    uint64_t page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return false;
    }
    uint64_t index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    uint64_t table = index / 512;
    uint64_t slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) {
        return false;
    }
    uint64_t entry = as->user_pt[table][slot];
    return (entry & RELIEFNT_PAGE_BACKED) && (entry & RELIEFNT_PAGE_DEVICE);
}

/**
 * Address space user memory kib.
 * @param as Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t address_space_user_memory_kib(const struct address_space *as)
{
    if (!as) {
        return 0;
    }
    return as->user_page_count * 4U;
}

/** @brief Count resident RAM pages, including owned PROT_NONE and shared pages. */
uint32_t address_space_user_resident_kib(const struct address_space *as)
{
    uint32_t pages = 0;
    if (!as) return 0;
    for (uint32_t table = 0; table < RELIEFNT_USER_PD_COUNT; ++table) {
        if (!as->user_pt[table]) continue;
        for (uint32_t slot = 0; slot < 512; ++slot) {
            uint64_t entry = as->user_pt[table][slot];
            if ((entry & RELIEFNT_PAGE_BACKED) && !(entry & RELIEFNT_PAGE_DEVICE)) ++pages;
        }
    }
    return pages * 4U;
}

/**
 * Address space map user stack.
 * @param as Value supplied by the caller.
 * @param stack_top Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
bool address_space_map_user_stack(struct address_space *as, uint64_t stack_top)
{
    if (!as || stack_top <= (uint64_t)RELIEFNT_USER_STACK_PAGES * PAGE_SIZE ||
        stack_top > RELIEFNT_USER_TOP) {
        return false;
    }
    uint64_t first = stack_top - (uint64_t)RELIEFNT_USER_STACK_PAGES * PAGE_SIZE;
    for (uint32_t i = 0; i < RELIEFNT_USER_STACK_PAGES; ++i) {
        uint64_t phys = mm_alloc_page();
        if (!phys || !address_space_map_user_page(as, first + (uint64_t)i * PAGE_SIZE,
                                                  phys, RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC)) {
            if (phys) {
                mm_free_page(phys);
            }
            return false;
        }
    }
    return true;
}

bool address_space_map_user_stack_page(struct address_space *as, uint64_t page)
{
    uint64_t phys;
    if (!as || (page & (PAGE_SIZE - 1ULL)) || page < RELIEFNT_USER_BASE ||
        page >= RELIEFNT_USER_TOP) {
        return false;
    }
    if (address_space_user_page_phys(as, page)) {
        return true;
    }
    phys = mm_alloc_page();
    if (!phys) {
        return false;
    }
    if (!address_space_map_user_page(as, page, phys,
                                     RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC)) {
        mm_free_page(phys);
        return false;
    }
    return true;
}

/**
 * @brief Resolves a write fault by copying a shared COW page into the faulting address space.
 * @param as Address space that owns the faulting mapping.
 * @param vaddr User address within the affected page.
 * @return True when the page is now writable and private to this address space.
 */
bool address_space_handle_cow_fault(struct address_space *as, uint64_t vaddr)
{
    uint64_t page;
    uint64_t index;
    uint64_t table;
    uint64_t slot;
    uint64_t entry;
    uint64_t old_phys;
    uint64_t new_phys;
    if (!as) {
        return false;
    }
    page = align_down(vaddr, PAGE_SIZE);
    if (page < RELIEFNT_USER_BASE || page >= RELIEFNT_USER_TOP) {
        return false;
    }
    index = (page - RELIEFNT_USER_BASE) / PAGE_SIZE;
    table = index / 512;
    slot = index % 512;
    if (table >= RELIEFNT_USER_PD_COUNT || !as->user_pt[table]) {
        return false;
    }
    entry = as->user_pt[table][slot];
    if (!(entry & RELIEFNT_PAGE_PRESENT) || !(entry & RELIEFNT_PAGE_COW)) {
        return false;
    }
    old_phys = entry & RELIEFNT_PHYS_ADDR_MASK;
    new_phys = mm_alloc_page();
    if (!new_phys) {
        return false;
    }
    copy_page(new_phys, old_phys);
    as->user_pt[table][slot] = new_phys | RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_USER |
                               RELIEFNT_PAGE_WRITABLE | RELIEFNT_PAGE_NOEXEC |
                               (entry & (RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD |
                                         RELIEFNT_PAGE_PAT));
    x86_64_invlpg(page); paging_note_mm_change();
    if (page_cache_owns(old_phys)) page_cache_release(old_phys);
    else mm_free_page(old_phys);
    return true;
}
