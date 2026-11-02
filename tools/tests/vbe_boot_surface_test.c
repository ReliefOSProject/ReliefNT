/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <reliefnt/memory_topology.h>
#include "../../arch/x86_64/mm/internal.h"
#include "../../drivers/video/framebuffer/core.c"
#include "../../drivers/video/framebuffer/vbe.c"

#define ARENA 0x30000000ULL
#define ARENA_BYTES (128ULL * 4096)
#define VBE_BASE 0x70000000ULL
static uint16_t selected_register;
static uint64_t (*published_branches)[512];
static bool vbe_claimed;
static struct efi_graphics_output_protocol fixture_gop;
static efi_handle_t fixture_handle = (void *)1;
/** @brief Locate the fixture GOP handle. @param search Search type. @param protocol GUID.
 * @param key Optional key. @param count Handle-count output. @param handles Handle-list output.
 * @return EFI success with one borrowed fixture handle.
 */
static efi_status_t __attribute__((ms_abi)) fixture_locate(uint64_t search, struct efi_guid *protocol,
                                                           void *key, uint64_t *count, efi_handle_t **handles)
{ (void)protocol; (void)key; assert(search == 2); *count = 1; *handles = &fixture_handle; return 0; }
/** @brief Resolve the fixture GOP protocol. @param handle Fixture handle. @param protocol GUID.
 * @param interface Protocol output. @return EFI success.
 */
static efi_status_t __attribute__((ms_abi)) fixture_protocol(efi_handle_t handle, struct efi_guid *protocol,
                                                             void **interface)
{ (void)protocol; assert(handle == fixture_handle); *interface = &fixture_gop; return 0; }
/** @brief Model absent VMware hardware. @return Zero. */
int framebuffer_vmware_probe(void) { return 0; }
/** @brief Ignore fixture logs. @param level Severity. @param format Format string. */
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
/** @brief Select a VBE register. @param value Register index. @param port Output port. */
void x86_64_outw(uint16_t value, uint16_t port) { (void)port; selected_register = value; }
/** @brief Return a compatible VBE ID and 64 KiB aperture. @param port Input port. @return Register value. */
uint16_t x86_64_inw(uint16_t port)
{ (void)port; return selected_register == VBE_DISPI_INDEX_ID ? VBE_DISPI_ID5 : 1; }
/** @brief Identify the emulated VBE device. @param vendor Vendor. @param device Device.
 * @param out Writable PCI identity. @return Zero for the expected device. */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out)
{ assert(vendor == 0x1234 && device == 0x1111); *out = (struct pci_device){0}; return 0; }
/** @brief Return the disjoint VBE BAR. @param b Bus. @param s Slot. @param f Function.
 * @param offset Config offset. @return BAR physical address. */
uint32_t pci_config_read32(uint8_t b, uint8_t s, uint8_t f, uint8_t offset)
{ (void)b; (void)s; (void)f; assert(offset == 0x10); return VBE_BASE; }
/** @brief Accept an emulated early UC claim. @param p Physical start. @param n Bytes. @return True. */
bool paging_mmio_uncached(uint64_t p, uint64_t n)
{ vbe_claimed = p == VBE_BASE && n == 65536; return vbe_claimed; }
/** @brief Check fixture arena coverage. @param p First byte. @param n Length. @return Coverage. */
bool paging_kernel_direct_map_range(uint64_t p, uint64_t n)
{ return p >= ARENA && p - ARENA < ARENA_BYTES && n <= ARENA_BYTES - (p - ARENA); }
/** @brief Resolve arena physical storage. @param p Physical byte. @return Borrowed alias or NULL. */
void *paging_kernel_direct_map(uint64_t p)
{ return paging_kernel_direct_map_range(p, 1) ? (void *)(uintptr_t)p : NULL; }
/** @brief Find the fixture's mapped overlap. @param start First page. @param end End.
 * @param first Writable first output. @param last Writable end output. @return True on overlap. */
bool paging_kernel_direct_map_next(uint64_t start, uint64_t end, uint64_t *first, uint64_t *last)
{ *first = start > ARENA ? start : ARENA; *last = end < ARENA + ARENA_BYTES ? end : ARENA + ARENA_BYTES; return *first < *last; }
/** @brief Resolve an arena page-table page directly for this host fixture.
 * @param physical Bootstrap-owned page. @return Borrowed writable host alias.
 */
uint64_t *paging_trusted_table_alias(uint64_t physical)
{ assert(paging_kernel_direct_map_range(physical, 4096)); return (void *)(uintptr_t)physical; }
/** @brief Report executable-disable support for the staged root. @return True. */
bool paging_nx_ready(void) { return true; }
/** @brief Validate a range against the fixture CPU's 46-bit physical limit.
 * @param p Physical start. @param n Byte count. @return Whether the range fits.
 */
bool paging_phys_addressable(uint64_t p, uint64_t n)
{ return n && p < (1ULL << 46) && n <= (1ULL << 46) - p; }
/** @brief Replay the real VBE probe's early UC claim into staging.
 * @param index Claim index. @param start Physical base output. @param end Exclusive end.
 * @param cache_flags Policy output. @return True only for the captured VBE claim.
 */
bool paging_boot_mmio_run(uint32_t index, uint64_t *start, uint64_t *end, uint64_t *cache_flags)
{ if (index || !vbe_claimed) return false; *start=VBE_BASE; *end=VBE_BASE+65536; *cache_flags=RELIEFNT_PAGE_PWT|RELIEFNT_PAGE_PCD; return true; }
/** @brief Retain the staged root for inspection without changing the host CR3.
 * @param branches Image-owned page-directory pointer tables.
 */
void paging_publish_direct_map(uint64_t branches[128][512]) { published_branches=branches; }
/** @brief Inspect a staged 4 KiB or 2 MiB leaf after the simulated publication.
 * @param p Physical byte to resolve. @return Raw leaf with attributes and PPN.
 */
static uint64_t staged_leaf(uint64_t p)
{
    assert(published_branches);
    uint64_t e = published_branches[p >> 39][(p >> 30) & 511];
    assert(e & RELIEFNT_PAGE_PRESENT);
    uint64_t *pd = paging_trusted_table_alias(e & RELIEFNT_PHYS_ADDR_MASK);
    e = pd[(p >> 21) & 511];
    assert(e & RELIEFNT_PAGE_PRESENT);
    if (e & 0x80) return e;
    uint64_t *pt = paging_trusted_table_alias(e & RELIEFNT_PHYS_ADDR_MASK);
    e = pt[(p >> 12) & 511];
    assert(e & RELIEFNT_PAGE_PRESENT);
    return e;
}
/** @brief Report no fixture ACPI resources. @return Zero. */
uint32_t power_acpi_resource_count(void) { return 0; }
/** @brief Reject absent fixture ACPI resources. @param i Index. @param p Base output.
 * @param n Length output. @return False. */
bool power_acpi_resource(uint32_t i, uint64_t *p, uint64_t *n) { (void)i; (void)p; (void)n; return false; }

/** @brief Exercise GOP capture, VBE probe, bootstrap and sparse-map staging.
 * @param argc Argument count. @param argv Optional "high" selects an unmapped high GOP.
 * @return Zero when disjoint GOP pixels and VBE BAR have retained UC/NX leaves.
 */
int main(int argc, char **argv)
{
    uint8_t *arena = mmap((void *)ARENA, ARENA_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(arena != MAP_FAILED);
    bool high = argc > 1 && !strcmp(argv[1], "high");
    uint64_t gop = high ? (32ULL << 30) : ARENA + 4096;
    assert(paging_kernel_direct_map_range(gop, 4096) == !high);
    struct efi_gop_mode_info info = {.horizontal_resolution=1024, .vertical_resolution=1,
        .pixel_format=EFI_GOP_PIXEL_BLUE_GREEN_RED, .pixels_per_scan_line=1024};
    struct efi_gop_mode mode = {.info=&info, .framebuffer_base=gop, .framebuffer_size=4096};
    fixture_gop.mode = &mode;
    struct efi_boot_services services = {.locate_handle_buffer=fixture_locate, .handle_protocol=fixture_protocol};
    struct efi_system_table system = {.boot_services=&services};
    framebuffer_capture_gop_impl((uintptr_t)&system);
    assert(gop_framebuffer.available && gop_framebuffer.reservation_start == gop &&
           gop_framebuffer.reservation_bytes == 4096);
    if (high) assert(!paging_kernel_direct_map(gop));
    struct boot_info no_framebuffer_tag = {0};
    framebuffer_init(&no_framebuffer_tag);
    assert(fb.available && (uintptr_t)fb.pixels == RELIEFNT_KERNEL_DIRECT_MAP_BASE + gop);
    assert(fb.reservation_start == VBE_BASE && fb.auxiliary_reservation_start == gop &&
           fb.auxiliary_reservation_bytes == 4096);
    struct multiboot2_mmap_entry *map = (void *)(arena + 0x8000);
    *map = (struct multiboot2_mmap_entry){ARENA, ARENA_BYTES, 1, 0};
    struct boot_info boot = {.mmap_addr=(uintptr_t)map, .mmap_entry_count=1, .mmap_entry_size=sizeof(*map)};
    assert(mm_bootstrap_prepare(&boot, 46) == 0);
    const struct mm_topology *topology = mm_bootstrap_topology();
    bool reserved = false;
    for (size_t i = 0; i < topology->range_count; ++i)
        if (topology->ranges[i].start <= gop && topology->ranges[i].end > gop)
            reserved = (topology->ranges[i].flags & MM_TOPOLOGY_RESERVED) != 0;
    assert(reserved && mm_bootstrap_alloc(1, UINT64_MAX) != gop);
    assert(paging_direct_map_init(topology) == 0);
    const uint64_t uc_nx = RELIEFNT_PAGE_PRESENT | RELIEFNT_PAGE_WRITABLE |
        RELIEFNT_PAGE_PWT | RELIEFNT_PAGE_PCD | RELIEFNT_PAGE_NOEXEC;
    uint64_t gop_leaf = staged_leaf(gop), vbe_leaf = staged_leaf(VBE_BASE);
    assert((gop_leaf & uc_nx) == uc_nx && (vbe_leaf & uc_nx) == uc_nx);
    assert((gop_leaf & RELIEFNT_PHYS_ADDR_MASK) == gop);
    assert((vbe_leaf & RELIEFNT_PHYS_ADDR_MASK) == VBE_BASE);
    puts(high ? "PASS unmapped high GOP fallback retained through VBE and UC/NX staging" :
                "PASS GOP RAM retained through VBE, bootstrap and UC/NX staging");
    return 0;
}

/* These non-PAT fixtures retain the original UC-only aperture. */
bool paging_mmio_writecombined(uint64_t physical,uint64_t bytes)
{ (void)physical;(void)bytes;return false; }
