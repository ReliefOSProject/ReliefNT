/*
 * ReliefOS x86_64 APIC support.
 *
 * ACPI MADT discovery is deliberately independent from interrupt policy. The
 * legacy PIC remains usable as a fallback, while the local APIC and IOAPIC are
 * available to SMP startup and future device routing.
 */
#include <reliefnt/apic.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/power.h>
#include <reliefnt/smp.h>
#include <reliefnt/time.h>

#define CPUID_FEATURE_APIC (1u << 9)
#define IA32_APIC_BASE 0x1bu
#define IA32_APIC_BASE_X2APIC (1ULL << 10)
#define IA32_APIC_BASE_ENABLE (1ULL << 11)
#define APIC_REG_ID 0x20u
#define APIC_REG_EOI 0xb0u
#define APIC_REG_SVR 0xf0u
#define APIC_REG_ICR_LOW 0x300u
#define APIC_REG_ICR_HIGH 0x310u
#define APIC_REG_LVT_LINT0 0x350u
#define APIC_REG_LVT_LINT1 0x360u
#define APIC_REG_LVT_TIMER 0x320u
#define APIC_REG_TIMER_INITIAL 0x380u
#define APIC_REG_TIMER_CURRENT 0x390u
#define APIC_REG_TIMER_DIVIDE 0x3e0u
#define APIC_TIMER_PERIODIC (1u << 17)
#define APIC_TIMER_DIVIDE_16 0x3u
#define APIC_TIMER_REFERENCE_COUNTS (ACPI_PM_TIMER_HZ / 20u)
#define APIC_TIMER_REFERENCE_MAX_COUNTS (APIC_TIMER_REFERENCE_COUNTS + APIC_TIMER_REFERENCE_COUNTS / 5u)
#define APIC_TIMER_SNAPSHOT_MAX_COUNTS (ACPI_PM_TIMER_HZ / 10000u)
#define APIC_TIMER_MIN_COUNT 16u
#ifndef APIC_TIMER_CALIBRATION_POLL_LIMIT
#define APIC_TIMER_CALIBRATION_POLL_LIMIT 1000000u
#endif
#define APIC_REG_ICR_DELIVERY_STATUS (1u << 12)
#define APIC_DM_INIT (5u << 8)
#define APIC_DM_STARTUP (6u << 8)
#define APIC_TRIGGER_LEVEL (1u << 15)
#define APIC_LEVEL_ASSERT (1u << 14)
#define APIC_DEST_PHYSICAL (0u << 11)
#define APIC_LVT_MASKED (1u << 16)
#define APIC_LVT_DELIVERY_NMI (4u << 8)
#define APIC_LVT_DELIVERY_EXTINT (7u << 8)
#define IOAPIC_REDIR_ACTIVE_LOW (1u << 13)
#define IOAPIC_REDIR_LEVEL_TRIGGERED (1u << 15)
#define IOAPIC_REDIR_MASKED (1u << 16)
#define IOAPIC_REGSEL 0x00u
#define IOAPIC_WINDOW 0x10u
#define IOAPIC_REDTBL 0x10u
#define IOAPIC_MAX 8u
#define SMP_DISCOVERED_MAX 64u

struct __attribute__((packed)) madt_header { uint8_t type; uint8_t length; };
struct __attribute__((packed)) ioapic_desc {
    uint8_t id; uint8_t reserved; uint32_t address; uint32_t gsi_base;
};
struct __attribute__((packed)) madt_interrupt_override {
    uint8_t bus;
    uint8_t source_irq;
    uint32_t gsi;
    uint16_t flags;
};
struct ioapic_info {
    volatile uint32_t *base; uint8_t id; uint32_t gsi_base; uint32_t gsi_count;
};
struct isa_irq_override {
    uint32_t gsi;
    uint16_t flags;
    bool present;
};

static bool local_apic_present;
static bool local_apic_enabled;
static bool ioapic_present;
static uintptr_t local_apic_base;
static uint32_t bsp_apic_id;
static uint32_t cpu_apic_ids[SMP_DISCOVERED_MAX];
static uint32_t cpu_count;
static struct ioapic_info ioapics[IOAPIC_MAX];
static uint32_t ioapic_count;
static struct isa_irq_override isa_irq_overrides[16];

static void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                  uint32_t *ecx, uint32_t *edx)
{
    __asm__ volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                     : "a"(leaf), "c"(0));
}

static uint64_t read_msr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static void write_msr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}

static uint32_t lapic_read(uint32_t reg)
{
#ifdef RELIEFNT_APIC_TEST
    extern uint32_t apic_test_read(uint32_t reg);
    return apic_test_read(reg);
#else
    return local_apic_base ? *(volatile uint32_t *)(local_apic_base + reg) : 0;
#endif
}

static void lapic_write(uint32_t reg, uint32_t value)
{
#ifdef RELIEFNT_APIC_TEST
    extern void apic_test_write(uint32_t reg, uint32_t value);
    apic_test_write(reg, value);
#else
    if (local_apic_base) {
        *(volatile uint32_t *)(local_apic_base + reg) = value;
        (void)lapic_read(APIC_REG_ID);
    }
#endif
}

static bool cpu_id_seen(uint32_t id)
{
    for (uint32_t i = 0; i < cpu_count; ++i) {
        if (cpu_apic_ids[i] == id) return true;
    }
    return false;
}

static void discover_madt(void)
{
    const uint8_t *table = (const uint8_t *)power_acpi_find_table("APIC");
    uint32_t length, offset;
    if (!table) return;
    length = *(const uint32_t *)(table + 4);
    if (length < 44u) return;
    offset = 44u;
    while (offset + sizeof(struct madt_header) <= length) {
        const struct madt_header *entry = (const struct madt_header *)(table + offset);
        if (!entry->length || offset + entry->length > length) break;
        if (entry->type == 0 && entry->length >= 8u) {
            const uint8_t *p = table + offset;
            uint32_t id = p[3], flags = *(const uint32_t *)(p + 4);
            if ((flags & 1u) && cpu_count < SMP_DISCOVERED_MAX && !cpu_id_seen(id))
                cpu_apic_ids[cpu_count++] = id;
        } else if (entry->type == 9 && entry->length >= 16u) {
            const uint8_t *p = table + offset;
            uint32_t id = *(const uint32_t *)(p + 4), flags = *(const uint32_t *)(p + 8);
            if ((flags & 1u) && cpu_count < SMP_DISCOVERED_MAX && !cpu_id_seen(id))
                cpu_apic_ids[cpu_count++] = id;
        } else if (entry->type == 1 && entry->length >= 12u && ioapic_count < IOAPIC_MAX) {
            const struct ioapic_desc *desc = (const struct ioapic_desc *)(table + offset + 2u);
            ioapics[ioapic_count] = (struct ioapic_info){
                (volatile uint32_t *)(uintptr_t)desc->address, desc->id, desc->gsi_base, 24u};
            ++ioapic_count;
        } else if (entry->type == 2 && entry->length >= 10u) {
            const struct madt_interrupt_override *override =
                (const struct madt_interrupt_override *)(table + offset + 2u);
            if (override->bus == 0u && override->source_irq < 16u) {
                isa_irq_overrides[override->source_irq] = (struct isa_irq_override){
                    .gsi = override->gsi,
                    .flags = override->flags,
                    .present = true,
                };
            }
        }
        offset += entry->length;
    }
}

void apic_init(void)
{
    uint32_t eax, ebx, ecx, edx;
    uint64_t base;
    cpu_count = 0;
    ioapic_count = 0;
    local_apic_enabled = false;
    for (uint32_t i = 0; i < 16u; ++i) {
        isa_irq_overrides[i] = (struct isa_irq_override){0};
    }
    cpuid(1, &eax, &ebx, &ecx, &edx);
    local_apic_present = (edx & CPUID_FEATURE_APIC) != 0;
    base = local_apic_present ? read_msr(IA32_APIC_BASE) : 0;
    if (base & IA32_APIC_BASE_X2APIC) {
        /* x2APIC uses MSR registers rather than the MMIO window implemented
         * here. Keep the legacy PIC fallback instead of touching an invalid
         * MMIO address. */
        local_apic_present = false;
        local_apic_base = 0;
    }
    if (local_apic_present) {
        local_apic_base = (uintptr_t)(base & 0xfffff000ULL);
        /* IA32_APIC_BASE contains an address in bits 12..35, not an APIC
         * identifier.  Treating its high address byte (normally 0xfe for
         * 0xfee00000) as the BSP ID makes SMP startup send INIT/SIPI to the
         * real BSP instead of skipping it, which resets the whole machine. */
        bsp_apic_id = lapic_read(APIC_REG_ID) >> 24;
    } else {
        /* CPUID.1:EBX[31:24] is the architectural fallback when the local
         * APIC MMIO interface cannot be used. */
        bsp_apic_id = ebx >> 24;
    }
    discover_madt();
    if (!cpu_count && local_apic_present) cpu_apic_ids[cpu_count++] = bsp_apic_id;
    console_info("[reliefnt] local APIC %s base=0x%llx MADT CPUs=%u BSP=%u\n",
                   local_apic_present ? "detected" : "unavailable",
                   (unsigned long long)local_apic_base, (unsigned)cpu_count,
                   (unsigned)bsp_apic_id);
}

void ioapic_init(void)
{
    ioapic_present = ioapic_count != 0;
    console_info("[reliefnt] IOAPIC %s count=%u\n",
                   ioapic_present ? "detected" : "unavailable", (unsigned)ioapic_count);
}

bool apic_available(void) { return local_apic_present; }
bool apic_enabled(void) { return local_apic_enabled; }
bool ioapic_available(void) { return ioapic_present; }
uint32_t apic_bsp_id(void) { return bsp_apic_id; }
uint32_t apic_id(void) { return local_apic_base ? (lapic_read(APIC_REG_ID) >> 24) : bsp_apic_id; }
uint32_t apic_cpu_count(void) { return cpu_count; }
uint32_t apic_cpu_id_at(uint32_t index)
{
    return index < cpu_count ? cpu_apic_ids[index] : 0xffffffffu;
}

void apic_enable(void)
{
    if (!local_apic_present || !local_apic_base) return;
    write_msr(IA32_APIC_BASE, read_msr(IA32_APIC_BASE) | IA32_APIC_BASE_ENABLE);
    lapic_write(APIC_REG_SVR, 0x100u | 0xffu);
    lapic_write(0x320u, APIC_LVT_MASKED);
    lapic_write(0x330u, APIC_LVT_MASKED);
    lapic_write(0x350u, APIC_LVT_MASKED);
    lapic_write(0x360u, APIC_LVT_MASKED);
    local_apic_enabled = true;
}

void apic_enable_legacy_pic(void)
{
    if (!local_apic_present || !local_apic_base) {
        return;
    }
    apic_enable();
    /* In virtual-wire mode the 8259 PIC reaches the BSP through LINT0.
     * LINT1 retains NMI delivery.  Without this, enabling the LAPIC for
     * SMP silently disconnects PIT, keyboard, and PS/2 mouse interrupts. */
    lapic_write(APIC_REG_LVT_LINT0, APIC_LVT_DELIVERY_EXTINT);
    lapic_write(APIC_REG_LVT_LINT1, APIC_LVT_DELIVERY_NMI);
}

struct apic_timer_sample {
    uint32_t reference;
    uint32_t count;
};

static bool apic_timer_calibration_poll(uint32_t *budget)
{
    if (!*budget) return false;
    --*budget;
    /* APs are online with IF=0. Respond without depending on IPI delivery or
     * holding a lock needed by the BSP or another calibrating AP. */
    smp_membarrier_poll();
    __asm__ volatile("pause" : : : "memory");
    return true;
}

static bool apic_timer_sample(struct apic_timer_sample *sample, uint32_t mask,
                              uint32_t *budget)
{
    while (apic_timer_calibration_poll(budget)) {
        uint32_t before, after, first_mask, last_mask;
        if (!power_acpi_pm_timer_read(&before, &first_mask)) return false;
        uint32_t count = lapic_read(APIC_REG_TIMER_CURRENT);
        if (!power_acpi_pm_timer_read(&after, &last_mask) ||
            first_mask != mask || last_mask != mask) return false;
        uint32_t span = (after - before) & mask;
        if (span <= APIC_TIMER_SNAPSHOT_MAX_COUNTS) {
            *sample = (struct apic_timer_sample){
                .reference = (before + span / 2u) & mask, .count = count};
            return true;
        }
        /* A slow/glitching snapshot is not a trustworthy timestamp for the
         * LAPIC read in its middle. Retry within the same bounded budget. */
    }
    return false;
}

void apic_timer_init(uint8_t vector, uint32_t fallback_count)
{
    if (!local_apic_enabled) return;
    struct apic_timer_sample samples[3] = {{0}};
    uint32_t budget = APIC_TIMER_CALIBRATION_POLL_LIMIT;
    uint32_t initial_count = fallback_count ? fallback_count : 1000000u;
    uint32_t reference = 0, mask = 0, elapsed = 0;
    const char *failure = "reference-unavailable";
    lapic_write(APIC_REG_LVT_TIMER, vector | APIC_LVT_MASKED);
    lapic_write(APIC_REG_TIMER_INITIAL, 0);
    if (!power_acpi_pm_timer_read(&reference, &mask) ||
        (mask != 0x00ffffffu && mask != UINT32_MAX)) goto program_periodic;
    /* Initial PIT IRQs can be delivered in a catch-up burst after boot ran
     * with IF=0. ACPI's 3.579545 MHz free-running counter measures the physical
     * interval independently of IRQ delivery and CPU/TSC frequency/offset. */
    lapic_write(APIC_REG_TIMER_DIVIDE, APIC_TIMER_DIVIDE_16);
    lapic_write(APIC_REG_TIMER_INITIAL, UINT32_MAX);
    failure = "sample-timeout";
    if (!apic_timer_sample(&samples[0], mask, &budget)) goto program_periodic;
    if (!samples[0].count) { failure = "counter-expired"; goto program_periodic; }
    uint64_t periods[2];
    for (unsigned window = 0; window < 2u; ++window) {
        const struct apic_timer_sample *start = &samples[window];
        struct apic_timer_sample *end = &samples[window + 1u];
        bool complete = false;
        while (apic_timer_calibration_poll(&budget)) {
            uint32_t current_mask;
            if (!power_acpi_pm_timer_read(&reference, &current_mask) || current_mask != mask) {
                failure = "reference-unavailable"; break;
            }
            elapsed = (reference - start->reference) & mask;
            if (elapsed < APIC_TIMER_REFERENCE_COUNTS) continue;
            if (!apic_timer_sample(end, mask, &budget)) break;
            elapsed = (end->reference - start->reference) & mask;
            if (elapsed < APIC_TIMER_REFERENCE_COUNTS || elapsed > APIC_TIMER_REFERENCE_MAX_COUNTS) {
                failure = "sample-overshoot"; break;
            }
            if (!end->count || end->count >= start->count) {
                failure = "counter-invalid"; break;
            }
            uint64_t delta = (uint64_t)start->count - end->count;
            uint64_t denominator = (uint64_t)elapsed * RELIEFNT_TICK_HZ;
            uint64_t measured = (delta * ACPI_PM_TIMER_HZ + denominator / 2u) / denominator;
            if (measured < APIC_TIMER_MIN_COUNT || measured > UINT32_MAX) {
                failure = "count-out-of-range"; break;
            }
            periods[window] = measured;
            complete = true;
            break;
        }
        if (!complete) goto program_periodic;
    }
    /* A single long vCPU pause may alias a full 24-bit PM counter turn into
     * a short interval. Two independent windows must agree within 5% plus
     * rounding, so such a pause cannot silently establish a wrong rate. */
    uint64_t higher = periods[0] > periods[1] ? periods[0] : periods[1];
    uint64_t lower = periods[0] < periods[1] ? periods[0] : periods[1];
    if (higher - lower > higher / 20u + 1u) {
        failure = "inconsistent-windows"; goto program_periodic;
    }
    elapsed = (samples[2].reference - samples[0].reference) & mask;
    uint64_t delta = (uint64_t)samples[0].count - samples[2].count;
    uint64_t denominator = (uint64_t)elapsed * RELIEFNT_TICK_HZ;
    initial_count = (uint32_t)((delta * ACPI_PM_TIMER_HZ + denominator / 2u) / denominator);
    failure = NULL;
program_periodic:
    /* The independent poll cap also covers a stopped reference. It bounds
     * iterations, not host wall time if a virtual CPU is descheduled. */
    lapic_write(APIC_REG_LVT_TIMER, vector | APIC_LVT_MASKED);
    lapic_write(APIC_REG_TIMER_INITIAL, 0);
    lapic_write(APIC_REG_TIMER_DIVIDE, APIC_TIMER_DIVIDE_16);
    lapic_write(APIC_REG_LVT_TIMER, vector | APIC_TIMER_PERIODIC);
    lapic_write(APIC_REG_TIMER_INITIAL, initial_count);
    /* Serialize only the completed result with user-console transactions.
     * Calibration itself holds no execution/scheduler lock. Lock admission
     * also polls membarriers while this AP still has interrupts masked. */
    uint64_t log_flags;
    kernel_execution_lock_irqsave(&log_flags);
    console_log(failure ? CONSOLE_LOG_WARN : CONSOLE_LOG_INFO,
        "[reliefnt] LAPIC timer APIC=%u target_hz=%u count=%u %s clock=acpi-pm window_us=%llu start=%u end=%u polls=%u\n",
        (unsigned)apic_id(), (unsigned)RELIEFNT_TICK_HZ, (unsigned)initial_count,
        failure ? failure : "calibrated",
        (unsigned long long)((uint64_t)elapsed * 1000000u / ACPI_PM_TIMER_HZ),
        (unsigned)samples[0].count, (unsigned)samples[2].count,
        (unsigned)(APIC_TIMER_CALIBRATION_POLL_LIMIT - budget));
    kernel_execution_unlock_irqrestore(log_flags);
}

void apic_eoi(void)
{
    /* SDM Vol. 3 10.8.1: every interrupt the local APIC accepted needs an
     * EOI, and an EOI written with no interrupt in service is ignored.  Gate
     * on presence, not on apic_enable(): firmware leaves the APIC enabled     * across the boot handoff, so a handler can owe an EOI before irq_init()
     * ever calls apic_enable(). */
    if (local_apic_present) {
        lapic_write(APIC_REG_EOI, 0);
    }
}

static void apic_wait_delivery(void)
{
    for (uint32_t i = 0; i < 1000000u; ++i) {
        if (!(lapic_read(APIC_REG_ICR_LOW) & APIC_REG_ICR_DELIVERY_STATUS)) return;
        __asm__ volatile("pause");
    }
}

void apic_send_init(uint32_t destination)
{
    if (!local_apic_enabled || destination > 0xffu) return;
    apic_wait_delivery();
    lapic_write(APIC_REG_ICR_HIGH, destination << 24);
    lapic_write(APIC_REG_ICR_LOW, APIC_DEST_PHYSICAL | APIC_DM_INIT |
                                  APIC_TRIGGER_LEVEL | APIC_LEVEL_ASSERT);
    apic_wait_delivery();
    for (volatile uint32_t delay = 0; delay < 100000u; ++delay) __asm__ volatile("pause");
    lapic_write(APIC_REG_ICR_LOW, APIC_DEST_PHYSICAL | APIC_DM_INIT | APIC_TRIGGER_LEVEL);
    apic_wait_delivery();
}

void apic_send_startup(uint32_t destination, uint8_t vector)
{
    if (!local_apic_enabled || destination > 0xffu) return;
    apic_wait_delivery();
    lapic_write(APIC_REG_ICR_HIGH, destination << 24);
    lapic_write(APIC_REG_ICR_LOW, APIC_DEST_PHYSICAL | APIC_DM_STARTUP | vector);
    apic_wait_delivery();
}

static uint32_t ioapic_read(const struct ioapic_info *io, uint8_t reg)
{
    if (!io || !io->base) return 0;
    io->base[IOAPIC_REGSEL / 4u] = reg;
    return io->base[IOAPIC_WINDOW / 4u];
}

void apic_send_ipi(uint32_t destination, uint8_t vector)
{
    if (!local_apic_enabled || destination > 0xffu) return;
    apic_wait_delivery();
    lapic_write(APIC_REG_ICR_HIGH, destination << 24);
    lapic_write(APIC_REG_ICR_LOW, APIC_DEST_PHYSICAL | vector);
    apic_wait_delivery();
}

/**
 * @brief Submit a physical NMI with a bounded wait for the local ICR.
 * @param destination Online physical xAPIC ID.
 * @return True on submission; false if unavailable or the ICR stays busy.
 */
bool apic_send_nmi(uint32_t destination)
{
    if (!local_apic_enabled || destination > 0xffu) return false;
    for (uint32_t attempt = 0; attempt < 1000000u; ++attempt) {
        if (!(lapic_read(APIC_REG_ICR_LOW) & APIC_REG_ICR_DELIVERY_STATUS)) {
            lapic_write(APIC_REG_ICR_HIGH, destination << 24);
            lapic_write(APIC_REG_ICR_LOW, APIC_DEST_PHYSICAL | APIC_LVT_DELIVERY_NMI);
            return true;
        }
        __asm__ volatile("pause");
    }
    return false;
}

static void ioapic_write(const struct ioapic_info *io, uint8_t reg, uint32_t value)
{
    if (!io || !io->base) return;
    io->base[IOAPIC_REGSEL / 4u] = reg;
    io->base[IOAPIC_WINDOW / 4u] = value;
}

bool ioapic_route_irq(uint32_t irq, uint8_t vector, uint32_t destination)
{
    uint32_t gsi = irq;
    uint32_t redirection_flags = 0;

    if (irq < 16u && isa_irq_overrides[irq].present) {
        uint16_t flags = isa_irq_overrides[irq].flags;
        uint16_t polarity = flags & 3u;
        uint16_t trigger = (flags >> 2) & 3u;

        /* ISA defaults are active-high, edge-triggered.  ACPI uses zero for
         * "conforms to bus"; reserved encodings are rejected so the caller
         * can retain the working virtual-wire PIC fallback instead. */
        if (polarity == 2u || trigger == 2u) {
            return false;
        }
        gsi = isa_irq_overrides[irq].gsi;
        if (polarity == 3u) {
            redirection_flags |= IOAPIC_REDIR_ACTIVE_LOW;
        }
        if (trigger == 3u) {
            redirection_flags |= IOAPIC_REDIR_LEVEL_TRIGGERED;
        }
    }
    for (uint32_t i = 0; i < ioapic_count; ++i) {
        struct ioapic_info *io = &ioapics[i];
        uint32_t count = ((ioapic_read(io, 1u) >> 16) & 0xffu) + 1u;
        io->gsi_count = count;
        if (gsi < io->gsi_base || gsi >= io->gsi_base + count) continue;
        uint8_t redir = (uint8_t)(IOAPIC_REDTBL + (gsi - io->gsi_base) * 2u);
        /* Mask first, then set the high dword, then publish the completed
         * entry.  This prevents an in-flight edge from reaching the wrong
         * CPU while an IOAPIC redirection entry is being rewritten. */
        ioapic_write(io, redir, IOAPIC_REDIR_MASKED);
        ioapic_write(io, (uint8_t)(redir + 1u), (destination & 0xffu) << 24);
        ioapic_write(io, redir, (uint32_t)vector | redirection_flags);
        console_info("[reliefnt] IOAPIC route ISA IRQ%u GSI%u vector=0x%x BSP=%u\n",
                       (unsigned)irq, (unsigned)gsi, (unsigned)vector,
                       (unsigned)destination);
        return true;
    }
    return false;
}
