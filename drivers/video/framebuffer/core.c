#include "internal.h"
#include <reliefnt/lock.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>

static void framebuffer_set_boot_limits(void);

static void framebuffer_capture_gop_impl(uint64_t system_table_addr);

struct framebuffer fb;
static struct framebuffer gop_framebuffer;

struct vmware_svga_state vmware_svga;

bool framebuffer_drm_scanout_owned;

bool framebuffer_drm_damage_pending;

int framebuffer_range_valid(uint64_t start, uint64_t bytes)
{
    return start != 0 && bytes != 0 && start < FRAMEBUFFER_PHYS_LIMIT &&
           bytes <= FRAMEBUFFER_PHYS_LIMIT - start;
}

uint64_t framebuffer_current_bytes(void)
{
    return (uint64_t)fb.pitch * fb.height;
}

static void framebuffer_set_boot_limits(void)
{
    uint64_t bytes = framebuffer_current_bytes();

    fb.max_width = fb.width;
    fb.max_height = fb.height;
    fb.max_bytes = bytes <= UINT32_MAX ? (uint32_t)bytes : 0;
    fb.backend = FRAMEBUFFER_BACKEND_BOOT;
    fb.capabilities = 0;
    fb.reservation_start = mm_virt_to_phys(fb.pixels);
    fb.reservation_bytes = fb.max_bytes;
}

/** @brief Copy the active GOP mode while firmware virtual mappings still work.
 * @param system_table_addr Firmware EFI system table address.
 * @return None. Uses boot services only during early boot and keeps no firmware pointers.
 */
static void framebuffer_capture_gop_impl(uint64_t system_table_addr)
{
    if (gop_framebuffer.available || system_table_addr == 0) {
        return;
    }

    static struct efi_guid gop_guid = {
        0x9042a9de,
        0x23dc,
        0x4a38,
        {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a},
    };

    struct efi_system_table *st = (struct efi_system_table *)(uintptr_t)system_table_addr;
    if (!st || !st->boot_services || !st->boot_services->locate_handle_buffer || !st->boot_services->handle_protocol) {
        return;
    }

    uint64_t handle_count = 0;
    efi_handle_t *handles = 0;
    efi_status_t status = st->boot_services->locate_handle_buffer(2, &gop_guid, 0, &handle_count, &handles);
    if (status || handle_count == 0 || !handles) {
        console_error("[reliefnt] GOP locate failed status=0x%llx handles=%llu\n",
                       (unsigned long long)status,
                       (unsigned long long)handle_count);
        if (handles && st->boot_services->free_pool)
            st->boot_services->free_pool(handles);
        return;
    }

    bool found = false;
    for (uint64_t i = 0; i < handle_count; ++i) {
        void *interface = 0;
        status = st->boot_services->handle_protocol(handles[i], &gop_guid, &interface);
        if (status || !interface) {
            continue;
        }
        struct efi_graphics_output_protocol *gop = (struct efi_graphics_output_protocol *)interface;
        if (!gop->mode || !gop->mode->info || gop->mode->framebuffer_base == 0) {
            continue;
        }
        if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_BLT_ONLY) {
            continue;
        }
        uint64_t framebuffer_bytes =
            (uint64_t)gop->mode->info->pixels_per_scan_line *
            gop->mode->info->vertical_resolution * 4u;
        if (!framebuffer_bytes ||
            !paging_kernel_direct_map_range(gop->mode->framebuffer_base,
                                            framebuffer_bytes)) {
            continue;
        }
        gop_framebuffer.pixels = (uint32_t *)mm_phys_to_virt(gop->mode->framebuffer_base);
        if (!gop_framebuffer.pixels) continue;
        gop_framebuffer.width = gop->mode->info->horizontal_resolution;
        gop_framebuffer.height = gop->mode->info->vertical_resolution;
        gop_framebuffer.pitch = gop->mode->info->pixels_per_scan_line * 4;
        gop_framebuffer.bpp = 32;
        gop_framebuffer.bytes_per_pixel = 4u;
        gop_framebuffer.type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
        if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_RED_GREEN_BLUE) {
            gop_framebuffer.red_field_position = 0u;
            gop_framebuffer.red_mask_size = 8u;
            gop_framebuffer.green_field_position = 8u;
            gop_framebuffer.green_mask_size = 8u;
            gop_framebuffer.blue_field_position = 16u;
            gop_framebuffer.blue_mask_size = 8u;
        } else if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_BLUE_GREEN_RED) {
            gop_framebuffer.red_field_position = 16u;
            gop_framebuffer.red_mask_size = 8u;
            gop_framebuffer.green_field_position = 8u;
            gop_framebuffer.green_mask_size = 8u;
            gop_framebuffer.blue_field_position = 0u;
            gop_framebuffer.blue_mask_size = 8u;
        } else if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_BIT_MASK &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[0],
                                        &gop_framebuffer.red_field_position, &gop_framebuffer.red_mask_size) &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[1],
                                        &gop_framebuffer.green_field_position, &gop_framebuffer.green_mask_size) &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[2],
                                        &gop_framebuffer.blue_field_position, &gop_framebuffer.blue_mask_size)) {
            /* The masks above describe the native pixel layout directly. */
        } else {
            gop_framebuffer.red_field_position = 16u;
            gop_framebuffer.red_mask_size = 8u;
            gop_framebuffer.green_field_position = 8u;
            gop_framebuffer.green_mask_size = 8u;
            gop_framebuffer.blue_field_position = 0u;
            gop_framebuffer.blue_mask_size = 8u;
        }
        gop_framebuffer.available = framebuffer_color_format_valid(&gop_framebuffer);
        console_info("[reliefnt] GOP framebuffer base=%p size=%llu format=%u "
                       "R=%u/%u G=%u/%u B=%u/%u\n",
                       (void *)(uintptr_t)gop->mode->framebuffer_base,
                       (unsigned long long)gop->mode->framebuffer_size,
                       gop->mode->info->pixel_format,
                       gop_framebuffer.red_field_position, gop_framebuffer.red_mask_size,
                       gop_framebuffer.green_field_position, gop_framebuffer.green_mask_size,
                       gop_framebuffer.blue_field_position, gop_framebuffer.blue_mask_size);
        found = gop_framebuffer.available;
        if (found) break;
    }

    if (handles && st->boot_services->free_pool)
        st->boot_services->free_pool(handles);
    if (!found)
        console_info("[reliefnt] GOP protocol present but no usable mode\n");
}

/** @brief Snapshot UEFI GOP mode data before replacing firmware mappings.
 * @param system_table_addr Firmware-owned EFI system table address.
 * @return None. Restores the caller's interrupt state around firmware calls.
 */
void framebuffer_capture_gop(uint64_t system_table_addr)
{
    uint64_t irq_flags = kernel_irq_save();
    framebuffer_capture_gop_impl(system_table_addr);
    kernel_irq_restore(irq_flags);
}

void framebuffer_init(const struct boot_info *boot)
{
    int direct_color;

    fb.pixels = (uint32_t *)mm_phys_to_virt(boot->framebuffer_addr);
    fb.width = boot->framebuffer_width;
    fb.height = boot->framebuffer_height;
    fb.pitch = boot->framebuffer_pitch;
    fb.bpp = boot->framebuffer_bpp;
    fb.bytes_per_pixel = framebuffer_bytes_per_pixel(fb.pitch, fb.width, fb.bpp);
    fb.type = boot->framebuffer_type;
    fb.red_field_position = boot->framebuffer_red_field_position;
    fb.red_mask_size = boot->framebuffer_red_mask_size;
    fb.green_field_position = boot->framebuffer_green_field_position;
    fb.green_mask_size = boot->framebuffer_green_mask_size;
    fb.blue_field_position = boot->framebuffer_blue_field_position;
    fb.blue_mask_size = boot->framebuffer_blue_mask_size;
    direct_color = fb.type == MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
    if (direct_color && !framebuffer_color_format_valid(&fb)) {
        framebuffer_set_default_format();
    }
    fb.available = boot->framebuffer_addr != 0 &&
                   (boot->framebuffer_bpp == 24u || boot->framebuffer_bpp == 32u) &&
                   fb.bytes_per_pixel != 0u && fb.pixels &&
                   paging_kernel_direct_map_range(boot->framebuffer_addr,
                       (uint64_t)boot->framebuffer_pitch * boot->framebuffer_height) &&
                   (direct_color || boot->framebuffer_type == 0u);
    if (!fb.available && gop_framebuffer.available) {
        fb = gop_framebuffer;
    }
    framebuffer_set_boot_limits();
    if (!framebuffer_vmware_probe()) {
        (void)framebuffer_vbe_probe();
    }

    if (fb.available) {
        console_info("[reliefnt] framebuffer %ux%u pitch=%u bpp=%u bytespp=%u type=%u "
                       "R=%u/%u G=%u/%u B=%u/%u\n",
                       fb.width, fb.height, fb.pitch, fb.bpp, fb.bytes_per_pixel, fb.type,
                       fb.red_field_position, fb.red_mask_size,
                       fb.green_field_position, fb.green_mask_size,
                       fb.blue_field_position, fb.blue_mask_size);
        if (fb.capabilities & FRAMEBUFFER_CAP_MODE_SET) {
            console_info("[reliefnt] dynamic framebuffer backend=%u max=%ux%u vram=%u KiB\n",
                           fb.backend, fb.max_width, fb.max_height,
                           fb.max_bytes / 1024u);
        }
    } else {
        console_warn("[reliefnt] framebuffer unavailable, VGA fallback active\n");
    }
}

const struct framebuffer *framebuffer_get(void)
{
    return &fb;
}

int framebuffer_set_mode(uint32_t width, uint32_t height)
{
    struct framebuffer previous;
    uint64_t required_bytes;
    int changed;

    if (framebuffer_drm_owned())
        return fb.available && fb.width == width && fb.height == height ? 0 : -1;
    if (!fb.available || !(fb.capabilities & FRAMEBUFFER_CAP_MODE_SET) ||
        width == 0 || height == 0 || width > fb.max_width || height > fb.max_height) {
        return -1;
    }
    required_bytes = (uint64_t)width * height * 4u;
    if (required_bytes > fb.max_bytes) {
        return -1;
    }
    if (fb.width == width && fb.height == height && fb.bpp == 32u) {
        if (fb.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
            framebuffer_vmware_enable_traces();
        }
        return 0;
    }

    previous = fb;
    if (fb.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
        changed = framebuffer_vmware_set_mode(width, height);
    } else if (fb.backend == FRAMEBUFFER_BACKEND_BOCHS_VBE) {
        changed = framebuffer_vbe_set_mode(width, height);
    } else {
        return -1;
    }
    if (changed) {
        console_info("[reliefnt] framebuffer mode changed backend=%u %ux%u pitch=%u\n",
                       fb.backend, fb.width, fb.height, fb.pitch);
        return 0;
    }

    if (previous.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
        (void)framebuffer_vmware_set_mode(previous.width, previous.height);
    } else if (previous.backend == FRAMEBUFFER_BACKEND_BOCHS_VBE) {
        (void)framebuffer_vbe_set_mode(previous.width, previous.height);
    }
    fb = previous;
    return -1;
}
