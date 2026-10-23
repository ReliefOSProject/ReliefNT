#include "internal.h"

static void framebuffer_set_boot_limits(void);

static int guid_equal(const struct efi_guid *a, const struct efi_guid *b);

static void framebuffer_init_from_gop(uint64_t system_table_addr);

struct framebuffer fb;

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
    fb.reservation_start = (uint64_t)(uintptr_t)fb.pixels;
    fb.reservation_bytes = fb.max_bytes;
}

static int guid_equal(const struct efi_guid *a, const struct efi_guid *b)
{
    if (a->data1 != b->data1 || a->data2 != b->data2 || a->data3 != b->data3) {
        return 0;
    }
    for (uint32_t i = 0; i < 8; ++i) {
        if (a->data4[i] != b->data4[i]) {
            return 0;
        }
    }
    return 1;
}

static void framebuffer_init_from_gop(uint64_t system_table_addr)
{
    if (fb.available || system_table_addr == 0) {
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
        return;
    }

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
        fb.pixels = (uint32_t *)(uintptr_t)gop->mode->framebuffer_base;
        fb.width = gop->mode->info->horizontal_resolution;
        fb.height = gop->mode->info->vertical_resolution;
        fb.pitch = gop->mode->info->pixels_per_scan_line * 4;
        fb.bpp = 32;
        fb.bytes_per_pixel = 4u;
        fb.type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB;
        if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_RED_GREEN_BLUE) {
            fb.red_field_position = 0u;
            fb.red_mask_size = 8u;
            fb.green_field_position = 8u;
            fb.green_mask_size = 8u;
            fb.blue_field_position = 16u;
            fb.blue_mask_size = 8u;
        } else if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_BLUE_GREEN_RED) {
            fb.red_field_position = 16u;
            fb.red_mask_size = 8u;
            fb.green_field_position = 8u;
            fb.green_mask_size = 8u;
            fb.blue_field_position = 0u;
            fb.blue_mask_size = 8u;
        } else if (gop->mode->info->pixel_format == EFI_GOP_PIXEL_BIT_MASK &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[0],
                                        &fb.red_field_position, &fb.red_mask_size) &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[1],
                                        &fb.green_field_position, &fb.green_mask_size) &&
                   framebuffer_gop_mask(gop->mode->info->pixel_information[2],
                                        &fb.blue_field_position, &fb.blue_mask_size)) {
            /* The masks above describe the native pixel layout directly. */
        } else {
            framebuffer_set_default_format();
        }
        fb.available = framebuffer_color_format_valid(&fb);
        console_info("[reliefnt] GOP framebuffer base=%p size=%llu format=%u "
                       "R=%u/%u G=%u/%u B=%u/%u\n",
                       (void *)(uintptr_t)gop->mode->framebuffer_base,
                       (unsigned long long)gop->mode->framebuffer_size,
                       gop->mode->info->pixel_format,
                       fb.red_field_position, fb.red_mask_size,
                       fb.green_field_position, fb.green_mask_size,
                       fb.blue_field_position, fb.blue_mask_size);
        return;
    }

    console_info("[reliefnt] GOP protocol present but no usable mode\n");
    (void)guid_equal;
}

void framebuffer_init(const struct boot_info *boot)
{
    int direct_color;

    fb.pixels = (uint32_t *)(uintptr_t)boot->framebuffer_addr;
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
                   fb.bytes_per_pixel != 0u &&
                   (direct_color || boot->framebuffer_type == 0u);
    framebuffer_init_from_gop(boot->efi_system_table);
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
