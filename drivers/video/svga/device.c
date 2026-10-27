#include <reliefnt/export.h>
#include "device.h"
#include <reliefnt/console.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/mm.h>
#ifndef SVGA_HOST_TEST
#include <reliefnt/lock.h>
#include <reliefnt/paging.h>
#include <reliefnt/port.h>
#include <reliefnt/pci.h>
static struct kernel_spinlock device_lock = KERNEL_SPINLOCK_INIT;
static uint16_t io_port;
#else
static volatile uint32_t host_device_lock;
#endif

struct svga_device svga;

/** @brief Match the physical FIFO reservation against its kernel CPU alias.
 * @param fb Framebuffer descriptor carrying the physical auxiliary range.
 * @param fifo Kernel direct-map pointer bound to the SVGA protocol layer.
 * @param bytes Bound FIFO size.
 * @return True only when the alias resolves to the exact reserved PFN and size.
 */
bool svga_fifo_reservation_matches(const struct framebuffer *fb,
                                   const volatile uint32_t *fifo,
                                   uint32_t bytes)
{
    uint64_t physical = fifo
        ? mm_virt_to_phys((const void *)(uintptr_t)fifo) : 0;
    return fb && physical && fb->auxiliary_reservation_start == physical &&
           fb->auxiliary_reservation_bytes == bytes;
}

uint64_t svga_lock(void)
{
#ifdef SVGA_HOST_TEST
    uint32_t expected;
    do {
        expected = 0;
        if (__atomic_compare_exchange_n(&host_device_lock, &expected, 1, false,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            return 0;
        }
        svga_relax();
    } while (true);
#else
    uint64_t flags;
    kernel_spin_lock_irqsave(&device_lock, &flags);
    return flags;
#endif
}
bool svga_try_lock_irqsave(uint64_t *flags)
{
    if (!flags) return false;
#ifdef SVGA_HOST_TEST
    uint32_t expected = 0;
    *flags = 0;
    return __atomic_compare_exchange_n(&host_device_lock, &expected, 1, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
#else
    uint32_t expected = 0;
    *flags = kernel_irq_save();
    if (__atomic_compare_exchange_n(&device_lock.state, &expected, 1, false,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return true;
    }
    kernel_irq_restore(*flags);
    return false;
#endif
}
void svga_unlock(uint64_t flags)
{
#ifdef SVGA_HOST_TEST
    (void)flags;
    __atomic_store_n(&host_device_lock, 0, __ATOMIC_RELEASE);
#else
    kernel_spin_unlock_irqrestore(&device_lock, flags);
#endif
}
void svga_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void svga_relax(void) { __asm__ volatile("pause" ::: "memory"); }

#ifndef SVGA_HOST_TEST
static uint32_t port_read(uint32_t reg)
{
    x86_64_outl(reg, io_port);
    return x86_64_inl(io_port + 1);
}
static void port_write(uint32_t reg, uint32_t value)
{
    x86_64_outl(reg, io_port);
    x86_64_outl(value, io_port + 1);
}
static uint64_t device_clock(void)
{
    uint32_t low, high;
    __asm__ volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) :: "memory");
    return ((uint64_t)high << 32) | low;
}
void svga_platform_bind(uint16_t port, volatile uint32_t *fifo, uint32_t bytes)
{
    io_port = port;
    const struct svga_ops ops = {port_read, port_write, mm_alloc_pages,
                                 mm_free_pages, paging_kernel_direct_map, device_clock};
    svga_bind(&ops, fifo, bytes);
}
#endif

void svga_bind(const struct svga_ops *ops, volatile uint32_t *fifo, uint32_t bytes)
{
    /* A rebind has no inherited FIFO completion evidence. */
    svga_fence_quiescence_reset();
    svga.ops = *ops;
    svga.fifo = fifo;
    svga.fifo_bytes = bytes;
    svga.bound = true;
    svga.traces_enabled = false;
}

/** @brief Restore the host's trace-based guest-framebuffer update policy. */
bool svga_restore_traces_locked(void)
{
    if (!svga.bound) {
        svga.traces_enabled = false;
        return false;
    }
    svga.caps = svga.ops.read(SVGA_REG_CAPABILITIES);
    svga.traces_enabled = (svga.caps & SVGA_CAP_TRACES) != 0;
    if (svga.traces_enabled) svga.ops.write(SVGA_REG_TRACES, 1);
    return svga.traces_enabled;
}

/* Record the original failure before a four-register 2D FIFO replaces it. */
static int probe_result(const char *stage, int status)
{
    svga.probe.stage = stage;
    svga.probe.status = status;
    return status;
}

#ifndef SVGA_HOST_TEST
#define SVGA_CACHE_PAGE_SIZE 4096ULL
/** @brief Round a physical byte range to pages and validate the direct map.
 * @param phys Inclusive byte start.
 * @param bytes Nonzero byte length.
 * @param page_start Receives the rounded inclusive page start.
 * @param page_length Receives the nonzero rounded length.
 * @return True when rounding is overflow-safe and the complete range is in the
 * kernel direct map.
 */
static bool svga_page_range(uint64_t phys, uint64_t bytes,
                            uint64_t *page_start, uint64_t *page_length)
{
    if (!bytes || bytes > UINT64_MAX - phys) return false;
    uint64_t end = phys + bytes;
    if (end > UINT64_MAX - (SVGA_CACHE_PAGE_SIZE - 1)) return false;
    uint64_t start = phys & ~(SVGA_CACHE_PAGE_SIZE - 1);
    uint64_t rounded_end = (end + SVGA_CACHE_PAGE_SIZE - 1) &
                           ~(SVGA_CACHE_PAGE_SIZE - 1);
    if (rounded_end <= start ||
        !paging_kernel_direct_map_range(start, rounded_end - start)) return false;
    *page_start = start;
    *page_length = rounded_end - start;
    return true;
}

/** @brief Test overlap between two already validated page ranges.
 * @param a First aligned page-range start.
 * @param a_length First nonzero page-range length.
 * @param b Second aligned page-range start.
 * @param b_length Second nonzero page-range length.
 * @return True when the half-open ranges overlap.
 */
static bool svga_page_ranges_overlap(uint64_t a, uint64_t a_length,
                                     uint64_t b, uint64_t b_length)
{
    return a < b + b_length && b < a + a_length;
}

/** @brief Quiesce the legacy FIFO before a PCI BAR size probe.
 * @return Zero after BUSY clears and CONFIG_DONE is disabled, or ETIMEDOUT.
 * Caller holds svga_lock; no BAR decode change occurs on timeout.
 */
static int svga_quiesce_fifo_for_bar_probe_locked(void)
{
    svga.ops.write(SVGA_REG_SYNC, 1);
    for (uint32_t i = 0; i < SVGA_POLL_LIMIT; ++i) {
        if (!svga.ops.read(SVGA_REG_BUSY)) {
            svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
            svga.traces_enabled = false;
            return 0;
        }
        svga_relax();
    }
    return SVGA_ETIMEDOUT;
}

/** @brief Install the FIFO's safe default-WB policy or UC fallback.
 * @param fb Current framebuffer physical reservations.
 * @param report Optional caller-owned output report, zeroed and populated when
 *        non-NULL; borrowed only for this call and never retained. This helper
 *        records diagnostics there but does not print while svga_lock is held.
 * @return Zero when WB or legacy UC/NX is installed, otherwise ENOTSUP.
 * Called only after shutdown and legacy FIFO drain under svga_lock. The
 * scan-out reservation remains UC/NX; the FIFO keeps its supervisor mapping
 * and is made non-executable when WB+NX succeeds.
 */
static int svga_prepare_fifo_cache_policy(const struct framebuffer *fb,
                                          struct svga_cache_report *report)
{
    if (report) *report = (struct svga_cache_report){0};
    if (!svga_fifo_reservation_matches(fb, svga.fifo, svga.fifo_bytes)) {
        if (report) report->reason = "reservation-mismatch";
        return SVGA_ENOTSUP;
    }

    uint64_t fifo_page, fifo_page_bytes, vram_page, vram_page_bytes;
    if (!svga_page_range(fb->auxiliary_reservation_start,
                         fb->auxiliary_reservation_bytes,
                         &fifo_page, &fifo_page_bytes) ||
        !svga_page_range(fb->reservation_start, fb->reservation_bytes,
                         &vram_page, &vram_page_bytes)) {
        if (report) report->reason = "physical-range";
        return SVGA_ENOTSUP;
    }

    if (!paging_mmio_uncached(fb->reservation_start, fb->reservation_bytes)) {
        if (report) report->reason = "vram-uc-pin";
        return SVGA_ENOTSUP;
    }

    const char *reason = NULL;
    if (svga_page_ranges_overlap(fifo_page, fifo_page_bytes,
                                       vram_page, vram_page_bytes)) {
        reason = "overlaps-vram";
    } else {
        struct pci_device primary;
        if (framebuffer_vmware_pci_identity(&primary) < 0) {
            reason = "no-bound-framebuffer";
        } else if (primary.vendor_id != 0x15adu || primary.device_id != 0x0405u) {
            reason = "primary-identity";
        } else if (!pci_device_bar_contains(&primary, 2, fifo_page,
                                             fifo_page_bytes)) {
            reason = "bar2-range";
        } else if (paging_mmio_writeback_nx(fifo_page, fifo_page_bytes)) {
            if (report)
                report->policy = "default-PTE WB+NX requested for validated BAR2";
            return 0;
        } else {
            reason = "wb-policy-or-capacity";
        }
    }

    if (!paging_mmio_uncached(fb->auxiliary_reservation_start,
                              fb->auxiliary_reservation_bytes)) {
        if (report) report->reason = "fifo-uc-pin";
        return SVGA_ENOTSUP;
    }
    if (report) {
        report->policy = "UC+NX fallback";
        report->reason = reason ? reason : "unknown";
    }
    return 0;
}
#endif

/** @brief Emit a cache-policy outcome after the SVGA lock has been released.
 * @param report Selected policy or cache setup/restoration failure; NULL is ignored.
 * Console presentation can re-enter the framebuffer and acquire svga_lock.
 */
void svga_report_cache_result_unlocked(const struct svga_cache_report *report)
{
    if (!report) return;
    if (!report->status) {
        if (!report->policy) return;
        if (report->reason) {
            console_log(CONSOLE_LOG_WARN,
                        "[svga3d] FIFO cache policy: %s reason=%s\n",
                        report->policy, report->reason);
        } else {
            console_log(CONSOLE_LOG_INFO,
                        "[svga3d] FIFO cache policy: %s\n", report->policy);
        }
        return;
    }
    if (report->legacy_restored) {
        console_log(CONSOLE_LOG_WARN,
                    "[svga3d] FIFO cache setup failed reason=%s status=%d; legacy 2D restored\n",
                    report->reason ? report->reason : "unknown", report->status);
    } else {
        console_log(CONSOLE_LOG_ERROR,
                    "[svga3d] FIFO cache setup failed reason=%s status=%d; legacy 2D restore failed\n",
                    report->reason ? report->reason : "unknown", report->status);
    }
}

/** @brief Restore the legacy FIFO after a post-drain cache-policy failure.
 * @param cache_status Original cache-validation or mapping failure.
 * @return The original failure when legacy reconfiguration succeeds, otherwise
 *         SVGA_EIO. Caller holds svga_lock; this preserves working 2D scanout.
 */
int svga_restore_legacy_after_cache_error_locked(int cache_status)
{
    if (!cache_status) return 0;
    return svga_configure_locked(false) == 0 ? cache_status : SVGA_EIO;
}

int svga_configure_locked(bool extended)
{
    svga.fifo_busy_present = false;
    if (svga.gb_active) return SVGA_EBUSY;
    if (extended) {
        svga.probe = (struct svga_probe_info){.stage = "fifo-setup", .status = SVGA_EIO};
    }
    if (!svga.bound || !svga.fifo || svga.fifo_bytes < 16384 ||
        (svga.fifo_bytes & 3)) {
        if (extended) probe_result("fifo-mapping", SVGA_ENODEV);
        return SVGA_ENODEV;
    }
    svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
    svga.traces_enabled = false;
    svga.fifo_ready = false;
    svga.available = false;
    svga.caps = svga.ops.read(SVGA_REG_CAPABILITIES);
    if (extended) svga.probe.enable = svga.ops.read(SVGA_REG_ENABLE);
    /* Keep the legacy path's four DWORD header. SVGA_FIFO_MIN is a byte
     * offset, so this yields the 16-byte minimum used by the 2D adapter. */
    bool extended_fifo = extended &&
        (svga.caps & SVGA_CAP_EXTENDED_FIFO) != 0;
    uint32_t raw_mem_regs = 0;
    uint32_t regs = 4;
    if (extended_fifo) {
        raw_mem_regs = svga.ops.read(SVGA_REG_MEM_REGS);
        svga.probe.mem_regs = raw_mem_regs;
        regs = raw_mem_regs;
        if (regs < SVGA_FIFO_NUM_REGS) regs = SVGA_FIFO_NUM_REGS;
        if (regs > (svga.fifo_bytes - 10240) / 4)
            return probe_result("fifo-registers", SVGA_EIO);
    }
    svga.min = regs * 4;
    if (extended) svga.probe.fifo_min = svga.min;
    const uint64_t fifo_busy_end =
        ((uint64_t)SVGA_FIFO_BUSY + 1u) * sizeof(uint32_t);
    bool fifo_busy_present = extended_fifo &&
        raw_mem_regs > SVGA_FIFO_BUSY &&
        fifo_busy_end <= svga.min && fifo_busy_end <= svga.fifo_bytes &&
        svga.min <= svga.fifo_bytes;
    svga.fifo[SVGA_FIFO_MIN] = svga.min;
    svga.fifo[SVGA_FIFO_MAX] = svga.fifo_bytes;
    svga.fifo[SVGA_FIFO_NEXT_CMD] = svga.min;
    svga.fifo[SVGA_FIFO_STOP] = svga.min;
    if (regs > SVGA_FIFO_GUEST_3D_HWVERSION) {
        svga.fifo[SVGA_FIFO_GUEST_3D_HWVERSION] = SVGA3D_HWVERSION_WS8_B1;
        svga.fifo[SVGA_FIFO_RESERVED] = 0;
        svga.fifo[SVGA_FIFO_FENCE] = 0;
    }
    if (fifo_busy_present) svga.fifo[SVGA_FIFO_BUSY] = 0;
    svga_barrier();
    svga.ops.write(SVGA_REG_CONFIG_DONE, 1);
    if (svga.fifo[SVGA_FIFO_MIN] != svga.min || svga.fifo[SVGA_FIFO_MAX] != svga.fifo_bytes) {
        svga.fifo_busy_present = false;
        if (extended) probe_result("fifo-header", SVGA_EIO);
        return SVGA_EIO;
    }
    svga.fifo_busy_present = fifo_busy_present;
    svga.fifo_caps = regs > SVGA_FIFO_CAPABILITIES ? svga.fifo[SVGA_FIFO_CAPABILITIES] : 0;
    svga.fifo_ready = true;
    ++svga.generation;
    if (!svga.generation) ++svga.generation;
    svga.next_fence = 1;
    svga.issued_fence = 0;
    (void)svga_restore_traces_locked();
    if (extended) (void)svga_negotiate_locked();
    return 0;
}

int svga_negotiate_locked(void)
{
    svga.available = false;
    svga.host_version = 0;
    svga.probe.fifo_caps = svga.fifo_caps;
    svga.probe.host_legacy = svga.min > SVGA_FIFO_3D_HWVERSION * 4 ?
                            svga.fifo[SVGA_FIFO_3D_HWVERSION] : 0;
    svga.probe.host_revised = svga.min > SVGA_FIFO_3D_HWVERSION_REVISED * 4 ?
                             svga.fifo[SVGA_FIFO_3D_HWVERSION_REVISED] : 0;
    svga.probe.gb_objects = (svga.caps & SVGA_CAP_GBOBJECTS) != 0;
    svga.probe.devcap_3d = 0;
    if (svga.probe.gb_objects) {
        svga.ops.write(SVGA_REG_DEV_CAP, SVGA3D_DEVCAP_3D);
        svga.probe.devcap_3d = svga.ops.read(SVGA_REG_DEV_CAP);
    }
    for (uint32_t i = 0; i < SVGA3D_DEVCAP_MAX; ++i) {
        svga.cap_valid[i] = false; svga.cap_values[i] = 0;
    }
    if (!(svga.caps & SVGA_CAP_3D)) return probe_result("device-3d", SVGA_ENOTSUP);
    if (!(svga.caps & SVGA_CAP_EXTENDED_FIFO))
        return probe_result("extended-fifo", SVGA_ENOTSUP);
    if (!(svga.fifo_caps & SVGA_FIFO_CAP_FENCE)) return probe_result("fence", SVGA_ENOTSUP);
    if (!(svga.caps & (SVGA_CAP_GMR | SVGA_CAP_GMR2))) return probe_result("gmr", SVGA_ENOTSUP);
    if (svga.min <= SVGA_FIFO_GUEST_3D_HWVERSION * 4)
        return probe_result("guest-version-register", SVGA_ENOTSUP);
    if (svga.probe.gb_objects) {
        if (!svga.probe.devcap_3d) return probe_result("devcap-3d", SVGA_ENOTSUP);
        /* GB devices expose capabilities through indexed registers. Like
         * vmwgfx's legacy userspace interface, report WS8_B1 compatibility. */
        svga.host_version = SVGA3D_HWVERSION_WS8_B1;
        for (uint32_t i = 0; i < SVGA3D_DEVCAP_MAX; ++i) {
            svga.ops.write(SVGA_REG_DEV_CAP, i);
            svga.cap_values[i] = svga.ops.read(SVGA_REG_DEV_CAP);
            svga.cap_valid[i] = true;
        }
    } else {
        svga.host_version = svga.fifo[(svga.fifo_caps & SVGA_FIFO_CAP_3D_HWVERSION_REVISED) ?
                                     SVGA_FIFO_3D_HWVERSION_REVISED : SVGA_FIFO_3D_HWVERSION];
        if (svga.host_version < SVGA3D_HWVERSION_WS65_B1)
            return probe_result("host-version", SVGA_ENOTSUP);
        uint32_t pos = SVGA_FIFO_3D_CAPS;
        while (pos <= SVGA_FIFO_3D_CAPS_LAST) {
            uint32_t length = svga.fifo[pos];
            if (!length) break;
            if (length < 2 || length > SVGA_FIFO_3D_CAPS_LAST + 1 - pos)
                return probe_result("cap-record", SVGA_EIO);
            /* DEVCAPS records contain index/value pairs. */
            if (svga.fifo[pos + 1] == 0x100) {
                if (length & 1) return probe_result("cap-record", SVGA_EIO);
                for (uint32_t p = pos + 2; p < pos + length; p += 2) {
                    uint32_t index = svga.fifo[p];
                    if (index < SVGA3D_DEVCAP_MAX) {
                        svga.cap_values[index] = svga.fifo[p + 1];
                        svga.cap_valid[index] = true;
                    }
                }
            }
            pos += length;
        }
    }
    if (!svga.cap_valid[SVGA3D_DEVCAP_3D] || !svga.cap_values[SVGA3D_DEVCAP_3D])
        return probe_result("devcap-3d", SVGA_ENOTSUP);
    svga.context_limit = SVGA_CONTEXTS;
    svga.surface_limit = SVGA_SURFACES;
    if (svga.cap_valid[SVGA3D_DEVCAP_MAX_CONTEXT_IDS] &&
        svga.cap_values[SVGA3D_DEVCAP_MAX_CONTEXT_IDS] < svga.context_limit)
        svga.context_limit = svga.cap_values[SVGA3D_DEVCAP_MAX_CONTEXT_IDS];
    if (svga.cap_valid[SVGA3D_DEVCAP_MAX_SURFACE_IDS] &&
        svga.cap_values[SVGA3D_DEVCAP_MAX_SURFACE_IDS] < svga.surface_limit)
        svga.surface_limit = svga.cap_values[SVGA3D_DEVCAP_MAX_SURFACE_IDS];
    svga.gmr_limit = svga.ops.read(SVGA_REG_GMR_MAX_IDS);
    if (svga.gmr_limit > SVGA_GMRS) svga.gmr_limit = SVGA_GMRS;
    svga.page_limit = SVGA_MAX_GUEST_PAGES;
    svga.surface_limit_bytes = SVGA_MAX_SURFACE_BYTES;
    if (svga.caps & SVGA_CAP_GMR2) {
        uint32_t pages = svga.ops.read(SVGA_REG_GMRS_MAX_PAGES);
        if (pages < svga.page_limit) svga.page_limit = pages;
    } else if (svga.ops.read(SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH) < 2)
        return probe_result("gmr-descriptors", SVGA_ENOTSUP);
    if (svga.probe.gb_objects) {
        uint32_t pages = svga.ops.read(SVGA_REG_SUGGESTED_GBOBJECT_MEM_SIZE_KB) / 4;
        svga.mob_max_bytes = svga.ops.read(SVGA_REG_MOB_MAX_SIZE);
        if (!pages || svga.mob_max_bytes < SVGA_GB_CONTEXT_BYTES)
            return probe_result("mob-limits", SVGA_ENOTSUP);
        if (pages < svga.page_limit) svga.page_limit = pages;
        uint64_t bytes = (uint64_t)svga.page_limit * 4096u;
        if (bytes < svga.surface_limit_bytes) svga.surface_limit_bytes = bytes;
    } else if (svga.caps & SVGA_CAP_GMR2) {
        uint32_t memory = svga.ops.read(SVGA_REG_MEMORY_SIZE);
        uint32_t vram = svga.ops.read(SVGA_REG_VRAM_SIZE);
        if (memory < vram) return probe_result("surface-memory", SVGA_ENOTSUP);
        memory -= vram;
        if (memory < svga.surface_limit_bytes) svga.surface_limit_bytes = memory;
    }
    svga.compat_surface_limit_bytes = svga.surface_limit_bytes;
    if (svga.probe.gb_objects) {
        uint64_t non_gb_profile_bytes = (uint64_t)svga.page_limit * 4096u / 2u;
        if (non_gb_profile_bytes < svga.compat_surface_limit_bytes)
            svga.compat_surface_limit_bytes = non_gb_profile_bytes;
    }
    if (!svga.context_limit || !svga.surface_limit || !svga.gmr_limit ||
        !svga.page_limit || !svga.surface_limit_bytes ||
        !svga.compat_surface_limit_bytes)
        return probe_result("resource-limits", SVGA_ENOTSUP);
    if (svga.probe.gb_objects) {
        int ret = svga_gb_init_locked();
        if (ret) return probe_result("gb-tables", ret);
    }
    svga.available = true;
    return probe_result(svga.probe.gb_objects ? "ready-gb" : "ready", 0);
}

svga_handle svga_new_handle(uint32_t kind, uint32_t slot)
{
    if (svga.handle_serial == UINT32_MAX) return 0;
    ++svga.handle_serial;
    return ((uint64_t)svga.handle_serial << 32) | (kind << 24) | slot;
}
int svga_find_context(svga_handle h)
{
    uint32_t id = (uint32_t)h & 0xffffff;
    return h && id < SVGA_CONTEXTS && svga.contexts[id].handle == h ? (int)id : -1;
}
int svga_find_surface(svga_handle h)
{
    uint32_t id = (uint32_t)h & 0xffffff;
    return h && id < SVGA_SURFACES && svga.surfaces[id].handle == h ? (int)id : -1;
}
int svga_find_gmr(svga_handle h)
{
    uint32_t id = (uint32_t)h & 0xffffff;
    return h && id < SVGA_GMRS && svga.gmrs[id].handle == h ? (int)id : -1;
}

bool svga_context_usable(uint32_t id)
{
    return id < SVGA_CONTEXTS && svga.contexts[id].handle && !svga.contexts[id].gb.retiring;
}
bool svga_surface_usable(uint32_t id)
{
    return id < SVGA_SURFACES && svga.surfaces[id].handle &&
           !svga.surfaces[id].retiring && !svga.surfaces[id].gb.retiring;
}

/**
 * @brief Resolve a current native resource handle to its backend index.
 * @param h Generation-tagged context, surface or GMR handle.
 * @return Current resource index, or UINT32_MAX for invalid/stale handles.
 * Task context; acquires the SVGA lock internally. Returns only a snapshot index, not a retained resource reference.
 */
uint32_t svga_resource_id(svga_handle h)
{
    uint64_t flags = svga_lock();
    int id = svga_find_context(h);
    if (id < 0) id = svga_find_surface(h);
    if (id < 0) id = svga_find_gmr(h);
    svga_unlock(flags);
    return id < 0 ? UINT32_MAX : (uint32_t)id;
}

/**
 * @brief Copy current SVGA capabilities, generation and live resource counts.
 * @param out Writable kernel snapshot, nullable; NULL is ignored.
 * Task context for module callers; acquires the SVGA lock internally and copies core-owned state without transferring resource ownership.
 */
void svga_get_info(struct svga_info *out)
{
    if (!out) return;
    uint64_t flags = svga_lock();
    *out = (struct svga_info){ .device_caps = svga.caps, .fifo_caps = svga.fifo_caps,
        .host_version = svga.host_version, .guest_version = SVGA3D_HWVERSION_WS8_B1,
        .generation = svga.generation, .guest_pages = svga.pages,
        .surface_bytes = svga.surface_bytes,
        .surface_limit_bytes = svga.surface_limit_bytes,
        .compat_surface_limit_bytes = svga.compat_surface_limit_bytes,
        .fifo_ready = svga.fifo_ready,
        .available = svga.available, .probe = svga.probe };
    for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i) out->contexts += !!svga.contexts[i].handle;
    for (uint32_t i = 0; i < SVGA_SURFACES; ++i) out->surfaces += !!svga.surfaces[i].handle;
    for (uint32_t i = 0; i < SVGA_GMRS; ++i) out->gmrs += !!svga.gmrs[i].handle;
    svga_unlock(flags);
}

/**
 * @brief Copy one cached host capability after applying supported local bounds.
 * @param index SVGA3D capability selector.
 * @param out Required writable kernel value, unchanged for an unavailable capability.
 * @return Zero or SVGA_ENOTSUP for an unavailable selector/NULL output.
 * Task context for module callers; acquires the SVGA lock internally. No probing, allocation, backing reference or pointer escapes.
 */
int svga3d_capability(uint32_t index, uint32_t *out)
{
    uint64_t flags = svga_lock();
    int ret = SVGA_ENOTSUP;
    if (out && index < SVGA3D_DEVCAP_MAX && svga.cap_valid[index]) {
        uint32_t value = svga.cap_values[index];
        switch (index) {
        case SVGA3D_DEVCAP_MAX_CONTEXT_IDS: {
            uint32_t local = svga.context_limit ? svga.context_limit - 1u : 0u;
            if (value > local) value = local;
            break;
        }
        case SVGA3D_DEVCAP_MAX_SURFACE_IDS: {
            uint32_t local = svga.surface_limit ? svga.surface_limit - 1u : 0u;
            if (value > local) value = local;
            break;
        }
        case SVGA3D_DEVCAP_MAX_TEXTURE_WIDTH:
        case SVGA3D_DEVCAP_MAX_TEXTURE_HEIGHT:
        case SVGA3D_DEVCAP_MAX_VOLUME_EXTENT: {
            uint64_t max_dimension = svga.surface_limit_bytes / 2u;
            if (index == SVGA3D_DEVCAP_MAX_VOLUME_EXTENT && max_dimension > 256u)
                max_dimension = 256u;
            if (value > max_dimension) value = (uint32_t)max_dimension;
            break;
        }
        case SVGA3D_DEVCAP_MAX_TEXTURES:
        case SVGA3D_DEVCAP_MAX_SHADER_TEXTURES:
        case SVGA3D_DEVCAP_MAX_VERTEX_SHADER_TEXTURES:
            if (value > 32u) value = 32u;
            break;
        case SVGA3D_DEVCAP_MAX_RENDER_TARGETS:
        case SVGA3D_DEVCAP_MAX_SIMULTANEOUS_RENDER_TARGETS:
            if (value > SVGA3D_MAX_SIMULTANEOUS_RENDER_TARGETS)
                value = SVGA3D_MAX_SIMULTANEOUS_RENDER_TARGETS;
            break;
        default:
            break;
        }
        *out = value;
        ret = 0;
    }
    svga_unlock(flags);
    return ret;
}

int svga3d_init(void)
{
    uint64_t flags = svga_lock();
    struct svga_cache_report cache_report = {0};
    bool cache_report_ready = false;
    svga.probe = (struct svga_probe_info){.stage = "not-bound", .status = SVGA_ENODEV};
    if (!svga.bound) {
        svga_unlock(flags);
        return SVGA_ENODEV;
    }
    int ret = svga_shutdown_locked();
    if (ret) probe_result("drain", ret);
#ifndef SVGA_HOST_TEST
    if (!ret) {
        ret = svga_quiesce_fifo_for_bar_probe_locked();
        if (ret) probe_result("fifo-drain", ret);
    }
    if (!ret) {
        int cache_status = svga_prepare_fifo_cache_policy(framebuffer_get(),
                                                          &cache_report);
        cache_report_ready = true;
        if (cache_status) {
            probe_result("mmio-cache", cache_status);
            cache_report.status = cache_status;
            int restore_result = svga_restore_legacy_after_cache_error_locked(cache_status);
            cache_report.legacy_restored = restore_result == cache_status;
            ret = restore_result;
        }
    }
#endif
    if (!ret) {
        svga.memory_ready = true;
        ret = svga_configure_locked(true);
        if (ret || !svga.available) {
            int status = ret ? ret : svga.probe.status;
            ret = svga_gb_shutdown_locked();
            if (!ret) {
                svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
                svga.fifo_ready = false;
                ret = svga_configure_locked(false) == 0 ? status : SVGA_EIO;
            }
        }
    }
    svga_unlock(flags);
    if (cache_report_ready) svga_report_cache_result_unlocked(&cache_report);
#ifndef SVGA_HOST_TEST
    console_log(ret < 0 ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
                "[svga3d] init=%d caps=0x%x fifo=0x%x host=0x%x guest=0x%x\n",
                   ret, svga.caps, svga.fifo_caps, svga.host_version, SVGA3D_HWVERSION_WS8_B1);
#endif
    return ret;
}

int svga_shutdown_locked(void)
{
    /* Shutdown runs under svga_lock and cannot dispatch borrowed BO ownership
     * callbacks. Refuse before changing any host state; the caller must retire
     * these attachments through the task-context detach path first. */
    for (uint32_t i = 0; i < SVGA_GMRS; ++i)
        if (svga.gmrs[i].handle && svga.gmrs[i].borrowed) return SVGA_EBUSY;
    if (!svga.fifo_ready) return svga.gb_active ? SVGA_EBUSY : 0;
    int ret = svga_sync_locked();
    if (ret) return ret;
    for (uint32_t i = 0; i < SVGA_CONTEXTS; ++i)
        if (svga.contexts[i].handle && (ret = svga_context_destroy_locked(i))) return ret;
    for (uint32_t i = 0; i < SVGA_SURFACES; ++i)
        if (svga.surfaces[i].handle && (ret = svga_surface_destroy_locked(i))) return ret;
    for (uint32_t i = 0; i < SVGA_GMRS; ++i)
        if (svga.gmrs[i].handle && (ret = svga_gmr_destroy_locked(i))) return ret;
    if ((ret = svga_gb_shutdown_locked())) return ret;
    svga_gpu_reset_locked();
    svga.available = false;
    return 0;
}
int svga3d_shutdown(void)
{
    uint64_t flags = svga_lock();
    int ret = svga_shutdown_locked();
    if (!ret && svga.bound) {
        svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
        svga.traces_enabled = false;
        svga.fifo_ready = false;
        if (svga_configure_locked(false) != 0) ret = SVGA_EIO;
    }
    svga_unlock(flags);
    return ret;
}
int svga_update_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t data[] = {x, y, width, height};
    struct svga_span span = {data, sizeof(data)};
    /* UPDATE is the ordinary scanout path. Packet publication already issues
     * the SVGA SYNC doorbell; waiting for BUSY here would hold the device lock
     * and serialize every framebuffer present behind host progress. Explicit
     * fences, mode transitions, and shutdown retain their synchronous paths. */
    return svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1);
}

/** @brief Publish a scanout UPDATE without waiting when the FIFO is full. */
int svga_update_try_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t data[] = {x, y, width, height};
    struct svga_span span = {data, sizeof(data)};
    return svga_fifo_packet_try_locked(SVGA_CMD_UPDATE, false, &span, 1);
}

int svga_update(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint64_t flags = svga_lock();
    int ret = svga_update_locked(x, y, width, height);
    svga_unlock(flags);
    return ret;
}
int svga_mode_begin(uint64_t *flags)
{
    if (!flags) return SVGA_EINVAL;
    *flags = svga_lock();
    if (svga.mode_leases) {
        svga_unlock(*flags);
        return SVGA_EBUSY;
    }
    int ret = svga_shutdown_locked();
    if (!ret && svga.bound) {
        svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
        svga.traces_enabled = false;
        svga.fifo_ready = false;
        svga.available = false;
    }
    if (ret) svga_unlock(*flags);
    return ret;
}

int svga_mode_lease_acquire(void)
{
    uint64_t flags = svga_lock();
    int ret = svga_mode_lease_acquire_locked();
    svga_unlock(flags);
    return ret;
}

int svga_mode_lease_acquire_locked(void)
{
    if (svga.mode_leases == UINT32_MAX) return SVGA_ENOMEM;
    ++svga.mode_leases;
    return 0;
}

int svga_mode_lease_release_locked(void)
{
    if (!svga.mode_leases) return SVGA_EINVAL;
    --svga.mode_leases;
    return 0;
}

int svga_mode_lease_release(void)
{
    uint64_t flags = svga_lock();
    int ret = svga_mode_lease_release_locked();
    svga_unlock(flags);
    return ret;
}
void svga_mode_end(uint64_t flags)
{
    int ret = svga_configure_locked(svga.memory_ready);
    if (ret || (svga.memory_ready && !svga.available)) {
        if (!svga_gb_shutdown_locked()) {
            svga.ops.write(SVGA_REG_CONFIG_DONE, 0);
            svga.fifo_ready = false;
            (void)svga_configure_locked(false);
        }
    }
    svga_unlock(flags);
}

EXPORT_SYMBOL(svga3d_capability);
EXPORT_SYMBOL(svga_get_info);
EXPORT_SYMBOL(svga_resource_id);
