#include "internal.h"

/**
 * @brief Allocate a same-layout RAM surface for one fullscreen text replay.
 * @param target Receives a copy of the active framebuffer descriptor.
 * @param pixels Receives the allocated backing storage on success.
 * @return True when a valid surface was allocated.
 */
static bool fb_console_allocate_replay_target(struct framebuffer *target,
                                              uint8_t **pixels);

/**
 * @brief Save the graphical framebuffer before a text VT paints over it.
 */
static void console_save_graphic_frame(void);

/**
 * @brief Restore the saved graphical framebuffer after returning from text.
 * @return True when a saved frame was restored.
 */
static bool console_restore_graphic_frame(uint32_t number);

static uint8_t *console_graphic_saved;
static size_t console_graphic_saved_bytes;
static uint32_t console_graphic_saved_vt;

struct console_vt_surface {
    struct framebuffer frame;
    struct fb_console_state state;
    bool initialized;
};

static struct console_vt_surface console_vt_surfaces[CONSOLE_VT_COUNT];
uint32_t fb_console_render_vt;

/**
 * @brief Retain a lazily allocated text surface matching the current display.
 * @param number Valid one-based virtual terminal number.
 * @return Its surface, or NULL when allocation or display format is unavailable.
 */
static struct console_vt_surface *fb_console_vt_surface(uint32_t number)
{
    struct console_vt_surface *surface = &console_vt_surfaces[number - 1u];
    const struct framebuffer *active = framebuffer_get();
    if (!active->available || !active->pixels) return NULL;
    if (surface->frame.pixels && surface->frame.width == active->width &&
        surface->frame.height == active->height && surface->frame.pitch == active->pitch &&
        surface->frame.bpp == active->bpp &&
        surface->frame.red_field_position == active->red_field_position &&
        surface->frame.red_mask_size == active->red_mask_size &&
        surface->frame.green_field_position == active->green_field_position &&
        surface->frame.green_mask_size == active->green_mask_size &&
        surface->frame.blue_field_position == active->blue_field_position &&
        surface->frame.blue_mask_size == active->blue_mask_size)
        return surface;
    if (surface->frame.pixels) kernel_free(surface->frame.pixels);
    *surface = (struct console_vt_surface){0};
    uint8_t *pixels;
    if (!fb_console_allocate_replay_target(&surface->frame, &pixels)) return NULL;
    return surface;
}

/**
 * @brief Save the visible text pixels and parser before another VT takes over.
 * The caller has removed the software cursor and serializes console access.
 */
static void fb_console_save_active_text(void)
{
    if (!console_active_vt || console_vt_graphical || !fb_console_enabled) return;
    struct console_vt_surface *surface = fb_console_vt_surface(console_active_vt);
    if (!surface) return;
    const struct framebuffer *active = framebuffer_get();
    __builtin_memcpy(surface->frame.pixels, active->pixels,
        (size_t)active->pitch * active->height);
    fb_console_state_save(&surface->state);
    surface->initialized = true;
}

/**
 * @brief Apply background output to its own retained pixels and streaming parser.
 * @param number Valid one-based destination VT number, currently off screen.
 * @param bytes Output bytes already accepted by the terminal.
 * @param length Number of bytes. Caller serializes console access.
 */
static void fb_console_write_background(uint32_t number, const char *bytes, size_t length)
{
    struct console_vt_surface *surface = fb_console_vt_surface(number);
    if (!surface) return;
    struct fb_console_state previous;
    fb_console_state_save(&previous);
    const struct framebuffer *previous_target = fb_console_render_target;
    uint32_t previous_vt = fb_console_render_vt;
    bool previous_enabled = fb_console_enabled;
    fb_console_render_target = &surface->frame;
    fb_console_render_vt = number;
    fb_console_enabled = true;
    if (surface->initialized) fb_console_state_restore(&surface->state);
    else {
        console_utf8 = (struct reliefos_utf8_stream){0};
        fb_console_initialize_fullscreen();
        struct console_vt_text *history = &vt_text[number - 1u];
        for (size_t i = 0; i < history->length; ++i) fb_console_ansi_feed(history->bytes[i]);
    }
    for (size_t i = 0; i < length; ++i) fb_console_ansi_feed(bytes[i]);
    fb_console_state_save(&surface->state);
    surface->initialized = true;
    fb_console_render_target = previous_target;
    fb_console_render_vt = previous_vt;
    fb_console_enabled = previous_enabled;
    fb_console_state_restore(&previous);
}

/**
 * @brief Allocate a same-layout RAM surface for one fullscreen text replay.
 * @param target Receives a copy of the active framebuffer descriptor.
 * @param pixels Receives the allocated backing storage on success.
 * @return True when a valid surface was allocated.
 */
static bool fb_console_allocate_replay_target(struct framebuffer *target,
                                              uint8_t **pixels)
{
    const struct framebuffer *active = framebuffer_get();
    uint64_t visible_row_bytes;
    uint64_t allocation_bytes;
    uint8_t *storage;

    if (!target || !pixels) {
        return false;
    }
    *pixels = NULL;
    if (!active->available || !active->pixels ||
        !active->width || !active->height ||
        (active->bytes_per_pixel != 3u && active->bytes_per_pixel != 4u)) {
        return false;
    }
    visible_row_bytes = (uint64_t)active->width * active->bytes_per_pixel;
    if (active->pitch < visible_row_bytes) {
        return false;
    }
    allocation_bytes = (uint64_t)active->pitch * active->height;
    if (!allocation_bytes || allocation_bytes > (uint64_t)(size_t)-1) {
        return false;
    }
    storage = kernel_malloc((size_t)allocation_bytes);
    if (!storage) {
        return false;
    }
    *target = *active;
    target->pixels = (uint32_t *)(void *)storage;
    *pixels = storage;
    return true;
}

/**
 * @brief Save the graphical framebuffer before a text VT paints over it.
 */
static void console_save_graphic_frame(void)
{
    const struct framebuffer *fb = framebuffer_get();
    uint64_t bytes64;
    size_t bytes;

    if (!fb->available || !fb->pixels || !fb->pitch || !fb->height) return;
    bytes64 = (uint64_t)fb->pitch * fb->height;
    if (bytes64 > (uint64_t)(size_t)-1) return;
    bytes = (size_t)bytes64;
    if (console_graphic_saved_bytes != bytes) {
        if (console_graphic_saved) kernel_free(console_graphic_saved);
        console_graphic_saved = kernel_malloc(bytes);
        console_graphic_saved_bytes = console_graphic_saved ? bytes : 0;
        console_graphic_saved_vt = 0;
    }
    if (console_graphic_saved) {
        __builtin_memcpy(console_graphic_saved, fb->pixels, bytes);
        console_graphic_saved_vt = console_active_vt;
    }
}

/**
 * @brief Restore the saved graphical framebuffer after returning from text.
 * @return True when a saved frame was restored.
 */
static bool console_restore_graphic_frame(uint32_t number)
{
    const struct framebuffer *fb = framebuffer_get();
    uint64_t bytes64;

    if (!fb->available || !fb->pixels || !fb->pitch || !fb->height) return false;
    bytes64 = (uint64_t)fb->pitch * fb->height;
    if (!console_graphic_saved || console_graphic_saved_vt != number ||
        bytes64 != console_graphic_saved_bytes) return false;
    __builtin_memcpy(fb->pixels, console_graphic_saved, console_graphic_saved_bytes);
    framebuffer_present();
    return true;
}

/**
 * @brief Append terminal output to its own VT history and paint only the active text VT.
 * @param number Virtual terminal from one through six.
 * @param s Bytes to append; need not be NUL terminated.
 * @param len Number of bytes to append.
 */
void console_vt_write(uint32_t number, const char *s, size_t len)
{
    if (!s || number < 1u || number > CONSOLE_VT_COUNT) return;
    if (console_vga_text_mode) { vga_console_write(number,s,len); return; }
    if (number == console_active_vt && !console_vt_graphical)
        fb_console_hide_tty_cursor();
    else fb_console_write_background(number, s, len);
    struct console_vt_text *screen = &vt_text[number - 1u];
    for (size_t i = 0; i < len; ++i) {
        if (screen->length == CONSOLE_VT_TEXT_CAP) {
            size_t drop = 4096u;
            while (drop < screen->length && screen->bytes[drop - 1u] != '\n') ++drop;
            for (size_t j = 0; j < screen->length - drop; ++j)
                screen->bytes[j] = screen->bytes[j + drop];
            screen->length -= drop;
        }
        screen->bytes[screen->length++] = s[i];
        if (number == console_active_vt && !console_vt_graphical)
            fb_console_ansi_feed(s[i]);
    }
    if (number == console_active_vt && !console_vt_graphical) console_present();
}

/**
 * @brief Select the framebuffer owner and restore a VT's text state when visible.
 * @param number Virtual terminal from one through six.
 * @param graphical True if the VT's graphical session owns the display.
 */
void console_vt_activate(uint32_t number, bool graphical)
{
    bool restored = false;
    if (number < 1u || number > CONSOLE_VT_COUNT) return;
    if (console_vga_text_mode) {
        console_active_vt=number;
        console_vt_graphical=graphical;
        console_service_logs_only=true;
        if (graphical) {
            if (!console_restore_graphic_frame(number)) {
                framebuffer_clear(0);
                framebuffer_present();
            }
        } else vga_console_activate();
        return;
    }
    bool preserve_text = !graphical && !console_vt_graphical &&
        (console_active_vt == number || (!console_active_vt && number == 1u));
    if (!console_active_vt) {
        struct console_vt_text *boot_screen = &vt_text[0];
        boot_screen->length = 0;
        for (size_t index = console_log_replay_start();
             index < log_len && boot_screen->length < CONSOLE_VT_TEXT_CAP; ++index)
            boot_screen->bytes[boot_screen->length++] =
                log_buffer[(log_start + index) % CONSOLE_LOG_CAP];
    }
    if (console_vt_graphical && !graphical) console_save_graphic_frame();
    fb_console_hide_tty_cursor();
    if (!preserve_text) fb_console_save_active_text();
    console_active_vt = number;
    console_vt_graphical = graphical;
    console_service_logs_only = true;
    fb_console_tty_cursor_active = !graphical;
    if (!framebuffer_get()->available) return;
    if (graphical) {
        restored = console_restore_graphic_frame(number);
        if (!restored) {
            framebuffer_clear(0u);
            framebuffer_present();
        }
        return;
    }
    if (!fb_console_enabled) return;
    if (preserve_text) {
        console_present();
        return;
    }
    struct console_vt_surface *surface = fb_console_vt_surface(number);
    if (surface) fb_console_render_target = &surface->frame;
    if (surface && surface->initialized) fb_console_state_restore(&surface->state);
    else {
        console_utf8 = (struct reliefos_utf8_stream){0};
        fb_console_initialize_fullscreen();
        struct console_vt_text *screen = &vt_text[number - 1u];
        for (size_t i = 0; i < screen->length; ++i) fb_console_ansi_feed(screen->bytes[i]);
    }
    if (surface) {
        fb_console_state_save(&surface->state);
        surface->initialized = true;
        const struct framebuffer *active = framebuffer_get();
        size_t visible_row_bytes =
            (size_t)active->width * active->bytes_per_pixel;
        for (uint32_t row = 0; row < active->height; ++row) {
            uint8_t *destination = (uint8_t *)active->pixels +
                                   (uint64_t)row * active->pitch;
            const uint8_t *source = (const uint8_t *)surface->frame.pixels +
                                    (uint64_t)row * surface->frame.pitch;
            __builtin_memcpy(destination, source, visible_row_bytes);
        }
        fb_console_render_target = NULL;
    }
    console_present();
    framebuffer_present();
}

/** @brief Leave a failed VGA transition uncommitted by the VT ownership layer. */
int console_prepare_vt(bool graphical)
{
    if (!console_vga_text_mode) return 0;
    if (console_vt_graphical && !graphical) console_save_graphic_frame();
    return vga_console_switch(graphical);
}
