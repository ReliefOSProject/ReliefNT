#ifndef RELIEFOS_UAPI_LINUX_FB_H
#define RELIEFOS_UAPI_LINUX_FB_H

#include <stddef.h>
#include <stdint.h>

struct fb_bitfield {
    uint32_t offset;
    uint32_t length;
    uint32_t msb_right;
};

struct fb_var_screeninfo {
    uint32_t xres;
    uint32_t yres;
    uint32_t xres_virtual;
    uint32_t yres_virtual;
    uint32_t xoffset;
    uint32_t yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;
    uint32_t nonstd;
    uint32_t activate;
    uint32_t height;
    uint32_t width;
    uint32_t accel_flags;
    uint32_t pixclock;
    uint32_t left_margin;
    uint32_t right_margin;
    uint32_t upper_margin;
    uint32_t lower_margin;
    uint32_t hsync_len;
    uint32_t vsync_len;
    uint32_t sync;
    uint32_t vmode;
    uint32_t rotate;
    uint32_t colorspace;
    uint32_t reserved[4];
};

struct fb_fix_screeninfo {
    char id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep;
    uint16_t ypanstep;
    uint16_t ywrapstep;
    uint32_t line_length;
    uint64_t mmio_start;
    uint32_t mmio_len;
    uint32_t accel;
    uint16_t capabilities;
    uint16_t reserved[2];
};

/* Linux fbdev colour-map ABI.  The four pointers are user addresses and
 * therefore remain 64-bit in the x86-64 ioctl wire layout. */
struct fb_cmap {
    uint32_t start;
    uint32_t len;
    uint64_t red;
    uint64_t green;
    uint64_t blue;
    uint64_t transp;
};

/* Keep the x86-64 userspace wire layout in lockstep with Linux.  Xorg's
 * fbdev helper reads line_length, visual, and mmio fields at these offsets;
 * a compact private struct silently shifts those fields and makes probe fail.
 */
_Static_assert(sizeof(struct fb_var_screeninfo) == 160,
               "Linux fb_var_screeninfo layout changed");
_Static_assert(sizeof(struct fb_fix_screeninfo) == 80,
               "Linux fb_fix_screeninfo layout changed");
_Static_assert(offsetof(struct fb_fix_screeninfo, line_length) == 48,
               "Linux fb_fix_screeninfo line_length offset changed");
_Static_assert(sizeof(struct fb_cmap) == 40,
               "Linux fb_cmap layout changed");

#define FBIOGET_VSCREENINFO 0x4600UL
#define FBIOPUT_VSCREENINFO 0x4601UL
#define FBIOGET_FSCREENINFO 0x4602UL
#define FBIOGETCMAP 0x4604UL
#define FBIOPUTCMAP 0x4605UL
/* Linux fbdev pan/flush request.  On VMware SVGA the visible surface is
 * refreshed from VRAM only when the host receives an update command, so
 * mmap writers need this to push frames to the display. */
#define FBIOPAN_DISPLAY 0x4606UL
#define FBIOBLANK 0x4611UL

#define FB_BLANK_UNBLANK 0
#define FB_BLANK_NORMAL 1
#define FB_BLANK_VSYNC_SUSPEND 2
#define FB_BLANK_HSYNC_SUSPEND 3
#define FB_BLANK_POWERDOWN 4

#define FB_TYPE_PACKED_PIXELS 0U
#define FB_VISUAL_TRUECOLOR 2U
#define FB_VISUAL_PSEUDOCOLOR 3U

#endif
