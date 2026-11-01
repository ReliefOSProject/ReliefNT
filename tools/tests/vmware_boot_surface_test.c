/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/video/framebuffer/vmware.c"

#define BOOT_BASE 0x30000000ULL
#define DEVICE_BASE 0x70000000ULL
struct framebuffer fb;
struct vmware_svga_state vmware_svga;
struct svga_device svga;
static uint32_t registers[64], selected;
static uint32_t enable_writes[8], enable_write_count;
static bool deny_alias, deny_uc, invalid_after_enable, deny_enable_ack;

/** @brief Emulate SVGA register writes, requiring disabled geometry changes.
 * @param value Register index or data. @param port I/O port.
 */
void x86_64_outl(uint32_t value, uint16_t port)
{
    if (port == 0x1000) { selected = value; return; }
    assert(port == 0x1001);
    if (selected == VMWARE_SVGA_REG_ENABLE) {
        assert(enable_write_count < sizeof(enable_writes) / sizeof(enable_writes[0]));
        enable_writes[enable_write_count++] = value;
    }
    if (selected == VMWARE_SVGA_REG_WIDTH || selected == VMWARE_SVGA_REG_HEIGHT ||
        selected == VMWARE_SVGA_REG_DEPTH || selected == VMWARE_SVGA_REG_BITS_PER_PIXEL ||
        selected == VMWARE_SVGA_REG_PSEUDOCOLOR)
        assert(!(registers[VMWARE_SVGA_REG_ENABLE] & 1u));
    registers[selected] = value;
}
/** @brief Read an SVGA register, optionally rejecting the newly enabled mode.
 * @param port I/O port. @return Emulated value or injected invalid width/enable.
 */
uint32_t x86_64_inl(uint16_t port)
{
    assert(port == 0x1001);
    if (invalid_after_enable && selected == VMWARE_SVGA_REG_WIDTH &&
        (registers[VMWARE_SVGA_REG_ENABLE] & 1u)) return 0;
    if (deny_enable_ack && selected == VMWARE_SVGA_REG_ENABLE &&
        (registers[VMWARE_SVGA_REG_ENABLE] & 1u)) return 0;
    return registers[selected];
}
/** @brief Identify the emulated VMware device. @param vendor Vendor. @param device Device.
 * @param out Required PCI output. @return Zero on match.
 */
int pci_find_device(uint16_t vendor, uint16_t device, struct pci_device *out)
{ assert(vendor == VMWARE_VENDOR_ID && device == VMWARE_SVGA_DEVICE_ID); *out = (struct pci_device){0}; return 0; }
/** @brief Supply the device I/O BAR. @param b Bus. @param s Slot. @param f Function.
 * @param reg Config offset. @return I/O BAR.
 */
uint32_t pci_config_read32(uint8_t b, uint8_t s, uint8_t f, uint8_t reg)
{ (void)b; (void)s; (void)f; assert(reg == 0x10); return 0x1001; }
/** @brief Supply PCI command state. @param b Bus. @param s Slot. @param f Function.
 * @param reg Config offset. @return Zero initial command.
 */
uint16_t pci_config_read16(uint8_t b, uint8_t s, uint8_t f, uint8_t reg)
{ (void)b; (void)s; (void)f; (void)reg; return 0; }
/** @brief Accept a PCI command update. @param b Bus. @param s Slot. @param f Function.
 * @param reg Offset. @param value Command bits.
 */
void pci_config_write16(uint8_t b, uint8_t s, uint8_t f, uint8_t reg, uint16_t value)
{ (void)b; (void)s; (void)f; (void)reg; (void)value; }
/** @brief Report the active boot surface extent. @return Byte count. */
uint64_t framebuffer_current_bytes(void) { return (uint64_t)fb.pitch * fb.height; }
/** @brief Validate nonempty fixture device/surface ranges. @param p Physical base.
 * @param n Byte count. @return Nonzero for fixture apertures.
 */
int framebuffer_range_valid(uint64_t p, uint64_t n) { return p && n; }
/** @brief Model or reject a UC claim. @param p Physical base. @param n Length.
 * @return True unless this case injects a claim failure.
 */
bool paging_mmio_uncached(uint64_t p, uint64_t n)
{ assert(p == DEVICE_BASE && n == 65536); return !deny_uc; }
/** @brief Resolve a device alias or inject mapping failure. @param p Physical base.
 * @return Synthetic direct-map pointer or NULL when failure is selected.
 */
void *mm_phys_to_virt(uint64_t p)
{ return deny_alias ? NULL : (void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + p); }
/** @brief Decode an existing fixture direct alias. @param p Borrowed pointer.
 * @return Synthetic physical address.
 */
uint64_t mm_virt_to_phys(const void *p) { return (uintptr_t)p - RELIEFNT_KERNEL_DIRECT_MAP_BASE; }
/** @brief Accept the fixture's default color format. */
void framebuffer_set_default_format(void) { }
/** @brief Ignore fixture diagnostics. @param level Level. @param format Format string. */
void console_log(enum console_log_level level, const char *format, ...) { (void)level; (void)format; }
/** @brief Model the SVGA lock. @return Saved flags. */
uint64_t svga_lock(void) { return 0; }
/** @brief Release the fixture lock. @param flags Saved flags. */
void svga_unlock(uint64_t flags) { (void)flags; }
/** @brief Accept fixture tracing restoration. @return True. */
bool svga_restore_traces_locked(void) { return true; }
/** @brief Ignore unused FIFO binding. @param port I/O port. @param fifo FIFO alias. @param bytes FIFO size. */
void svga_platform_bind(uint16_t port, volatile uint32_t *fifo, uint32_t bytes)
{ (void)port; (void)fifo; (void)bytes; }

/** @brief Exercise one real probe with distinct GOP and SVGA surfaces.
 * @param argc Argument count. @param argv Required scenario name.
 * @return Zero when failure preserves scanout/geometry and success selects SVGA.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *scenario = argv[1];
    bool already_enabled = !strncmp(scenario, "enabled_", 8);
    bool success = !strcmp(scenario, "success") || !strcmp(scenario, "enabled_success");
    bool bad_mode = !strcmp(scenario, "bad_mode");
    deny_enable_ack = !strcmp(scenario, "no_enable_ack");
    deny_uc = !strcmp(scenario, "uc_failure");
    deny_alias = !strcmp(scenario, "alias_failure") || !strcmp(scenario, "enabled_alias_failure");
    bool bad_width = !strcmp(scenario, "max_width") || !strcmp(scenario, "enabled_max_width");
    bool bad_height = !strcmp(scenario, "max_height");
    assert(success || bad_mode || deny_enable_ack || deny_uc || deny_alias || bad_width || bad_height);
    invalid_after_enable = bad_mode;
    struct framebuffer boot = {.available=true, .pixels=(void *)(uintptr_t)(RELIEFNT_KERNEL_DIRECT_MAP_BASE + BOOT_BASE),
        .width=16, .height=1, .pitch=64, .reservation_start=BOOT_BASE, .reservation_bytes=64};
    registers[VMWARE_SVGA_REG_FB_START] = DEVICE_BASE;
    registers[VMWARE_SVGA_REG_FB_OFFSET] = 0;
    registers[VMWARE_SVGA_REG_FB_MAX_SIZE] = 65536;
    registers[VMWARE_SVGA_REG_FB_SIZE] = 65536;
    registers[VMWARE_SVGA_REG_WIDTH] = already_enabled ? 16 : 23;
    registers[VMWARE_SVGA_REG_HEIGHT] = already_enabled ? 1 : 3;
    registers[VMWARE_SVGA_REG_DEPTH] = already_enabled ? 24 : 16;
    registers[VMWARE_SVGA_REG_BITS_PER_PIXEL] = already_enabled ? 32 : 16;
    registers[VMWARE_SVGA_REG_PSEUDOCOLOR] = already_enabled ? 0 : 1;
    registers[VMWARE_SVGA_REG_BYTES_PER_LINE] = 64;
    registers[VMWARE_SVGA_REG_ENABLE] = already_enabled ? 1 : 2;
    registers[VMWARE_SVGA_REG_MAX_WIDTH] = bad_width ? 0 : 1024;
    registers[VMWARE_SVGA_REG_MAX_HEIGHT] = bad_height ? 0 : 1024;
    const uint32_t old_enable = registers[VMWARE_SVGA_REG_ENABLE];
    const uint32_t old_width = registers[VMWARE_SVGA_REG_WIDTH];
    const uint32_t old_height = registers[VMWARE_SVGA_REG_HEIGHT];
    const uint32_t old_depth = registers[VMWARE_SVGA_REG_DEPTH];
    const uint32_t old_bpp = registers[VMWARE_SVGA_REG_BITS_PER_PIXEL];
    const uint32_t old_pseudocolor = registers[VMWARE_SVGA_REG_PSEUDOCOLOR];
    const uint32_t old_pitch = registers[VMWARE_SVGA_REG_BYTES_PER_LINE];
    fb = boot;
    int rc = framebuffer_vmware_probe();
    if (!success) {
        assert(!rc);
        assert(fb.available == boot.available && fb.pixels == boot.pixels &&
               fb.width == boot.width && fb.height == boot.height && fb.pitch == boot.pitch &&
               fb.reservation_start == BOOT_BASE && fb.reservation_bytes == 64);
        assert(registers[VMWARE_SVGA_REG_ENABLE] == old_enable);
        assert(registers[VMWARE_SVGA_REG_WIDTH] == old_width);
        assert(registers[VMWARE_SVGA_REG_HEIGHT] == old_height);
        assert(registers[VMWARE_SVGA_REG_DEPTH] == old_depth);
        assert(registers[VMWARE_SVGA_REG_BITS_PER_PIXEL] == old_bpp);
        assert(registers[VMWARE_SVGA_REG_PSEUDOCOLOR] == old_pseudocolor);
        assert(registers[VMWARE_SVGA_REG_BYTES_PER_LINE] == old_pitch);
        if (already_enabled) assert(!enable_write_count);
        else assert(enable_write_count == 4 && enable_writes[0] == 0 && enable_writes[1] == 1 &&
                    enable_writes[2] == 0 && enable_writes[3] == old_enable);
    } else {
        assert(rc);
        assert((uintptr_t)fb.pixels == RELIEFNT_KERNEL_DIRECT_MAP_BASE + DEVICE_BASE &&
               fb.reservation_start == DEVICE_BASE && fb.reservation_bytes == 65536);
        assert((registers[VMWARE_SVGA_REG_ENABLE] & 1u) &&
               registers[VMWARE_SVGA_REG_WIDTH] == 16 && registers[VMWARE_SVGA_REG_HEIGHT] == 1 &&
               registers[VMWARE_SVGA_REG_DEPTH] == 24 && registers[VMWARE_SVGA_REG_BITS_PER_PIXEL] == 32 &&
               registers[VMWARE_SVGA_REG_PSEUDOCOLOR] == 0);
        if (already_enabled) assert(!enable_write_count);
        else assert(enable_write_count == 2 && enable_writes[0] == 0 && enable_writes[1] == 1);
    }
    printf("PASS VMware probe %s\n", scenario);
    return 0;
}
