#include "internal.h"

#include <reliefnt/mm.h>
#include <reliefnt/paging.h>

static uint16_t vbe_read(uint16_t index);

static void vbe_write(uint16_t index, uint16_t value);

static struct bochs_vbe_state bochs_vbe;
static uint16_t vbe_read(uint16_t index)
{
    x86_64_outw(index, VBE_DISPI_IOPORT_INDEX);
    return x86_64_inw(VBE_DISPI_IOPORT_DATA);
}

static void vbe_write(uint16_t index, uint16_t value)
{
    x86_64_outw(index, VBE_DISPI_IOPORT_INDEX);
    x86_64_outw(value, VBE_DISPI_IOPORT_DATA);
}

int framebuffer_vbe_probe(void)
{
    struct pci_device device;
    uint32_t vbe_id;
    uint32_t vram_64k;
    uint64_t vram_bytes;
    uint32_t bar;
    uint32_t lfb_start;

    if (!fb.available) {
        return 0;
    }
    vbe_id = vbe_read(VBE_DISPI_INDEX_ID);
    if (vbe_id < VBE_DISPI_ID0 || vbe_id > VBE_DISPI_ID5) {
        return 0;
    }
    vram_64k = vbe_read(VBE_DISPI_INDEX_VIDEO_MEMORY_64K);
    vram_bytes = (uint64_t)vram_64k * 65536u;
    if (!vram_bytes || vram_bytes > FRAMEBUFFER_MAX_VRAM_BYTES ||
        vram_bytes < framebuffer_current_bytes()) {
        return 0;
    }

    /* The VBE registers select the LFB in the VGA PCI BAR, not the GOP
     * framebuffer reported at boot.  Keep that address authoritative for
     * every mode change; continuing to draw through a stale GOP address
     * leaves QEMU scanning a black VBE surface. */
    if (pci_find_device(0x1234u, 0x1111u, &device) < 0) {
        return 0;
    }
    bar = pci_config_read32(device.bus, device.slot, device.function, 0x10);
    lfb_start = bar & 0xfffffff0u;
    if ((bar & 1u) != 0 ||
        !framebuffer_range_valid((uint64_t)lfb_start, vram_bytes) ||
        !paging_mmio_uncached((uint64_t)lfb_start, vram_bytes)) {
        return 0;
    }

    bochs_vbe.lfb_start = lfb_start;
    bochs_vbe.vram_bytes = (uint32_t)vram_bytes;
    bochs_vbe.present = true;
    fb.max_width = FRAMEBUFFER_MODE_MAX_WIDTH;
    fb.max_height = FRAMEBUFFER_MODE_MAX_HEIGHT;
    fb.max_bytes = (uint32_t)vram_bytes;
    fb.backend = FRAMEBUFFER_BACKEND_BOCHS_VBE;
    fb.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    fb.reservation_start = lfb_start;
    fb.reservation_bytes = (uint32_t)vram_bytes;
    return 1;
}

int framebuffer_vbe_set_mode(uint32_t width, uint32_t height)
{
    uint32_t actual_width;
    uint32_t actual_height;
    uint32_t virtual_width;
    uint64_t pitch;

    if (!bochs_vbe.present ||
        !framebuffer_range_valid(bochs_vbe.lfb_start, bochs_vbe.vram_bytes)) {
        return 0;
    }

    vbe_write(VBE_DISPI_INDEX_ENABLE, 0);
    vbe_write(VBE_DISPI_INDEX_XRES, (uint16_t)width);
    vbe_write(VBE_DISPI_INDEX_YRES, (uint16_t)height);
    vbe_write(VBE_DISPI_INDEX_BPP, 32u);
    vbe_write(VBE_DISPI_INDEX_ENABLE, VBE_DISPI_ENABLED | VBE_DISPI_LFB_ENABLED);

    actual_width = vbe_read(VBE_DISPI_INDEX_XRES);
    actual_height = vbe_read(VBE_DISPI_INDEX_YRES);
    virtual_width = vbe_read(VBE_DISPI_INDEX_VIRT_WIDTH);
    pitch = (uint64_t)virtual_width * 4u;
    if (actual_width != width || actual_height != height ||
        virtual_width < width || pitch > UINT32_MAX ||
        pitch * height > fb.max_bytes) {
        return 0;
    }
    fb.pixels = (uint32_t *)mm_phys_to_virt(bochs_vbe.lfb_start);
    if (!fb.pixels) return 0;
    fb.width = width;
    fb.height = height;
    fb.pitch = (uint32_t)pitch;
    fb.bpp = 32u;
    fb.bytes_per_pixel = 4u;
    fb.reservation_start = bochs_vbe.lfb_start;
    fb.reservation_bytes = bochs_vbe.vram_bytes;
    framebuffer_set_default_format();
    fb.available = true;
    return 1;
}
