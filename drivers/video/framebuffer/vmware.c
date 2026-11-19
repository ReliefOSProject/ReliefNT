// SPDX-License-Identifier: Apache-2.0
#include <reliefnt/export.h>
#include "internal.h"
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/pat.h>
#include <linux/errno.h>

/* Only geometry is rolled back. FIFO cursors and completed generations must
 * never be copied back after the host has consumed commands. */
struct vmware_mode {
    uint32_t enable, width, height, depth, bpp, pseudocolor, traces;
};

/** @brief Read a register during boot, modeset, or single-owner panic. */
uint32_t vmware_svga_read(uint32_t reg)
{
    x86_64_outl(reg, vmware_svga.io_port);
    return x86_64_inl((uint16_t)(vmware_svga.io_port + 1u));
}

/** @brief Write a register under the same serialization as register reads. */
void vmware_svga_write(uint32_t reg, uint32_t value)
{
    x86_64_outl(reg, vmware_svga.io_port);
    x86_64_outl(value, (uint16_t)(vmware_svga.io_port + 1u));
}

/** @brief Order WC pixels before the UC FIFO or device doorbell. */
void vmware_svga_memory_fence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/** @brief Snapshot writable scanout state before disabling the device. */
static struct vmware_mode vmware_mode_read(void)
{
    return (struct vmware_mode){
        vmware_svga_read(VMWARE_SVGA_REG_ENABLE),
        vmware_svga_read(VMWARE_SVGA_REG_WIDTH),
        vmware_svga_read(VMWARE_SVGA_REG_HEIGHT),
        vmware_svga_read(VMWARE_SVGA_REG_DEPTH),
        vmware_svga_read(VMWARE_SVGA_REG_BITS_PER_PIXEL),
        vmware_svga_read(VMWARE_SVGA_REG_PSEUDOCOLOR),
        vmware_svga_read(VMWARE_SVGA_REG_TRACES),
    };
}

/** @brief Program a complete mode with scanout disabled during geometry writes. */
static void vmware_mode_write(const struct vmware_mode *mode)
{
    vmware_svga_write(VMWARE_SVGA_REG_ENABLE, 0);
    vmware_svga_write(VMWARE_SVGA_REG_WIDTH, mode->width);
    vmware_svga_write(VMWARE_SVGA_REG_HEIGHT, mode->height);
    vmware_svga_write(VMWARE_SVGA_REG_DEPTH, mode->depth);
    vmware_svga_write(VMWARE_SVGA_REG_BITS_PER_PIXEL, mode->bpp);
    vmware_svga_write(VMWARE_SVGA_REG_PSEUDOCOLOR, mode->pseudocolor);
    vmware_svga_write(VMWARE_SVGA_REG_ENABLE, mode->enable);
}

/** @brief Preserve a retained WC aperture; otherwise establish its UC mapping. */
static bool vmware_map_pixels(uint64_t physical, uint64_t bytes)
{
    return paging_mmio_writecombined(physical, bytes) ||
           paging_mmio_uncached(physical, bytes);
}

/** @brief Validate actual enabled scanout and prepare an unpublished descriptor.
 * @param candidate Caller-owned copy of the existing framebuffer.
 * @param width Requested width, or zero to accept the live boot mode.
 * @param height Requested height, or zero to accept the live boot mode.
 * @return True after mapping the actual FB_START/FB_OFFSET aperture. No global
 * framebuffer state is changed on any failure. Caller serializes device I/O.
 */
static bool vmware_scanout_read(struct framebuffer *candidate,
                                uint32_t width, uint32_t height)
{
    struct vmware_mode mode = vmware_mode_read();
    uint32_t pitch = vmware_svga_read(VMWARE_SVGA_REG_BYTES_PER_LINE);
    uint32_t start = vmware_svga_read(VMWARE_SVGA_REG_FB_START);
    uint32_t offset = vmware_svga_read(VMWARE_SVGA_REG_FB_OFFSET);
    uint32_t capacity = vmware_svga_read(VMWARE_SVGA_REG_FB_MAX_SIZE);
    uint64_t visible = (uint64_t)pitch * mode.height;
    if (!(mode.enable & 1u) || !mode.width || !mode.height ||
        mode.width > FRAMEBUFFER_MODE_MAX_WIDTH ||
        mode.height > FRAMEBUFFER_MODE_MAX_HEIGHT ||
        (width && mode.width != width) || (height && mode.height != height) ||
        mode.depth != 24u || mode.bpp != 32u || mode.pseudocolor ||
        (uint64_t)pitch < (uint64_t)mode.width * 4u || (pitch & 3u) ||
        !capacity || capacity > FRAMEBUFFER_MAX_VRAM_BYTES || offset >= capacity ||
        visible > (uint64_t)capacity - offset ||
        !framebuffer_range_valid(start, capacity) ||
        !framebuffer_range_valid((uint64_t)start + offset, visible) ||
        !vmware_map_pixels(start, capacity))
        return false;
    uint32_t *pixels = mm_phys_to_virt((uint64_t)start + offset);
    if (!pixels) return false;
    candidate->pixels = pixels;
    candidate->width = mode.width;
    candidate->height = mode.height;
    candidate->pitch = pitch;
    candidate->bpp = 32;
    candidate->bytes_per_pixel = 4;
    candidate->max_bytes = capacity - offset;
    candidate->reservation_start = start;
    candidate->reservation_bytes = capacity;
    candidate->available = true;
    return true;
}

/** @brief Bind the boot FIFO once and configure the shared transport.
 * @return Zero or an SVGA error. Boot-only, caller holds svga_lock. Mapping is
 * validated before touching CONFIG_DONE. Later modesets retain this binding.
 */
static int vmware_fifo_bind_locked(struct framebuffer *candidate)
{
    uint32_t start = vmware_svga_read(VMWARE_SVGA_REG_MEM_START);
    uint32_t bytes = vmware_svga_read(VMWARE_SVGA_REG_MEM_SIZE);
    if (bytes < 16384u || bytes > FRAMEBUFFER_MAX_VRAM_BYTES ||
        ((start | bytes) & 3u) || !framebuffer_range_valid(start, bytes) ||
        ((uint64_t)start < candidate->reservation_start + candidate->reservation_bytes &&
         candidate->reservation_start < (uint64_t)start + bytes) ||
        !paging_mmio_uncached(start, bytes))
        return SVGA_ENODEV;
    volatile uint32_t *fifo = mm_phys_to_virt(start);
    if (!fifo) return SVGA_ENODEV;
    svga_platform_bind(vmware_svga.io_port, fifo, bytes);
    int status = svga_configure_locked(false);
    if (status) return status;
    candidate->auxiliary_reservation_start = start;
    candidate->auxiliary_reservation_bytes = bytes;
    return 0;
}

/** @brief Copy the immutable PCI identity after successful boot binding. */
int framebuffer_vmware_pci_identity(struct pci_device *out)
{
    if (!out || !vmware_svga.present) return -ENODEV;
    *out = vmware_svga.pci;
    return 0;
}

/** @brief Promote only the prefetchable pixel BAR during single-CPU boot.
 * FIFO/register memory retains its separate mapping. Failure preserves UC.
 */
bool framebuffer_enable_writecombine_boot(void)
{
    if (!arch_pat_writecombine_ready() || !vmware_svga.present ||
        fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA) return false;
    const struct pci_device *pci = &vmware_svga.pci;
    uint32_t bar = pci_config_read32(pci->bus, pci->slot, pci->function, 0x14);
    uint64_t physical = bar & ~0xfULL;
    if ((bar & 1u) || !(bar & 8u)) return false;
    uint32_t type = bar & 6u;
    if (type == 4u)
        physical |= (uint64_t)pci_config_read32(pci->bus, pci->slot, pci->function, 0x18) << 32;
    else if (type != 0) return false;
    if (physical != fb.reservation_start || !fb.reservation_bytes) return false;
    uint64_t fifo = fb.auxiliary_reservation_start;
    uint64_t fifo_bytes = fb.auxiliary_reservation_bytes;
    if (fifo_bytes && physical < fifo + fifo_bytes &&
        fifo < physical + fb.reservation_bytes) return false;
    if (!paging_mmio_writecombine_boot(physical, fb.reservation_bytes)) return false;
    console_info("[framebuffer] VMware SVGA pixel cache=WC physical=%p bytes=%llu\n",
        (void *)(uintptr_t)physical, (unsigned long long)fb.reservation_bytes);
    return true;
}

/** @brief Restore mmap write tracing while the console owns the scanout. */
void framebuffer_vmware_enable_traces(void)
{
    uint64_t flags = svga_lock();
    if (vmware_svga.present && svga.bound && svga.fifo_ready &&
        !framebuffer_drm_owned())
        (void)svga_restore_traces_locked();
    svga_unlock(flags);
}

/** @brief Select a verified SVGA II scanout and its single FIFO at boot.
 * @return One on selection, zero preserving the boot surface. Boot runs before
 * other producers. Failed probes restore PCI decoding and writable geometry.
 */
int framebuffer_vmware_probe(void)
{
    struct pci_device device;
    if (!fb.available || pci_find_device(VMWARE_VENDOR_ID, VMWARE_SVGA_DEVICE_ID,
                                         &device) < 0) return 0;
    uint32_t bar = pci_config_read32(device.bus, device.slot, device.function, 0x10);
    uint32_t port = bar & 0xfffcu;
    if (!(bar & 1u) || !port || port > 0xfffeu) return 0;
    uint16_t command = pci_config_read16(device.bus, device.slot, device.function, 0x04);
    pci_config_write16(device.bus, device.slot, device.function, 0x04,
                      command | VMWARE_PCI_COMMAND_IO | VMWARE_PCI_COMMAND_MEMORY);
    vmware_svga.io_port = (uint16_t)port;
    uint32_t old_id = vmware_svga_read(VMWARE_SVGA_REG_ID);
    vmware_svga_write(VMWARE_SVGA_REG_ID, VMWARE_SVGA_ID_2);
    if (vmware_svga_read(VMWARE_SVGA_REG_ID) != VMWARE_SVGA_ID_2) {
        vmware_svga_write(VMWARE_SVGA_REG_ID, old_id);
        pci_config_write16(device.bus, device.slot, device.function, 0x04, command);
        return 0;
    }
    struct vmware_mode old_mode = vmware_mode_read();
    uint32_t old_config = vmware_svga_read(VMWARE_SVGA_REG_CONFIG_DONE);
    struct framebuffer candidate = fb;
    bool changed = !(old_mode.enable & 1u);
    if (changed) {
        struct vmware_mode requested = {1, fb.width, fb.height, 24, 32, 0, 0};
        vmware_mode_write(&requested);
    }
    uint32_t max_width = vmware_svga_read(VMWARE_SVGA_REG_MAX_WIDTH);
    uint32_t max_height = vmware_svga_read(VMWARE_SVGA_REG_MAX_HEIGHT);
    if (!max_width || !max_height ||
        !vmware_scanout_read(&candidate, changed ? fb.width : 0,
                             changed ? fb.height : 0)) goto failed;
    uint64_t flags = svga_lock();
    int status = vmware_fifo_bind_locked(&candidate);
    svga_unlock(flags);
    if (status) goto failed;
    if (max_width > FRAMEBUFFER_MODE_MAX_WIDTH) max_width = FRAMEBUFFER_MODE_MAX_WIDTH;
    if (max_height > FRAMEBUFFER_MODE_MAX_HEIGHT) max_height = FRAMEBUFFER_MODE_MAX_HEIGHT;
    vmware_svga.pci = device;
    vmware_svga.max_width = max_width;
    vmware_svga.max_height = max_height;
    vmware_svga.present = true;
    candidate.max_width = max_width;
    candidate.max_height = max_height;
    candidate.backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    candidate.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    fb = candidate;
    framebuffer_set_default_format();
    console_info("[reliefnt] VMware SVGA detected io=0x%x fb=%p mode=%ux%u pitch=%u fifo=%p\n",
        port, fb.pixels, fb.width, fb.height, fb.pitch, (void *)svga.fifo);
    return 1;
failed:
    if (changed) vmware_mode_write(&old_mode);
    vmware_svga_write(VMWARE_SVGA_REG_CONFIG_DONE, old_config);
    vmware_svga_write(VMWARE_SVGA_REG_TRACES, old_mode.traces);
    pci_config_write16(device.bus, device.slot, device.function, 0x04, command);
    return 0;
}

/** @brief Change geometry transactionally after draining all previous users.
 * @return One on commit, zero on rejection. The old descriptor stays published
 * until geometry, mapping and FIFO restart succeed. Failed changes restore the
 * old hardware mode and reconfigure a fresh FIFO generation, never old cursors.
 */
int framebuffer_vmware_set_mode(uint32_t width, uint32_t height)
{
    if (!vmware_svga.present || !width || !height ||
        width > vmware_svga.max_width || height > vmware_svga.max_height) return 0;
    uint64_t flags = svga_lock();
    bool accelerated = svga.available;
    struct vmware_mode old_mode = vmware_mode_read();
    if (svga_mode_prepare_locked()) {
        svga_unlock(flags);
        return 0;
    }
    struct framebuffer candidate = fb;
    struct vmware_mode requested = {1, width, height, 24, 32, 0, 0};
    vmware_mode_write(&requested);
    bool mapped = vmware_scanout_read(&candidate, width, height);
    int status = mapped ? svga_mode_configure_locked() : SVGA_EIO;
    if (mapped && svga.fifo_ready && (!accelerated || !status)) {
        fb = candidate;
        framebuffer_set_default_format();
        framebuffer_svga_clear_damage();
        svga_unlock(flags);
        return 1;
    }
    /* A failed 3D restart may have created GB tables. Retire them before the
     * old geometry is restored; never free pages without completion proof. */
    int retired = svga_shutdown_locked();
    if (!retired) {
        vmware_mode_write(&old_mode);
        int restored = svga_mode_configure_locked();
        struct framebuffer check = fb;
        if (!svga.fifo_ready || (accelerated && restored) ||
            !vmware_scanout_read(&check, old_mode.width, old_mode.height) ||
            check.pixels != fb.pixels || check.pitch != fb.pitch)
            fb.available = false;
        vmware_svga_write(VMWARE_SVGA_REG_TRACES, old_mode.traces);
        svga.traces_enabled = old_mode.traces != 0;
    } else {
        fb.available = false;
    }
    svga_unlock(flags);
    console_warn("[reliefnt] VMware SVGA rejected mode=%ux%u status=%d restored=%u\n",
                 width, height, status, fb.available);
    return 0;
}
