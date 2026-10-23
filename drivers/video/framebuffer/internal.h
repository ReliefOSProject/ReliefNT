#ifndef RELIEFNT_DRIVERS_VIDEO_FRAMEBUFFER_FRAMEBUFFER_C_INTERNAL_H
#define RELIEFNT_DRIVERS_VIDEO_FRAMEBUFFER_FRAMEBUFFER_C_INTERNAL_H

#include <reliefos/psf_font.h>
#include <reliefnt/console.h>
#include <reliefnt/drm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/pci.h>
#include <reliefnt/port.h>
#include "framebuffer_fifo.h"
#include "../svga/device.h"














#define FRAMEBUFFER_MODE_MAX_WIDTH 4096u
#define FRAMEBUFFER_MODE_MAX_HEIGHT 4096u
#define FRAMEBUFFER_MAX_VRAM_BYTES (512u * 1024u * 1024u)
#define FRAMEBUFFER_PHYS_LIMIT 0x100000000ULL

#define VBE_DISPI_IOPORT_INDEX 0x01ceu
#define VBE_DISPI_IOPORT_DATA 0x01cfu
#define VBE_DISPI_INDEX_ID 0u
#define VBE_DISPI_INDEX_XRES 1u
#define VBE_DISPI_INDEX_YRES 2u
#define VBE_DISPI_INDEX_BPP 3u
#define VBE_DISPI_INDEX_ENABLE 4u
#define VBE_DISPI_INDEX_VIRT_WIDTH 6u
#define VBE_DISPI_INDEX_VIDEO_MEMORY_64K 10u
#define VBE_DISPI_ID0 0xb0c0u
#define VBE_DISPI_ID5 0xb0c5u
#define VBE_DISPI_ENABLED 0x01u
#define VBE_DISPI_LFB_ENABLED 0x40u

#define VMWARE_VENDOR_ID 0x15adu
#define VMWARE_SVGA_DEVICE_ID 0x0405u
#define VMWARE_PCI_COMMAND_IO 0x0001u
#define VMWARE_PCI_COMMAND_MEMORY 0x0002u
#define VMWARE_SVGA_ID_2 0x90000002u
#define VMWARE_SVGA_REG_ID 0u
#define VMWARE_SVGA_REG_ENABLE 1u
#define VMWARE_SVGA_REG_WIDTH 2u
#define VMWARE_SVGA_REG_HEIGHT 3u
#define VMWARE_SVGA_REG_MAX_WIDTH 4u
#define VMWARE_SVGA_REG_MAX_HEIGHT 5u
#define VMWARE_SVGA_REG_DEPTH 6u
#define VMWARE_SVGA_REG_BITS_PER_PIXEL 7u
#define VMWARE_SVGA_REG_PSEUDOCOLOR 8u
#define VMWARE_SVGA_REG_BYTES_PER_LINE 12u
#define VMWARE_SVGA_REG_FB_START 13u
#define VMWARE_SVGA_REG_FB_OFFSET 14u
#define VMWARE_SVGA_REG_FB_MAX_SIZE 15u
#define VMWARE_SVGA_REG_FB_SIZE 16u
#define VMWARE_SVGA_REG_CAPABILITIES 17u
#define VMWARE_SVGA_REG_MEM_START 18u
#define VMWARE_SVGA_REG_MEM_SIZE 19u
#define VMWARE_SVGA_REG_CONFIG_DONE 20u
#define VMWARE_SVGA_REG_SYNC 21u
#define VMWARE_SVGA_REG_BUSY 22u
#define VMWARE_SVGA_REG_TRACES 45u
#define VMWARE_SVGA_CAP_TRACES 0x00200000u
#define VMWARE_SVGA_SYNC_POLL_LIMIT 4000u

#define VMWARE_SVGA_FIFO_MIN 0u
#define VMWARE_SVGA_FIFO_MAX 1u
#define VMWARE_SVGA_FIFO_NEXT_CMD 2u
#define VMWARE_SVGA_FIFO_STOP 3u
#define VMWARE_SVGA_FIFO_MIN_BYTES (4u * sizeof(uint32_t))
#define VMWARE_SVGA_CMD_UPDATE 1u
#define VMWARE_SVGA_UPDATE_WORDS 5u

struct vmware_svga_state {
    uint16_t io_port;
    uint32_t max_width;
    uint32_t max_height;
    volatile uint32_t *fifo;
    uint32_t fifo_min;
    uint32_t fifo_max;
    bool present;
    bool fifo_present;
    bool fifo_full_logged;
    bool sync_timeout_logged;
};

struct bochs_vbe_state {
    uint32_t lfb_start;
    uint32_t vram_bytes;
    bool present;
};





































struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_system_table {
    struct efi_table_header hdr;
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    void *con_out;
    void *standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
};

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;

typedef efi_status_t (__attribute__((ms_abi)) *efi_handle_protocol_fn)(
    efi_handle_t handle,
    struct efi_guid *protocol,
    void **interface);
typedef efi_status_t (__attribute__((ms_abi)) *efi_locate_handle_buffer_fn)(
    uint64_t search_type,
    struct efi_guid *protocol,
    void *search_key,
    uint64_t *no_handles,
    efi_handle_t **buffer);

struct efi_boot_services {
    struct efi_table_header hdr;
    efi_status_t (*raise_tpl)(uint64_t tpl);
    void (*restore_tpl)(uint64_t tpl);
    char _pad_allocate_pages_through_get_map[24];
    void *allocate_pool;
    efi_status_t (__attribute__((ms_abi)) *free_pool)(void *buffer);
    char _pad_create_event_through_check_event[48];
    efi_status_t (*install_protocol_interface)(void);
    efi_status_t (*reinstall_protocol_interface)(void);
    efi_status_t (*uninstall_protocol_interface)(void);
    efi_handle_protocol_fn handle_protocol;
    void *_reserved;
    efi_status_t (*register_protocol_notify)(void);
    efi_status_t (*locate_handle)(void);
    efi_status_t (*locate_device_path)(void);
    efi_status_t (*install_configuration_table)(void);
    efi_status_t (*load_image)(void);
    efi_status_t (*start_image)(void);
    efi_status_t (*exit)(void);
    efi_status_t (*unload_image)(void);
    efi_status_t (*exit_boot_services)(void);
    efi_status_t (*get_next_monotonic_count)(void);
    efi_status_t (*stall)(void);
    efi_status_t (*set_watchdog_timer)(void);
    efi_status_t (*connect_controller)(void);
    efi_status_t (*disconnect_controller)(void);
    efi_status_t (*open_protocol)(void);
    efi_status_t (*close_protocol)(void);
    efi_status_t (*open_protocol_information)(void);
    efi_status_t (*protocols_per_handle)(void);
    efi_locate_handle_buffer_fn locate_handle_buffer;
};

struct efi_gop_mode_info {
    uint32_t version;
    uint32_t horizontal_resolution;
    uint32_t vertical_resolution;
    uint32_t pixel_format;
    uint32_t pixel_information[4];
    uint32_t pixels_per_scan_line;
};

struct efi_gop_mode {
    uint32_t max_mode;
    uint32_t mode;
    struct efi_gop_mode_info *info;
    uint64_t size_of_info;
    uint64_t framebuffer_base;
    uint64_t framebuffer_size;
};

struct efi_graphics_output_protocol {
    void *query_mode;
    void *set_mode;
    void *blt;
    struct efi_gop_mode *mode;
};

#define EFI_GOP_PIXEL_RED_GREEN_BLUE 0u
#define EFI_GOP_PIXEL_BLUE_GREEN_RED 1u
#define EFI_GOP_PIXEL_BIT_MASK 2u
#define EFI_GOP_PIXEL_BLT_ONLY 3u













































/* Kick the host to process queued FIFO commands without waiting for it to
 * drain.  Writing the SYNC register is the doorbell; reading BUSY in a loop is
 * the synchronous wait.  On the fast present path we only need the doorbell:
 * blocking here would pin a core spinning (and serialise every other core's
 * syscall) for the duration of a frame the host can process asynchronously. */


extern struct framebuffer fb;
extern struct vmware_svga_state vmware_svga;
extern bool framebuffer_drm_scanout_owned;
extern bool framebuffer_drm_damage_pending;

/** @brief Return the font's one- or two-column width for a Unicode scalar. */
uint32_t framebuffer_codepoint_width(uint32_t cp);

/**
 * @brief Draw a Unicode scalar without allocating memory or accessing storage.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar.
 * @param fg Foreground color.
 * @param bg Background color.
 */
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t cp, uint32_t fg, uint32_t bg);

/**
 * @brief Draw a Unicode scalar into an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the glyph.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param cp Unicode scalar to look up.
 * @param fg Foreground color.
 * @param bg Background color.
 */
void framebuffer_codepoint_to(const struct framebuffer *target,
                              uint32_t x, uint32_t y, uint32_t cp,
                              uint32_t fg, uint32_t bg);

bool framebuffer_drm_owned(void);

int framebuffer_color_format_valid(const struct framebuffer *target);

/**
 * @brief Convert logical RGB to the native channel layout of a framebuffer.
 * @param target Framebuffer format that determines the native channel layout.
 * @param color Logical RGB color to convert.
 * @return Native-format pixel value.
 */
uint32_t framebuffer_native_color_for(const struct framebuffer *target,
                                             uint32_t color);

uint32_t framebuffer_native_color(uint32_t color);

uint32_t framebuffer_logical_color(uint32_t pixel);

int framebuffer_gop_mask(uint32_t mask, uint8_t *position, uint8_t *size);

void framebuffer_set_default_format(void);

uint8_t framebuffer_bytes_per_pixel(uint32_t pitch, uint32_t width,
                                           uint8_t bpp);

int framebuffer_range_valid(uint64_t start, uint64_t bytes);

uint64_t framebuffer_current_bytes(void);

uint32_t vmware_svga_read(uint32_t reg);

void vmware_svga_write(uint32_t reg, uint32_t value);

void vmware_svga_memory_fence(void);

/**
 * @brief Enable VMware GFB write tracing for mmap-backed fbdev clients.
 * @return None.
 *
 * Xorg's fbdev shadow framebuffer writes the scan-out surface through its
 * mmap.  Once the SVGA FIFO is enabled those writes do not imply an UPDATE
 * unless SVGA_REG_TRACES is explicitly enabled.  Keep the capability check
 * here so mode/FIFO reinitialisation restores the same display semantics.
 */
void framebuffer_vmware_enable_traces(void);

void framebuffer_vmware_fifo_init(bool report, bool device_lock_held);

int framebuffer_vmware_fifo_update(uint32_t x, uint32_t y,
                                          uint32_t width, uint32_t height);

void framebuffer_vmware_kick(void);

/**
 * @brief Reclaim the legacy framebuffer after stopping other panic CPUs.
 * Bypasses the device lock, which the interrupted CPU may already own.
 */
void framebuffer_panic_prepare(void);

/**
 * @brief Publish panic pixels without logging, locks or unbounded FIFO waits.
 * The panic owner is the only remaining producer. A wedged GPU is best effort.
 */
void framebuffer_panic_present(void);

void framebuffer_vmware_sync(bool report);

int framebuffer_vmware_probe(void);

int framebuffer_vbe_probe(void);

int framebuffer_vbe_set_mode(uint32_t width, uint32_t height);

int framebuffer_vmware_set_mode(uint32_t width, uint32_t height);

uint8_t *framebuffer_pixel_ptr(uint32_t x, uint32_t y);

void framebuffer_init(const struct boot_info *boot);

const struct framebuffer *framebuffer_get(void);

int framebuffer_set_mode(uint32_t width, uint32_t height);

void framebuffer_clear(uint32_t color);

void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/**
 * @brief Fill an axis-aligned rectangle in an explicit framebuffer target.
 * @param target Framebuffer whose pixel storage receives the fill.
 * @param x Left pixel coordinate.
 * @param y Top pixel coordinate.
 * @param w Rectangle width in pixels.
 * @param h Rectangle height in pixels.
 * @param color Logical RGB color to fill.
 */
void framebuffer_rect_to(const struct framebuffer *target,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t color);

void framebuffer_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t stride, const uint32_t *pixels);

uint32_t framebuffer_get_pixel(uint32_t x, uint32_t y);

void framebuffer_put_pixel(uint32_t x, uint32_t y, uint32_t color);

uint32_t framebuffer_get_pixel_public(uint32_t x, uint32_t y);

void framebuffer_put_pixel_public(uint32_t x, uint32_t y, uint32_t color);

void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height);

void framebuffer_present(void);

void framebuffer_display_tick(void);

int framebuffer_drm_scanout_begin(void);

void framebuffer_drm_scanout_release(void);

int framebuffer_drm_present_region(const void *pixels, uint32_t pitch,
                                   uint32_t format, uint32_t x1, uint32_t y1,
                                   uint32_t x2, uint32_t y2);

void framebuffer_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);

void framebuffer_text_transparent(uint32_t x, uint32_t y, const char *text, uint32_t fg);

#endif
