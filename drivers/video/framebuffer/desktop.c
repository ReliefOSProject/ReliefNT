#include "internal.h"

static void beveled_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill, int pressed);

static uint32_t text_fit_chars(uint32_t pixel_width);

static void draw_window_button(uint32_t x, uint32_t y, char label, int pressed);

static void draw_window(uint8_t id);

static void draw_taskbar_button(uint8_t id, uint32_t x, uint32_t y);

static void draw_start_menu(void);


uint32_t taskbar_y(void)
{
    return fb.height > DESKTOP_TASKBAR_H ? fb.height - DESKTOP_TASKBAR_H : 0;
}

static void beveled_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t fill, int pressed)
{
    if (1) {
        uint32_t mapped_fill = fill == 0x00c0c0c0u ? 0x00f3f3f3u : fill;
        uint32_t border = pressed ? 0x000078d4u : 0x00d0d0d0u;
        framebuffer_rect(x, y, w, h, mapped_fill);
        framebuffer_rect(x, y, w, 1, border);
        framebuffer_rect(x, y, 1, h, border);
        framebuffer_rect(x + w - 1, y, 1, h, border);
        framebuffer_rect(x, y + h - 1, w, 1, border);
        return;
    }
    const uint32_t white = 0x00ffffff;
    const uint32_t dark = 0x00808080;
    const uint32_t black = 0x00000000;
    uint32_t tl = pressed ? dark : white;
    uint32_t br = pressed ? white : black;

    framebuffer_rect(x, y, w, h, fill);
    framebuffer_rect(x, y, w, 1, tl);
    framebuffer_rect(x, y, 1, h, tl);
    framebuffer_rect(x + w - 1, y, 1, h, br);
    framebuffer_rect(x, y + h - 1, w, 1, br);
    if (w > 3 && h > 3) {
        framebuffer_rect(x + 1, y + 1, w - 2, 1, pressed ? black : 0x00dfdfdf);
        framebuffer_rect(x + 1, y + 1, 1, h - 2, pressed ? black : 0x00dfdfdf);
        framebuffer_rect(x + w - 2, y + 1, 1, h - 2, pressed ? 0x00dfdfdf : dark);
        framebuffer_rect(x + 1, y + h - 2, w - 2, 1, pressed ? 0x00dfdfdf : dark);
    }
}

static uint32_t text_fit_chars(uint32_t pixel_width)
{
    return pixel_width / RELIEFOS_FONT_W;
}

static void draw_window_button(uint32_t x, uint32_t y, char label, int pressed)
{
    beveled_rect(x, y, 18, 20, 0x00c0c0c0, pressed);
    char text[2] = {label, 0};
    framebuffer_text_transparent(x + 5 + (pressed ? 1 : 0), y + 2 + (pressed ? 1 : 0), text, 0x00000000);
}

static void draw_window(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS) {
        return;
    }
    struct desktop_window *w = &windows[id];
    if (!w->visible || w->minimized) {
        return;
    }

    const uint32_t gray = 0x00c0c0c0;
    const uint32_t white = 0x00ffffff;
    const uint32_t black = 0x00000000;
    const uint32_t dark = 0x00808080;
    const uint32_t active = 0x00000080;
    const uint32_t inactive = 0x00808080;
    uint32_t title = (active_window == id) ? active : inactive;

    beveled_rect((uint32_t)w->x, (uint32_t)w->y, w->width, w->height, gray, 0);
    framebuffer_rect((uint32_t)w->x + 4, (uint32_t)w->y + 4, w->width - 8, DESKTOP_TITLEBAR_H, title);

    uint32_t title_space = w->width > 76 ? w->width - 76 : 0;
    uint32_t title_chars = text_fit_chars(title_space);
    char title_buf[48];
    uint32_t pos = 0;
    while (w->title && w->title[pos] && pos + 1 < sizeof(title_buf) && pos < title_chars) {
        title_buf[pos] = w->title[pos];
        ++pos;
    }
    title_buf[pos] = 0;
    framebuffer_text((uint32_t)w->x + 10, (uint32_t)w->y + 9, title_buf, white, title);

    uint32_t bx = (uint32_t)w->x + w->width - 64;
    uint32_t by = (uint32_t)w->y + 6;
    draw_window_button(bx, by, '_', 0);
    draw_window_button(bx + 20, by, w->maximized ? 'r' : 'M', 0);
    draw_window_button(bx + 40, by, 'X', 0);

    uint32_t body_x = (uint32_t)w->x + 8;
    uint32_t body_y = (uint32_t)w->y + DESKTOP_TITLEBAR_H + 10;
    uint32_t body_w = w->width > 16 ? w->width - 16 : 0;
    uint32_t body_h = w->height > DESKTOP_TITLEBAR_H + 18 ? w->height - DESKTOP_TITLEBAR_H - 18 : 0;
    framebuffer_rect(body_x, body_y, body_w, body_h, w->body_color);
    framebuffer_rect(body_x, body_y, body_w, 1, dark);
    framebuffer_rect(body_x, body_y, 1, body_h, dark);
    framebuffer_rect(body_x + body_w - 1, body_y, 1, body_h, white);
    framebuffer_rect(body_x, body_y + body_h - 1, body_w, 1, white);

    if (id == 0) {
        framebuffer_text(body_x + 16, body_y + 18, "desktop.elf window server", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 42, "Drag title bar. Use buttons.", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 66, "Start opens closed windows.", black, w->body_color);
    } else if (id == 1) {
        framebuffer_text(body_x + 16, body_y + 18, "/", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 42, "boot  system  userland", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 66, "System root view", black, w->body_color);
    } else {
        framebuffer_text(body_x + 16, body_y + 18, "Settings", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 42, "Win98 style controls", black, w->body_color);
        framebuffer_text(body_x + 16, body_y + 66, "GUI state lives in ReliefNT", black, w->body_color);
    }

    framebuffer_rect((uint32_t)w->x + w->width - 13, (uint32_t)w->y + w->height - 13, 9, 1, dark);
    framebuffer_rect((uint32_t)w->x + w->width - 9, (uint32_t)w->y + w->height - 17, 1, 9, dark);
    framebuffer_rect((uint32_t)w->x + w->width - 10, (uint32_t)w->y + w->height - 10, 6, 1, black);
    framebuffer_rect((uint32_t)w->x + w->width - 6, (uint32_t)w->y + w->height - 14, 1, 6, black);
}

static void draw_taskbar_button(uint8_t id, uint32_t x, uint32_t y)
{
    if (id >= DESKTOP_MAX_WINDOWS || !windows[id].visible) {
        return;
    }
    int active = active_window == id && !windows[id].minimized;
    beveled_rect(x, y, 150, 24, 0x00c0c0c0, active);
    framebuffer_text_transparent(x + 8 + (active ? 1 : 0), y + 4 + (active ? 1 : 0), windows[id].title, 0x00000000);
}

static void draw_start_menu(void)
{
    if (!start_menu_open) {
        return;
    }
    uint32_t y = taskbar_y();
    uint32_t menu_y = y > 126 ? y - 126 : 0;
    beveled_rect(6, menu_y, 210, 126, 0x00c0c0c0, 0);
    framebuffer_rect(10, menu_y + 4, 26, 118, 0x00000080);
    framebuffer_text(44, menu_y + 16, "Desktop Server", 0x00000000, 0x00c0c0c0);
    framebuffer_text(44, menu_y + 42, "File Manager", 0x00000000, 0x00c0c0c0);
    framebuffer_text(44, menu_y + 68, "Settings", 0x00000000, 0x00c0c0c0);
    framebuffer_rect(40, menu_y + 92, 166, 1, 0x00808080);
    framebuffer_text(44, menu_y + 100, "Close Menu", 0x00000000, 0x00c0c0c0);
}

void desktop_redraw(void)
{
    if (!fb.available || !desktop_ready) {
        return;
    }

    const uint32_t metro = 1u;
    const uint32_t teal = metro ? 0x000078d4u : 0x00008080u;
    const uint32_t gray = metro ? 0x00f3f3f3u : 0x00c0c0c0u;
    const uint32_t dark = metro ? 0x006b6b6bu : 0x00808080u;
    const uint32_t white = 0x00ffffff;
    const uint32_t black = 0x00000000;

    framebuffer_clear(teal);
    framebuffer_rect(24, 32, 48, 38, gray);
    framebuffer_rect(24, 32, 48, 2, white);
    framebuffer_rect(24, 32, 2, 38, white);
    framebuffer_rect(70, 32, 2, 38, black);
    framebuffer_rect(24, 68, 48, 2, black);
    framebuffer_text(16, 78, "/", white, teal);

    framebuffer_rect(24, 112, 48, 38, gray);
    framebuffer_rect(24, 112, 48, 2, white);
    framebuffer_rect(24, 112, 2, 38, white);
    framebuffer_rect(70, 112, 2, 38, black);
    framebuffer_rect(24, 148, 48, 2, black);
    framebuffer_text(8, 158, "Apps", white, teal);

    for (uint8_t i = 0; i < DESKTOP_MAX_WINDOWS; ++i) {
        draw_window(z_order[i]);
    }

    uint32_t tb_y = taskbar_y();
    framebuffer_rect(0, tb_y, fb.width, DESKTOP_TASKBAR_H, gray);
    framebuffer_rect(0, tb_y, fb.width, 2, white);
    framebuffer_rect(0, tb_y + 2, fb.width, 1, dark);
    beveled_rect(6, tb_y + 5, 86, 24, gray, start_menu_open);
    framebuffer_text_transparent(24 + (start_menu_open ? 1 : 0), tb_y + 9 + (start_menu_open ? 1 : 0), "Start", black);

    uint32_t x = 106;
    for (uint8_t i = 0; i < DESKTOP_MAX_WINDOWS; ++i) {
        draw_taskbar_button(i, x, tb_y + 5);
        x += 158;
    }

    draw_start_menu();
    cursor_visible = false;
}

void desktop_boot_paint(void)
{
    if (!fb.available) {
        return;
    }

    windows[0] = (struct desktop_window){
        .x = 120,
        .y = 84,
        .width = 420,
        .height = 220,
        .restore_x = 120,
        .restore_y = 84,
        .restore_width = 420,
        .restore_height = 220,
        .title = "Desktop Server",
        .body_color = 0x00c0c0c0,
        .visible = true,
    };
    windows[1] = (struct desktop_window){
        .x = 190,
        .y = 150,
        .width = 360,
        .height = 190,
        .restore_x = 190,
        .restore_y = 150,
        .restore_width = 360,
        .restore_height = 190,
        .title = "File Manager",
        .body_color = 0x00ffffff,
        .visible = true,
        .minimized = true,
    };
    windows[2] = (struct desktop_window){
        .x = 270,
        .y = 210,
        .width = 320,
        .height = 170,
        .restore_x = 270,
        .restore_y = 210,
        .restore_width = 320,
        .restore_height = 170,
        .title = "Settings",
        .body_color = 0x00dfdfdf,
        .visible = true,
        .minimized = true,
    };
    z_order[0] = 1;
    z_order[1] = 2;
    z_order[2] = 0;
    desktop_ready = true;
    start_menu_open = false;
    active_window = 0;
    drag_window = -1;
    drag_mode = DESKTOP_DRAG_NONE;
    previous_mouse_buttons = 0;
    desktop_redraw();
}
