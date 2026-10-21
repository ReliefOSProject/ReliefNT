#include "internal.h"

static int hit_rect(uint32_t x, uint32_t y, int32_t rx, int32_t ry, uint32_t rw, uint32_t rh);

static void clamp_window(struct desktop_window *w);

static void bring_to_front(uint8_t id);

static int32_t hit_window(uint32_t x, uint32_t y);

static void minimize_window(uint8_t id);

static void restore_window(uint8_t id);

static void toggle_maximize(uint8_t id);

static void close_window(uint8_t id);

static void handle_start_click(uint32_t x, uint32_t y);

static int handle_taskbar_click(uint32_t x, uint32_t y);

static void begin_window_drag(uint8_t id, uint32_t x, uint32_t y);

static void begin_window_resize(uint8_t id, uint32_t x, uint32_t y);

static uint32_t cursor_bg[CURSOR_W * CURSOR_H];
static uint32_t cursor_x;
static uint32_t cursor_y;
static const char cursor_art[CURSOR_H][CURSOR_W + 1] = {
    "X...............",
    "XO..............",
    "XOX.............",
    "XOOX............",
    "XOOOX...........",
    "XOOOOX..........",
    "XOOOOOX.........",
    "XOOOOOOX........",
    "XOOOOOOOX.......",
    "XOOOOOOOOX......",
    "XOOOOOOOOOX.....",
    "XOOOOX..........",
    "XOOOOX..........",
    "XOOOOX..........",
    "XOOOXX..........",
    "XXXXX...........",
};
static int32_t drag_dx;
static int32_t drag_dy;
static int32_t drag_origin_x;
static int32_t drag_origin_y;
static uint32_t drag_origin_w;
static uint32_t drag_origin_h;
static int hit_rect(uint32_t x, uint32_t y, int32_t rx, int32_t ry, uint32_t rw, uint32_t rh)
{
    return (int32_t)x >= rx && (int32_t)y >= ry &&
           (int32_t)x < rx + (int32_t)rw && (int32_t)y < ry + (int32_t)rh;
}

static void clamp_window(struct desktop_window *w)
{
    if (!w || !fb.available) {
        return;
    }
    if (w->width < DESKTOP_MIN_W) {
        w->width = DESKTOP_MIN_W;
    }
    if (w->height < DESKTOP_MIN_H) {
        w->height = DESKTOP_MIN_H;
    }
    if (w->width > fb.width) {
        w->width = fb.width;
    }
    uint32_t max_h = taskbar_y() > 8 ? taskbar_y() - 8 : fb.height;
    if (w->height > max_h) {
        w->height = max_h;
    }
    int32_t max_x = (int32_t)fb.width - (int32_t)w->width;
    int32_t max_y = (int32_t)taskbar_y() - (int32_t)w->height;
    if (max_x < 0) {
        max_x = 0;
    }
    if (max_y < 0) {
        max_y = 0;
    }
    if (w->x < 0) {
        w->x = 0;
    }
    if (w->y < 0) {
        w->y = 0;
    }
    if (w->x > max_x) {
        w->x = max_x;
    }
    if (w->y > max_y) {
        w->y = max_y;
    }
}

static void bring_to_front(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS) {
        return;
    }
    uint8_t pos = DESKTOP_MAX_WINDOWS;
    for (uint8_t i = 0; i < DESKTOP_MAX_WINDOWS; ++i) {
        if (z_order[i] == id) {
            pos = i;
            break;
        }
    }
    if (pos == DESKTOP_MAX_WINDOWS) {
        return;
    }
    for (uint8_t i = pos; i + 1 < DESKTOP_MAX_WINDOWS; ++i) {
        z_order[i] = z_order[i + 1];
    }
    z_order[DESKTOP_MAX_WINDOWS - 1] = id;
    active_window = id;
}

static int32_t hit_window(uint32_t x, uint32_t y)
{
    for (int32_t zi = DESKTOP_MAX_WINDOWS - 1; zi >= 0; --zi) {
        uint8_t id = z_order[zi];
        struct desktop_window *w = &windows[id];
        if (!w->visible || w->minimized) {
            continue;
        }
        if (hit_rect(x, y, w->x, w->y, w->width, w->height)) {
            return id;
        }
    }
    return -1;
}

static void minimize_window(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS) {
        return;
    }
    windows[id].minimized = true;
    if (active_window == id) {
        active_window = -1;
        for (int32_t zi = DESKTOP_MAX_WINDOWS - 1; zi >= 0; --zi) {
            uint8_t next = z_order[zi];
            if (windows[next].visible && !windows[next].minimized) {
                active_window = next;
                break;
            }
        }
    }
}

static void restore_window(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS || !windows[id].visible) {
        return;
    }
    windows[id].minimized = false;
    bring_to_front(id);
    clamp_window(&windows[id]);
}

static void toggle_maximize(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS) {
        return;
    }
    struct desktop_window *w = &windows[id];
    if (w->maximized) {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->width = w->restore_width;
        w->height = w->restore_height;
        w->maximized = false;
        clamp_window(w);
        return;
    }

    w->restore_x = w->x;
    w->restore_y = w->y;
    w->restore_width = w->width;
    w->restore_height = w->height;
    w->x = 0;
    w->y = 0;
    w->width = fb.width;
    w->height = taskbar_y();
    w->maximized = true;
}

static void close_window(uint8_t id)
{
    if (id >= DESKTOP_MAX_WINDOWS) {
        return;
    }
    windows[id].visible = false;
    windows[id].minimized = false;
    if (active_window == id) {
        active_window = -1;
    }
}

static void handle_start_click(uint32_t x, uint32_t y)
{
    uint32_t tb_y = taskbar_y();
    if (hit_rect(x, y, 6, (int32_t)tb_y + 5, 86, 24)) {
        start_menu_open = !start_menu_open;
        return;
    }

    if (!start_menu_open) {
        return;
    }

    uint32_t menu_y = tb_y > 126 ? tb_y - 126 : 0;
    if (!hit_rect(x, y, 6, (int32_t)menu_y, 210, 126)) {
        start_menu_open = false;
        return;
    }

    if (hit_rect(x, y, 40, (int32_t)menu_y + 8, 166, 24)) {
        windows[0].visible = true;
        restore_window(0);
    } else if (hit_rect(x, y, 40, (int32_t)menu_y + 34, 166, 24)) {
        windows[1].visible = true;
        restore_window(1);
    } else if (hit_rect(x, y, 40, (int32_t)menu_y + 60, 166, 24)) {
        windows[2].visible = true;
        restore_window(2);
    }
    start_menu_open = false;
}

static int handle_taskbar_click(uint32_t x, uint32_t y)
{
    uint32_t tb_y = taskbar_y();
    if (!hit_rect(x, y, 0, (int32_t)tb_y, fb.width, DESKTOP_TASKBAR_H)) {
        return 0;
    }

    handle_start_click(x, y);
    if (hit_rect(x, y, 6, (int32_t)tb_y + 5, 86, 24)) {
        return 1;
    }

    uint32_t bx = 106;
    for (uint8_t i = 0; i < DESKTOP_MAX_WINDOWS; ++i) {
        if (hit_rect(x, y, (int32_t)bx, (int32_t)tb_y + 5, 150, 24) && windows[i].visible) {
            if (active_window == i && !windows[i].minimized) {
                minimize_window(i);
            } else {
                restore_window(i);
            }
            start_menu_open = false;
            return 1;
        }
        bx += 158;
    }
    start_menu_open = false;
    return 1;
}

static void begin_window_drag(uint8_t id, uint32_t x, uint32_t y)
{
    struct desktop_window *w = &windows[id];
    if (w->maximized) {
        return;
    }
    drag_window = id;
    drag_mode = DESKTOP_DRAG_MOVE;
    drag_dx = (int32_t)x - w->x;
    drag_dy = (int32_t)y - w->y;
}

static void begin_window_resize(uint8_t id, uint32_t x, uint32_t y)
{
    struct desktop_window *w = &windows[id];
    if (w->maximized) {
        return;
    }
    drag_window = id;
    drag_mode = DESKTOP_DRAG_RESIZE;
    drag_origin_x = (int32_t)x;
    drag_origin_y = (int32_t)y;
    drag_origin_w = w->width;
    drag_origin_h = w->height;
}

void desktop_handle_mouse(uint32_t x, uint32_t y, uint8_t buttons)
{
    if (!fb.available || !desktop_ready) {
        previous_mouse_buttons = buttons;
        return;
    }

    uint8_t left = buttons & 1;
    uint8_t was_left = previous_mouse_buttons & 1;
    int changed = 0;

    if (left && drag_window >= 0 && drag_window < DESKTOP_MAX_WINDOWS) {
        struct desktop_window *w = &windows[drag_window];
        if (drag_mode == DESKTOP_DRAG_MOVE) {
            w->x = (int32_t)x - drag_dx;
            w->y = (int32_t)y - drag_dy;
            clamp_window(w);
            changed = 1;
        } else if (drag_mode == DESKTOP_DRAG_RESIZE) {
            int32_t dw = (int32_t)x - drag_origin_x;
            int32_t dh = (int32_t)y - drag_origin_y;
            int32_t new_w = (int32_t)drag_origin_w + dw;
            int32_t new_h = (int32_t)drag_origin_h + dh;
            w->width = new_w < DESKTOP_MIN_W ? DESKTOP_MIN_W : (uint32_t)new_w;
            w->height = new_h < DESKTOP_MIN_H ? DESKTOP_MIN_H : (uint32_t)new_h;
            clamp_window(w);
            changed = 1;
        }
    }

    if (!left && was_left) {
        drag_window = -1;
        drag_mode = DESKTOP_DRAG_NONE;
    }

    if (left && !was_left) {
        if (handle_taskbar_click(x, y)) {
            changed = 1;
        } else {
            int32_t id = hit_window(x, y);
            if (id >= 0) {
                struct desktop_window *w = &windows[id];
                bring_to_front((uint8_t)id);
                start_menu_open = false;
                changed = 1;

                uint32_t bx = (uint32_t)w->x + w->width - 64;
                uint32_t by = (uint32_t)w->y + 7;
                if (hit_rect(x, y, (int32_t)bx, (int32_t)by, 18, 16)) {
                    minimize_window((uint8_t)id);
                } else if (hit_rect(x, y, (int32_t)bx + 20, (int32_t)by, 18, 16)) {
                    toggle_maximize((uint8_t)id);
                } else if (hit_rect(x, y, (int32_t)bx + 40, (int32_t)by, 18, 16)) {
                    close_window((uint8_t)id);
                } else if (hit_rect(x, y, w->x + (int32_t)w->width - 18, w->y + (int32_t)w->height - 18, 18, 18)) {
                    begin_window_resize((uint8_t)id, x, y);
                } else if (hit_rect(x, y, w->x + 4, w->y + 4, w->width > 8 ? w->width - 8 : 0, DESKTOP_TITLEBAR_H)) {
                    begin_window_drag((uint8_t)id, x, y);
                }
            } else if (start_menu_open) {
                start_menu_open = false;
                changed = 1;
            }
        }
    }

    previous_mouse_buttons = buttons;
    if (changed) {
        desktop_redraw();
    }
}

void desktop_draw_mouse(uint32_t x, uint32_t y)
{
    if (!fb.available) {
        return;
    }
    if (x + CURSOR_W > fb.width) {
        x = fb.width > CURSOR_W ? fb.width - CURSOR_W : 0;
    }
    if (y + CURSOR_H > fb.height) {
        y = fb.height > CURSOR_H ? fb.height - CURSOR_H : 0;
    }
    if (cursor_visible && cursor_x == x && cursor_y == y) {
        return;
    }

    if (cursor_visible) {
        for (uint32_t yy = 0; yy < CURSOR_H; ++yy) {
            for (uint32_t xx = 0; xx < CURSOR_W; ++xx) {
                framebuffer_put_pixel(cursor_x + xx, cursor_y + yy, cursor_bg[yy * CURSOR_W + xx]);
            }
        }
    }

    for (uint32_t yy = 0; yy < CURSOR_H; ++yy) {
        for (uint32_t xx = 0; xx < CURSOR_W; ++xx) {
            cursor_bg[yy * CURSOR_W + xx] = framebuffer_get_pixel(x + xx, y + yy);
        }
    }

    for (uint32_t yy = 0; yy < CURSOR_H; ++yy) {
        for (uint32_t xx = 0; xx < CURSOR_W; ++xx) {
            char cell = cursor_art[yy][xx];
            if (cell == 'X') {
                framebuffer_put_pixel(x + xx, y + yy, 0x00000000);
            } else if (cell == 'O') {
                framebuffer_put_pixel(x + xx, y + yy, 0x00ffffff);
            }
        }
    }
    cursor_x = x;
    cursor_y = y;
    cursor_visible = true;
}
