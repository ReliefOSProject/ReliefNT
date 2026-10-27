/*
 * ReliefOS framebuffer interface: declares early and kernel display services.
 * Handles framebuffer discovery, pixel output, fonts, and console rendering.
 */
#ifndef RELIEFNT_FRAMEBUFFER_H
#define RELIEFNT_FRAMEBUFFER_H

#include <reliefnt/multiboot2.h>
#include <reliefnt/types.h>

struct pci_device;
/** @brief Copy the immutable identity established by successful boot binding.
 * @param out Required writable PCI output; unchanged when unavailable.
 * @return Zero or -ENODEV. Called after boot framebuffer binding, with no
 * allocation, I/O, optional-provider dependency or ownership transfer.
 */
int framebuffer_vmware_pci_identity(struct pci_device *out);

struct framebuffer {
    uint32_t *pixels;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bytes;
    uint32_t backend;
    uint64_t reservation_start;
    uint32_t reservation_bytes;
    uint64_t auxiliary_reservation_start;
    uint32_t auxiliary_reservation_bytes;
    uint8_t bpp;
    uint8_t bytes_per_pixel;
    uint16_t capabilities;
    uint8_t type;
    uint8_t red_field_position;
    uint8_t red_mask_size;
    uint8_t green_field_position;
    uint8_t green_mask_size;
    uint8_t blue_field_position;
    uint8_t blue_mask_size;
    bool available;
};

struct framebuffer_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
};

struct framebuffer_capabilities {
    uint8_t bytes_per_pixel;
    uint8_t reserved;
    uint16_t capabilities;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t max_bytes;
    uint32_t backend;
};

#define FRAMEBUFFER_CAP_MODE_SET 0x0001u

#define FRAMEBUFFER_BACKEND_BOOT 0u
#define FRAMEBUFFER_BACKEND_BOCHS_VBE 1u
#define FRAMEBUFFER_BACKEND_VMWARE_SVGA 2u

struct framebuffer_mode_cmd {
    uint32_t width;
    uint32_t height;
};

struct framebuffer_rect_cmd {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t color;
};

struct framebuffer_text_cmd {
    uint32_t x;
    uint32_t y;
    uint32_t fg;
    uint32_t bg;
    const char *text;
};

struct framebuffer_blit_cmd {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    const uint32_t *pixels;
};

/**
 * @brief Discover and set up the framebuffer described by the boot info.
 */
void framebuffer_init(const struct boot_info *boot);
/**
 * @brief Snapshot UEFI GOP display metadata while boot services are callable.
 * @param system_table_addr Firmware-owned EFI system table address, readable
 *        before the kernel replaces firmware mappings.
 * @return None. Retains only copied geometry/format and a physical framebuffer
 *         address translated later through the kernel direct map.
 */
void framebuffer_capture_gop(uint64_t system_table_addr);
/**
 * @brief Borrow the permanent framebuffer descriptor without allocating or probing hardware.
 * @return Stable core-owned descriptor, whose available flag describes actual binding.
 * Boot/task/IRQ snapshot context. No ownership is transferred; callers serialize mutable mode fields with the existing display/execution policy.
 */
const struct framebuffer *framebuffer_get(void);
struct fb_var_screeninfo;
struct fb_fix_screeninfo;
/** @brief Fill Linux fbdev fixed information with the framebuffer's physical PFN.
 * @param fb Active framebuffer whose pixels pointer is a kernel CPU alias.
 * @param info Caller-owned fixed-screen-info output.
 * @return True when the active pixel alias translates to a supported physical
 *         address; false for missing pointers or an invalid direct-map alias.
 */
bool framebuffer_fbdev_get_fix_info(const struct framebuffer *fb,
                                    struct fb_fix_screeninfo *info);
/**
 * @brief Validate and apply one supported Linux fbdev variable-screen request.
 * @param var Input request; on success replaced with the normalized layout.
 * @return Zero on success, or a negative Linux errno without changing var.
 */
int framebuffer_fbdev_put_var(struct fb_var_screeninfo *var);
/**
 * @brief Switch the display to width x height; returns 0 on success.
 */
int framebuffer_set_mode(uint32_t width, uint32_t height);
/**
 * @brief Fill the whole screen with color.
 */
void framebuffer_clear(uint32_t color);
/**
 * @brief Fill the axis-aligned rectangle at (x,y) of size w x h with color.
 */
void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
/**
 * @brief Fill a rectangle in an explicit framebuffer without changing the
 *        active framebuffer descriptor.
 * @param target Framebuffer whose pixel storage receives the drawing.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param w Rectangle width in pixels.
 * @param h Rectangle height in pixels.
 * @param color Logical RGB color to fill.
 */
void framebuffer_rect_to(const struct framebuffer *target,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t color);
/**
 * @brief Draw text at (x,y) using fg as the foreground and bg as the background color.
 */
void framebuffer_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);
/** @brief Return bitmap width in terminal columns for scalar cp. */
uint32_t framebuffer_codepoint_width(uint32_t cp);
/** @brief Draw scalar cp at x/y with foreground fg and background bg. */
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t fg, uint32_t bg);
/**
 * @brief Draw a scalar into an explicit framebuffer without changing the
 *        active framebuffer descriptor.
 * @param target Framebuffer whose pixel storage receives the glyph.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar to draw.
 * @param fg Logical RGB foreground color.
 * @param bg Logical RGB background color.
 */
void framebuffer_codepoint_to(const struct framebuffer *target,
                              uint32_t x, uint32_t y, uint32_t cp,
                              uint32_t fg, uint32_t bg);
/**
 * @brief Copy a w x h pixel buffer (stride words per row) to (x,y).
 */
void framebuffer_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride, const uint32_t *pixels);
/**
 * @brief Flush the back buffer so pending drawing becomes visible.
 */
void framebuffer_present(void);
/**
 * @brief Reclaim fatal scanout without locks after other CPUs have stopped.
 * Restores legacy GFB tracing where supported; never resets the video mode.
 */
void framebuffer_panic_prepare(void);
/** @brief Publish fatal pixels without locks, with a finite FIFO retry bound. */
void framebuffer_panic_present(void);
/**
 * @brief Flush only the (x,y,width,height) sub-region of the back buffer.
 */
void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height);
/**
 * @brief Periodically publish the mmap-backed framebuffer to the display.
 *
 * This is needed by legacy VMware SVGA devices where guest writes do not
 * generate an UPDATE command themselves.
 */
void framebuffer_display_tick(void);
/**
 * @brief Transfer a validated SVGA scanout from console tracing to DRM presentation.
 * @return Zero, including already-owned scanout, or legacy internal -1 for unsupported framebuffer state.
 * Execution-owned task context; takes the SVGA lock internally and disables traces only on success. No heap allocation or provider self-reference.
 */
int framebuffer_drm_scanout_begin(void);
/**
 * @brief Return the permanent SVGA scanout to ordinary framebuffer tracing.
 * Execution-owned task context after dependent jobs drain. Clears pending DRM damage and restores tracing under the SVGA lock; does not unbind the boot console or free provider-owned backing.
 */
void framebuffer_drm_scanout_release(void);
/**
 * @brief Copy validated XRGB8888 damage into the retained DRM-owned scanout.
 * @param pixels Readable kernel source covering every requested row.
 * @param pitch Source byte stride, at least the display width times four.
 * @param format Supported XRGB8888 format code.
 * @param x1 Inclusive left pixel.
 * @param y1 Inclusive top pixel.
 * @param x2 Exclusive right pixel, within display width.
 * @param y2 Exclusive bottom pixel, within display height.
 * @return Zero after copying/queuing damage, or legacy internal -1 for invalid or unavailable scanout.
 * Execution-owned task context; source remains borrowed only during the copy. Coalesces FIFO pressure into permanent damage state under the SVGA lock; no module/data pointer is queued.
 */
int framebuffer_drm_present_region(const void *pixels, uint32_t pitch,
                                   uint32_t format, uint32_t x1, uint32_t y1,
                                   uint32_t x2, uint32_t y2);
/**
 * @brief Read the color of the pixel at (x,y).
 */
uint32_t framebuffer_get_pixel_public(uint32_t x, uint32_t y);
/**
 * @brief Set the pixel at (x,y) to color.
 */
void framebuffer_put_pixel_public(uint32_t x, uint32_t y, uint32_t color);
#endif
