/*
 * ReliefOS x86_64 power control: implements firmware and virtual-machine power.
 * Provides shutdown, reboot, and platform-specific fallback mechanisms.
 */
#include <reliefnt/console.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/power.h>
#include <reliefnt/paging.h>
#include <reliefnt/memory_topology.h>
#include <linux/errno.h>

#include "../port.h"
#include "firmware_internal.h"

/**
 * Io delay.
 */
static void io_delay(void)
{
    for (volatile uint32_t i = 0; i < 100000; ++i) {
        __asm__ volatile("pause");
    }
}

struct __attribute__((packed)) acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
};

struct __attribute__((packed)) acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};

struct acpi_power_state {
    uint16_t pm1a_control;
    uint16_t pm1b_control;
    uint16_t sleep_type_a;
    uint16_t sleep_type_b;
    uint32_t smi_command;
    uint8_t acpi_enable;
    uint8_t pm1_control_length;
    uint8_t available;
};

static struct acpi_power_state acpi_power;
static const struct acpi_rsdp *acpi_root_rsdp;
/** @brief Borrowed boot map while the loader's physical identity map is active. */
static const struct boot_info *acpi_early_boot;
#define ACPI_RESOURCE_MAX 4096u
struct acpi_boot_resource { uint64_t start, bytes; };
static struct acpi_boot_resource acpi_resources[ACPI_RESOURCE_MAX];
static uint32_t acpi_resource_count;
static uint16_t acpi_pm_timer_port;
static uint32_t acpi_pm_timer_mask;

/**
 * Acpi read32.
 * @param base Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t acpi_read32(const uint8_t *base, uint32_t offset)
{
    uint32_t value = 0;
    if (!base) {
        return 0;
    }
    for (uint32_t i = 0; i < 4; ++i) {
        value |= (uint32_t)base[offset + i] << (i * 8U);
    }
    return value;
}

/**
 * Acpi read64.
 * @param base Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint64_t acpi_read64(const uint8_t *base, uint32_t offset)
{
    uint64_t value = 0;
    if (!base) {
        return 0;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        value |= (uint64_t)base[offset + i] << (i * 8U);
    }
    return value;
}

static void acpi_pm_timer_init(const uint8_t *fadt, uint32_t length)
{
    acpi_pm_timer_port = 0;
    acpi_pm_timer_mask = 0;
    if (!fadt || length < 116u || fadt[91] != 4u) return;
    uint32_t flags = acpi_read32(fadt, 112);
    if (flags & (1u << 20)) return; /* HW_REDUCED_ACPI has no fixed registers. */
    uint64_t address = acpi_read32(fadt, 76);
    uint32_t mask = flags & (1u << 8) ? UINT32_MAX : 0x00ffffffu;
    if (length >= 220u && acpi_read64(fadt, 212)) {
        /* A nonzero X_PM_TMR_BLK supersedes PM_TMR_BLK. Do not silently use
         * the legacy address when the preferred GAS is unsupported/invalid. */
        /* PM_TMR is read as one 32-bit register even on 24-bit counters. */
        if (fadt[208] != 1u || fadt[209] != 32u ||
            fadt[210] || (fadt[211] && fadt[211] != 3u)) return;
        address = acpi_read64(fadt, 212);
    }
    if (!address || address > 0xfffcu) return;
    acpi_pm_timer_port = (uint16_t)address;
    acpi_pm_timer_mask = mask;
}

bool power_acpi_pm_timer_read(uint32_t *count, uint32_t *mask)
{
    if (!count || !mask || !acpi_pm_timer_port) return false;
    *mask = acpi_pm_timer_mask;
    *count = x86_64_inl(acpi_pm_timer_port) & acpi_pm_timer_mask;
    return true;
}
#include <reliefnt/export.h>
EXPORT_SYMBOL(power_acpi_pm_timer_read);

/**
 * Acpi guid equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int acpi_guid_equal(const uint8_t *a, const uint8_t *b)
{
    if (!a || !b) {
        return 0;
    }
    for (uint32_t i = 0; i < 16U; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Acpi bytes equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @param length Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static int acpi_bytes_equal(const char *a, const char *b, uint32_t length)
{
    if (!a || !b) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Acpi checksum valid.
 * @param table Value supplied by the caller.
 * @param length Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
static int acpi_checksum_valid(const uint8_t *table, uint32_t length)
{
    uint8_t checksum = 0;
    if (!table || !length || length > 1024U * 1024U) {
        return 0;
    }
    for (uint32_t i = 0; i < length; ++i) {
        checksum = (uint8_t)(checksum + table[i]);
    }
    return checksum == 0;
}

/**
 * Acpi rsdp valid.
 * @param rsdp Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int acpi_rsdp_valid(const struct acpi_rsdp *rsdp)
{
    uint8_t checksum = 0;
    if (!rsdp || !acpi_bytes_equal(rsdp->signature, "RSD PTR ", 8)) {
        return 0;
    }
    for (uint32_t i = 0; i < 20U; ++i) {
        checksum = (uint8_t)(checksum + ((const uint8_t *)rsdp)[i]);
    }
    if (checksum != 0) {
        return 0;
    }
    if (rsdp->revision >= 2 &&
        (rsdp->length < sizeof(*rsdp) || rsdp->length > 4096U ||
         !acpi_checksum_valid((const uint8_t *)rsdp, rsdp->length))) {
        return 0;
    }
    return 1;
}

/**
 * Acpi scan rsdp range.
 * @param start Value supplied by the caller.
 * @param end Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static const struct acpi_rsdp *acpi_scan_rsdp_range(uint32_t start,
                                                     uint32_t end)
{
    for (uint32_t address = (start + 15U) & ~15U;
         address + sizeof(struct acpi_rsdp) <= end;
         address += 16U) {
        const struct acpi_rsdp *rsdp =
            (const struct acpi_rsdp *)(uintptr_t)address;
        if (acpi_rsdp_valid(rsdp)) {
            return rsdp;
        }
    }
    return 0;
}

/**
 * Acpi scan rsdp.
 * @return The value or status produced by the operation.
 */
static const struct acpi_rsdp *acpi_scan_rsdp(void)
{
    const struct acpi_rsdp *rsdp;
    uint16_t ebda_segment = *(const volatile uint16_t *)(uintptr_t)0x40e;
    uint32_t ebda = (uint32_t)ebda_segment << 4;
    if (ebda >= 0x400U && ebda < 0xa0000U) {
        uint32_t ebda_end = ebda + 1024U;
        if (ebda_end > 0xa0000U) {
            ebda_end = 0xa0000U;
        }
        rsdp = acpi_scan_rsdp_range(ebda, ebda_end);
        if (rsdp) {
            return rsdp;
        }
    }
    return acpi_scan_rsdp_range(0xe0000U, 0x100000U);
}

/** @brief Resolve a complete ACPI physical span with the active mapping.
 * @param address Physical first byte. @param bytes Nonzero extent length.
 * @return Borrowed readable alias or NULL when its mapping cannot be proved.
 */
static const void *acpi_physical_span(uint64_t address, uint64_t bytes);

/** @brief Validate an RSDP without accessing bytes outside a proven span.
 * @param physical Physical RSDP address. @return Borrowed valid alias or NULL.
 */
static const struct acpi_rsdp *acpi_resolve_rsdp(uint64_t physical)
{
    const struct acpi_rsdp *rsdp = acpi_physical_span(physical, 20);
    if (!rsdp) return NULL;
    if (rsdp->revision >= 2) {
        rsdp = acpi_physical_span(physical, sizeof(*rsdp));
        if (!rsdp || rsdp->length < sizeof(*rsdp) || rsdp->length > 4096) return NULL;
        rsdp = acpi_physical_span(physical, rsdp->length);
        if (!rsdp) return NULL;
    }
    return acpi_rsdp_valid(rsdp) ? rsdp : NULL;
}

/** @brief Find an EFI configuration-table RSDP through proven identity spans.
 * @param system_table Live EFI system table physical address.
 * @return Borrowed valid RSDP alias or NULL. Called before ExitBootServices.
 */
static const struct acpi_rsdp *acpi_rsdp_from_efi(uint64_t system_table)
{
    static const uint8_t acpi_guids[][16] = {
        /* EFI_ACPI_20_TABLE_GUID and EFI_ACPI_TABLE_GUID. */
        {0x71, 0xe8, 0x68, 0x88, 0xf1, 0xe4, 0xd3, 0x11,
         0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81},
        {0x30, 0x2d, 0x9d, 0xeb, 0x88, 0x2d, 0xd3, 0x11,
         0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d},
    };
    uint64_t entry_count;
    uint64_t config_table;
    const uint8_t *st = acpi_physical_span(system_table, 120);
    if (!st) return NULL;
    /* EFI_SYSTEM_TABLE.ConfigurationTable fields are at offsets 104/112. */
    entry_count = acpi_read64(st, 104);
    config_table = acpi_read64(st, 112);
    if (!entry_count || entry_count > 1024U || !config_table) return NULL;
    const uint8_t *entries = acpi_physical_span(config_table, entry_count * 24U);
    if (!entries) return NULL;
    for (uint64_t i = 0; i < entry_count; ++i) {
        const uint8_t *entry = entries + i * 24U;
        uint64_t table_address = acpi_read64(entry, 16);
        for (uint32_t guid = 0; guid < sizeof(acpi_guids) / sizeof(acpi_guids[0]); ++guid) {
            if (acpi_guid_equal(entry, acpi_guids[guid])) {
                const struct acpi_rsdp *rsdp = acpi_resolve_rsdp(table_address);
                if (rsdp) return rsdp;
            }
        }
    }
    return 0;
}

/** @brief Check a span inside the Multiboot block already parsed by the loader.
 * @param start Physical first byte. @param bytes Nonzero bounded extent.
 * @return True only for bytes inside the validated borrowed handoff block.
 */
static bool acpi_multiboot_identity_range(uint64_t start, uint64_t bytes)
{
    const struct boot_info *boot = acpi_early_boot;
    if (!boot || !boot->multiboot_info) return false;
    const struct multiboot2_info *info = (const void *)(uintptr_t)boot->multiboot_info;
    uint64_t base = boot->multiboot_info, size = info->total_size;
    return start >= base && start - base < size && bytes <= size - (start - base);
}

/** @brief Resolve a bounded ACPI physical span in the current boot phase.
 * @param address Physical start. @param bytes Complete required byte length.
 * @return Borrowed proven identity or direct-map alias, or NULL.
 * The i386 entry proves its first 4 GiB; active EFI amd64 entry and descriptor
 * coverage prove firmware spans until sparse kernel mapping is published.
 */
static const void *acpi_physical_span(uint64_t address, uint64_t bytes)
{
    if (!address || !bytes || address >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        bytes > RELIEFNT_KERNEL_DIRECT_MAP_SIZE - address ||
        !paging_phys_addressable(address, bytes)) return NULL;
    if (!mm_bootstrap_topology()) {
        if (acpi_multiboot_identity_range(address, bytes)) return (const void *)(uintptr_t)address;
        if (acpi_early_boot && acpi_early_boot->efi_system_table)
            return firmware_identity_range(acpi_early_boot, address, bytes) ?
                (const void *)(uintptr_t)address : NULL;
        return acpi_early_boot && acpi_early_boot->bios_identity_4g &&
            address < 0x100000000ULL && bytes <= 0x100000000ULL - address ?
            (const void *)(uintptr_t)address : NULL;
    }
    if (!paging_kernel_direct_map_range(address, bytes)) return NULL;
    return paging_kernel_direct_map(address);
}

/** @brief Capture an EFI RSDP while boot services and identity mappings live.
 * @param boot Writable handoff with a fresh EFI map and active services.
 * Preserves a valid Multiboot RSDP, otherwise records the configuration-table
 * RSDP. This performs no firmware calls and never follows unproved pointers.
 */
void power_acpi_capture_efi_rsdp(struct boot_info *boot)
{
    if (!boot || !boot->efi_boot_services_active || !boot->efi_system_table) return;
    acpi_early_boot = boot;
    if (acpi_resolve_rsdp(boot->rsdp_addr)) return;
    const struct acpi_rsdp *rsdp = acpi_rsdp_from_efi(boot->efi_system_table);
    boot->efi_rsdp_addr = (uintptr_t)rsdp;
}

/**
 * @brief Find a validated root-listed ACPI table through the active mapping.
 * @param rsdp Borrowed readable RSDP in the current address space.
 * @param signature Four-byte table signature.
 * @return Borrowed validated table or NULL for absent, malformed or unmapped data.
 */
static const struct acpi_sdt_header *acpi_find_table(const struct acpi_rsdp *rsdp,
                                                      const char signature[4])
{
    if (!rsdp || !signature) {
        return 0;
    }
    for (uint32_t root_index = 0; root_index < 2U; ++root_index) {
        const uint8_t *root;
        uint64_t root_address;
        uint32_t root_length;
        uint32_t entry_size;
        uint32_t entry_count;
        if (root_index == 0U) {
            if (rsdp->revision < 2 || !rsdp->xsdt_address) {
                continue;
            }
            root_address = rsdp->xsdt_address;
            entry_size = 8;
        } else {
            if (!rsdp->rsdt_address) {
                continue;
            }
            root_address = rsdp->rsdt_address;
            entry_size = 4;
        }
        root = acpi_physical_span(root_address, sizeof(struct acpi_sdt_header));
        if (!root) continue;
        root_length = ((const struct acpi_sdt_header *)root)->length;
        if (root_length < sizeof(struct acpi_sdt_header) || root_length > 16U * 1024U * 1024U ||
            !(root = acpi_physical_span(root_address, root_length)) ||
            !acpi_checksum_valid(root, root_length)) {
            continue;
        }
        entry_count = (root_length - sizeof(struct acpi_sdt_header)) / entry_size;
        for (uint32_t i = 0; i < entry_count; ++i) {
            uint64_t address = entry_size == 8
                                   ? acpi_read64(root, sizeof(struct acpi_sdt_header) + i * 8U)
                                   : acpi_read32(root, sizeof(struct acpi_sdt_header) + i * 4U);
            const struct acpi_sdt_header *table;
            if (!address) {
                continue;
            }
            table = acpi_physical_span(address, sizeof(*table));
            if (!table) continue;
            if (acpi_bytes_equal(table->signature, signature, 4) &&
                table->length >= sizeof(struct acpi_sdt_header) && table->length <= 16U * 1024U * 1024U &&
                (table = acpi_physical_span(address, table->length)) &&
                acpi_checksum_valid((const uint8_t *)table, table->length)) {
                return table;
            }
        }
    }
    return 0;
}

/** @brief Record one verified nonempty ACPI physical span without allocation.
 * @param start Physical start. @param bytes Complete byte length.
 * @return 0, -EINVAL or -ENOSPC; duplicate exact spans do not consume slots.
 */
static int acpi_resource_add(uint64_t start, uint64_t bytes)
{
    if (!start || !bytes || start >= RELIEFNT_KERNEL_DIRECT_MAP_SIZE ||
        bytes > RELIEFNT_KERNEL_DIRECT_MAP_SIZE - start || !paging_phys_addressable(start, bytes))
        return -EINVAL;
    for (uint32_t i = 0; i < acpi_resource_count; ++i)
        if (acpi_resources[i].start == start && acpi_resources[i].bytes == bytes) return 0;
    if (acpi_resource_count == ACPI_RESOURCE_MAX) return -ENOSPC;
    acpi_resources[acpi_resource_count++] = (struct acpi_boot_resource){start, bytes};
    return 0;
}

/** @brief Record a bounded ACPI table if its complete physical extent is readable.
 * @param address Candidate physical SDT address.
 * @param table Optional output receiving the borrowed identity table pointer.
 * @return 1 for a recorded table, 0 for malformed input, or negative errno.
 */
static int acpi_resource_table(uint64_t address, const struct acpi_sdt_header **table)
{
    const struct acpi_sdt_header *header = acpi_physical_span(address, sizeof(*header));
    if (!header || header->length < sizeof(*header) || header->length > 16U * 1024U * 1024U ||
        !acpi_physical_span(address, header->length)) return 0;
    int rc = acpi_resource_add(address, header->length);
    if (rc) return rc;
    if (table) *table = header;
    return 1;
}

/** @brief Capture RSDP, root, referenced SDTs and the FADT's DSDT as boot resources.
 * @return 0 or negative errno. BSP before bootstrap publication while proven
 * firmware identity spans remain readable; malformed tables are skipped.
 */
int power_acpi_capture_resources(void)
{
    if (mm_bootstrap_topology()) return -EBUSY;
    acpi_resource_count = 0;
    const struct acpi_rsdp *rsdp = acpi_root_rsdp;
    if (!rsdp) return 0;
    uint64_t rsdp_bytes = rsdp->revision >= 2 ? rsdp->length : 20;
    int rc = acpi_resource_add((uintptr_t)rsdp, rsdp_bytes);
    if (rc) return rc;
    for (unsigned which = 0; which < 2; ++which) {
        uint64_t physical = which ? rsdp->rsdt_address :
            (rsdp->revision >= 2 ? rsdp->xsdt_address : 0);
        uint32_t entry_size = which ? 4 : 8;
        if (!physical) continue;
        const struct acpi_sdt_header *root;
        rc = acpi_resource_table(physical, &root);
        if (rc < 0) return rc;
        if (!rc || !acpi_checksum_valid((const uint8_t *)root, root->length)) continue;
        uint32_t count = (root->length - sizeof(*root)) / entry_size;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *raw = (const uint8_t *)root;
            uint32_t offset = sizeof(*root) + i * entry_size;
            uint64_t address = entry_size == 8 ? acpi_read64(raw, offset) : acpi_read32(raw, offset);
            const struct acpi_sdt_header *table;
            rc = acpi_resource_table(address, &table);
            if (rc < 0) return rc;
            if (!rc || !acpi_bytes_equal(table->signature, "FACP", 4) || table->length < 44) continue;
            const uint8_t *fadt = (const uint8_t *)table;
            uint64_t dsdt = acpi_read32(fadt, 40);
            if (table->length >= 148 && acpi_read64(fadt, 140)) dsdt = acpi_read64(fadt, 140);
            rc = acpi_resource_table(dsdt, NULL);
            if (rc < 0) return rc;
        }
    }
    return 0;
}

/** @brief Count ACPI spans captured before bootstrap allocation.
 * @return Stable count; no physical page ownership is transferred.
 */
uint32_t power_acpi_resource_count(void) { return acpi_resource_count; }

/** @brief Read one captured ACPI physical span.
 * @param index Zero-based inventory index. @param start Required base output.
 * @param bytes Required byte-length output.
 * @return True for a valid entry; false for invalid arguments or index.
 */
bool power_acpi_resource(uint32_t index, uint64_t *start, uint64_t *bytes)
{
    if (index >= acpi_resource_count || !start || !bytes) return false;
    *start = acpi_resources[index].start;
    *bytes = acpi_resources[index].bytes;
    return true;
}

/**
 * Acpi pkg length.
 * @param aml Value supplied by the caller.
 * @param length Maximum number of elements available in the related buffer.
 * @param consumed Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint32_t acpi_pkg_length(const uint8_t *aml, uint32_t length,
                                uint32_t *consumed)
{
    uint8_t lead;
    uint32_t follow;
    uint32_t value;
    if (!aml || !length || !consumed) {
        return 0;
    }
    lead = aml[0];
    follow = (lead >> 6) & 3U;
    if (follow + 1U > length) {
        return 0;
    }
    /**
 * @brief A one-byte PkgLength uses six payload bits; longer encodings use the low nibble and the following bytes.
 */
    value = follow ? (lead & 0x0fU) : (lead & 0x3fU);
    for (uint32_t i = 0; i < follow; ++i) {
        value |= (uint32_t)aml[i + 1U] << (4U + i * 8U);
    }
    *consumed = follow + 1U;
    return value;
}

/**
 * Acpi aml integer.
 * @param aml Value supplied by the caller.
 * @param length Maximum number of elements available in the related buffer.
 * @param consumed Value supplied by the caller.
 * @param value Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int acpi_aml_integer(const uint8_t *aml, uint32_t length,
                            uint32_t *consumed, uint16_t *value)
{
    uint32_t result;
    if (!aml || !length || !consumed || !value) {
        return 0;
    }
    switch (aml[0]) {
    case 0x00: /* ZeroOp */
        *consumed = 1;
        *value = 0;
        return 1;
    case 0x01: /* OneOp */
        *consumed = 1;
        *value = 1;
        return 1;
    case 0x0a: /**
 * @brief BytePrefix
 */
        if (length < 2) {
            return 0;
        }
        *consumed = 2;
        *value = aml[1];
        return 1;
    case 0x0b: /**
 * @brief WordPrefix
 */
        if (length < 3) {
            return 0;
        }
        *consumed = 3;
        *value = (uint16_t)(aml[1] | ((uint16_t)aml[2] << 8));
        return 1;
    case 0x0c: /**
 * @brief DWordPrefix
 */
        if (length < 5) {
            return 0;
        }
        result = acpi_read32(aml, 1);
        *consumed = 5;
        *value = (uint16_t)result;
        return 1;
    default:
        return 0;
    }
}

/**
 * Acpi find sleep types.
 * @param dsdt Value supplied by the caller.
 * @param sleep_type_a Value supplied by the caller.
 * @param sleep_type_b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static int acpi_find_sleep_types(const struct acpi_sdt_header *dsdt,
                                 uint16_t *sleep_type_a, uint16_t *sleep_type_b)
{
    const uint8_t *aml;
    uint32_t length;
    if (!dsdt || dsdt->length < sizeof(struct acpi_sdt_header) + 4U ||
        !sleep_type_a || !sleep_type_b) {
        return 0;
    }
    aml = (const uint8_t *)dsdt + sizeof(struct acpi_sdt_header);
    length = dsdt->length - sizeof(struct acpi_sdt_header);
    for (uint32_t i = 0; i + 4U < length; ++i) {
        uint32_t package_offset;
        uint32_t package_length;
        uint32_t package_header;
        uint32_t integer_length;
        uint16_t first;
        uint16_t second;
        if (aml[i] != '_' || aml[i + 1U] != 'S' ||
            aml[i + 2U] != '5' || aml[i + 3U] != '_') {
            continue;
        }
        package_offset = i + 4U;
        if (package_offset >= length || aml[package_offset] != 0x12) {
            continue;
        }
        ++package_offset;
        package_length = acpi_pkg_length(aml + package_offset,
                                         length - package_offset,
                                         &package_header);
        if (!package_length || package_length < package_header + 1U ||
            package_length > length - package_offset) {
            continue;
        }
        package_offset += package_header;
        {
            uint32_t package_end = (i + 5U) + package_length;
            if (package_offset >= package_end) {
                continue;
            }
            /**
 * @brief The first package byte is the element count.
 */
            if (aml[package_offset] < 2U) {
                continue;
            }
            ++package_offset;
            if (!acpi_aml_integer(aml + package_offset,
                                  package_end - package_offset,
                                  &integer_length, &first)) {
                continue;
            }
            package_offset += integer_length;
            if (!acpi_aml_integer(aml + package_offset,
                                  package_end - package_offset,
                                  &integer_length, &second)) {
                continue;
            }
        }
        if (first > 7U || second > 7U) {
            continue;
        }
        *sleep_type_a = (uint16_t)(first << 10);
        *sleep_type_b = (uint16_t)(second << 10);
        return 1;
    }
    return 0;
}

/**
 * Acpi gas io port.
 * @param fadt Value supplied by the caller.
 * @param offset Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
static uint16_t acpi_gas_io_port(const uint8_t *fadt, uint32_t offset)
{
    uint64_t address;
    if (!fadt) {
        return 0;
    }
    /**
 * @brief Generic Address Structure: address space 1 is system I/O.
 */
    if (fadt[offset] != 1 || fadt[offset + 1U] == 0) {
        return 0;
    }
    address = acpi_read64(fadt, offset + 4U);
    return address <= 0xffffU ? (uint16_t)address : 0;
}

/** @brief Locate validated ACPI tables and cache shutdown controls.
 * @param boot Borrowed handoff, with EFI RSDP captured before service exit.
 * BSP before root replacement; subsequent lookups use retained direct aliases.
 */
void power_init(const struct boot_info *boot)
{
    const struct acpi_rsdp *rsdp = 0;
    const struct acpi_sdt_header *fadt;
    const struct acpi_sdt_header *dsdt;
    const uint8_t *fadt_bytes;
    uint64_t dsdt_address;
    for (uint32_t i = 0; i < sizeof(acpi_power); ++i) {
        ((uint8_t *)&acpi_power)[i] = 0;
    }
    acpi_root_rsdp = 0;
    acpi_pm_timer_port = 0;
    acpi_pm_timer_mask = 0;
    acpi_early_boot = boot;
    if (boot) {
        rsdp = acpi_resolve_rsdp(boot->rsdp_addr);
        if (!rsdp) rsdp = acpi_resolve_rsdp(boot->efi_rsdp_addr);
    }
    if (!rsdp && boot && boot->bios_identity_4g && !boot->efi_system_table) {
        rsdp = acpi_scan_rsdp();
    }
    if (!rsdp) {
        console_warn("[reliefnt] ACPI RSDP unavailable\n");
        return;
    }
    acpi_root_rsdp = rsdp;
    fadt = acpi_find_table(rsdp, "FACP");
    if (!fadt || fadt->length < 90U) {
        console_warn("[reliefnt] ACPI FADT unavailable\n");
        return;
    }
    fadt_bytes = (const uint8_t *)fadt;
    /* The independent clock is usable even without a DSDT _S5 object. */
    acpi_pm_timer_init(fadt_bytes, fadt->length);
    acpi_power.smi_command = acpi_read32(fadt_bytes, 48);
    acpi_power.acpi_enable = fadt_bytes[52];
    acpi_power.pm1_control_length = fadt_bytes[89];
    {
        uint32_t pm1a = acpi_read32(fadt_bytes, 64);
        uint32_t pm1b = acpi_read32(fadt_bytes, 68);
        acpi_power.pm1a_control = pm1a <= 0xffffU ? (uint16_t)pm1a : 0;
        acpi_power.pm1b_control = pm1b <= 0xffffU ? (uint16_t)pm1b : 0;
    }
    if (fadt->length >= 196U) {
        uint16_t x_pm1a = acpi_gas_io_port(fadt_bytes, 172);
        uint16_t x_pm1b = acpi_gas_io_port(fadt_bytes, 184);
        if (x_pm1a) {
            acpi_power.pm1a_control = x_pm1a;
        }
        if (x_pm1b) {
            acpi_power.pm1b_control = x_pm1b;
        }
    }
    dsdt_address = acpi_read32(fadt_bytes, 40);
    if (fadt->length >= 148U) {
        uint64_t extended_dsdt = acpi_read64(fadt_bytes, 140);
        if (extended_dsdt) {
            dsdt_address = extended_dsdt;
        }
    }
    dsdt = acpi_physical_span(dsdt_address, sizeof(*dsdt));
    if (!dsdt || dsdt->length < sizeof(*dsdt) ||
        !(dsdt = acpi_physical_span(dsdt_address, dsdt->length))) {
        console_warn("[reliefnt] ACPI DSDT unavailable\n");
        return;
    }
    if (!acpi_bytes_equal(dsdt->signature, "DSDT", 4) ||
        dsdt->length < sizeof(struct acpi_sdt_header) ||
        !acpi_checksum_valid((const uint8_t *)dsdt, dsdt->length) ||
        !acpi_find_sleep_types(dsdt, &acpi_power.sleep_type_a,
                               &acpi_power.sleep_type_b)) {
        console_warn("[reliefnt] ACPI _S5_ sleep object unavailable\n");
        return;
    }
    if (!acpi_power.pm1a_control || acpi_power.pm1_control_length < 2U) {
        console_warn("[reliefnt] ACPI PM1 control block unavailable\n");
        return;
    }
    acpi_power.available = 1;
    console_info("[reliefnt] ACPI S5 shutdown ready pm1a=0x%x pm1b=0x%x\n",
                   acpi_power.pm1a_control, acpi_power.pm1b_control);
}

/** @brief Find a validated ACPI table without retaining a stale identity alias.
 * @param signature Four-byte signature supplied by the caller.
 * @return Borrowed validated table in the current address space or NULL.
 */
const void *power_acpi_find_table(const char signature[4])
{
    const struct acpi_rsdp *rsdp = acpi_resolve_rsdp((uintptr_t)acpi_root_rsdp);
    return acpi_find_table(rsdp, signature);
}

/**
 * Acpi enable power management.
 * @return The value or status produced by the operation.
 */
static int acpi_enable_power_management(void)
{
    if (!acpi_power.available) {
        return 0;
    }
    if (!(x86_64_inw(acpi_power.pm1a_control) & 1U) &&
        acpi_power.smi_command <= 0xffffU && acpi_power.acpi_enable) {
        x86_64_outb(acpi_power.acpi_enable,
                    (uint16_t)acpi_power.smi_command);
        for (uint32_t i = 0; i < 1000000U; ++i) {
            if (x86_64_inw(acpi_power.pm1a_control) & 1U) {
                break;
            }
            __asm__ volatile("pause");
        }
    }
    return (x86_64_inw(acpi_power.pm1a_control) & 1U) != 0;
}

/**
 * Power reboot.
 */
void power_reboot(void)
{
    console_info("[reliefnt] reboot requested\n");
    for (;;) {
        while (x86_64_inb(0x64) & 0x02) {
        }
        x86_64_outb(0xfe, 0x64);
        io_delay();
    }
}

/**
 * Power shutdown.
 */
void power_shutdown(void)
{
    console_info("[reliefnt] shutdown requested\n");
    if (acpi_enable_power_management()) {
        uint16_t sleep_enable = 1U << 13;
        uint16_t pm1a = x86_64_inw(acpi_power.pm1a_control);
        pm1a = (uint16_t)((pm1a & ~(uint16_t)(7U << 10)) |
                          acpi_power.sleep_type_a | sleep_enable);
        x86_64_outw(pm1a, acpi_power.pm1a_control);
        if (acpi_power.pm1b_control) {
            uint16_t pm1b = x86_64_inw(acpi_power.pm1b_control);
            pm1b = (uint16_t)((pm1b & ~(uint16_t)(7U << 10)) |
                              acpi_power.sleep_type_b | sleep_enable);
            x86_64_outw(pm1b, acpi_power.pm1b_control);
        }
        io_delay();
    }
    /**
 * @brief QEMU, Bochs and VirtualBox expose these legacy power ports.
 */
    x86_64_outw(0x2000, 0x604);
    io_delay();
    x86_64_outw(0x3400, 0xb004);
    io_delay();
    x86_64_outw(0x2000, 0x4004);
    io_delay();
    x86_64_outl(0x2000, 0x604);
    x86_64_halt();
}
