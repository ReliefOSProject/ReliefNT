#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "modules/console.h"
#include "../../drivers/video/framebuffer/boot_logo.c"

void *kernel_malloc(size_t size) { return malloc(size); }
void kernel_free(void *ptr) { free(ptr); }

static uint32_t pixels[80 * 48];
static uint32_t full_clear_count;
static uint32_t text_present_count;
static uint32_t presented[4];
static uint32_t last_codepoint;
static char serial_output[16384];
static size_t serial_length;
static struct framebuffer display = {
    .pixels = pixels, .width = 80, .height = 48, .pitch = 80 * 4,
    .bpp = 32, .bytes_per_pixel = 4, .available = true,
};
const struct framebuffer *framebuffer_get(void) { return &display; }
void framebuffer_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t c)
{
    if (!x && !y && w == display.width && h == display.height) ++full_clear_count;
    for (uint32_t row = y; row < y + h && row < 48; ++row)
        for (uint32_t col = x; col < x + w && col < 80; ++col)
            pixels[row * 80 + col] = c;
}
void framebuffer_rect_to(const struct framebuffer *target, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h, uint32_t c)
{
    if (!target || !target->available || !target->pixels ||
        x >= target->width || y >= target->height) return;
    if (!x && !y && w == target->width && h == target->height) ++full_clear_count;
    if (w > target->width - x) w = target->width - x;
    if (h > target->height - y) h = target->height - y;
    for (uint32_t row = y; row < y + h; ++row)
        for (uint32_t col = x; col < x + w; ++col)
            ((uint32_t *)(void *)((uint8_t *)target->pixels +
                (size_t)row * target->pitch))[col] = c;
}
void framebuffer_clear(uint32_t c) { framebuffer_rect(0, 0, 80, 48, c); }
void framebuffer_present(void) {}
void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    memcpy(presented, (uint32_t[]){x, y, w, h}, sizeof(presented));
    ++text_present_count;
}
uint32_t framebuffer_codepoint_width(uint32_t c) { return c == 0x4e2du ? 2u : 1u; }
void framebuffer_codepoint(uint32_t x, uint32_t y, uint32_t c, uint32_t fg, uint32_t bg)
{ last_codepoint = c; framebuffer_rect(x, y, 8 * framebuffer_codepoint_width(c), 16, c == ' ' ? bg : fg); }
void framebuffer_codepoint_to(const struct framebuffer *target, uint32_t x, uint32_t y,
                              uint32_t c, uint32_t fg, uint32_t bg)
{ last_codepoint = c; framebuffer_rect_to(target, x, y, 8 * framebuffer_codepoint_width(c), 16, c == ' ' ? bg : fg); }
void framebuffer_codepoint_bold_to(const struct framebuffer *target, uint32_t x, uint32_t y,
                                   uint32_t c, uint32_t fg, uint32_t bg)
{ framebuffer_codepoint_to(target, x, y, c, fg, bg); }
void serial_write(const char *text)
{
    size_t length = strlen(text);
    assert(length < sizeof(serial_output) - serial_length);
    memcpy(serial_output + serial_length, text, length + 1u);
    serial_length += length;
}
void vga_putc(char c) { (void)c; }
uint64_t time_uptime_us(void) { return 0; }
uint64_t kernel_irq_save(void) { return 0; }
void kernel_irq_restore(uint64_t flags) { (void)flags; }
uint32_t smp_current_cpu(void) { return 0; }
uint32_t smp_cpu_count(void) { return 1; }
void smp_membarrier_poll(void) {}

/** @brief Reject panic-only GPU publication during ordinary VT rendering.
 * @param x Unused left pixel. @param y Unused top pixel.
 * @param width Unused width. @param height Unused height.
 */
void graphics_service_panic_damage(uint32_t x, uint32_t y,
                                   uint32_t width, uint32_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    assert(!"ordinary VT output entered the fatal display path");
}

/** @brief Keep text intact during BS motion and erase only explicitly overwritten cells. */
static void test_backspace_preserves_text(void)
{
    static const char text[] = "\033[?25l\033[0m\033[2J\033[HABCDE";
    uint32_t saved[80 * 48];
    console_vt_write(2, text, sizeof(text) - 1);
    memcpy(saved, pixels, sizeof(saved));
    console_vt_write(2, "\b\b", 2);
    assert(fb_col == 3 && fb_row == 0);
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);
    console_vt_write(2, "\r\b", 2);
    assert(fb_col == 0 && fb_row == 0);
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);

    /* Canonical erase explicitly writes a space over E, then moves back. */
    static const char erase[] = "\033[6G\b \b";
    console_vt_write(2, erase, sizeof(erase) - 1);
    assert(fb_col == 4 && fb_row == 0);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 32; x < 40; ++x) saved[y * 80 + x] = 0;
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);

    console_vt_write(2, text, sizeof(text) - 1);
    memcpy(saved, pixels, sizeof(saved));
    console_vt_write(2, "\033[?25h", sizeof("\033[?25h") - 1);
    console_vt_write(2, "\b", 1);
    assert(fb_col == 4 && fb_console_tty_cursor_visible);
    fb_console_hide_tty_cursor();
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);

    console_vt_activate(3, false);
    console_vt_write(2, "\b", 1);
    console_vt_activate(2, false);
    assert(fb_col == 3);
    fb_console_hide_tty_cursor();
    assert(memcmp(saved, pixels, sizeof(saved)) == 0);
    puts("BS cursor motion preserves foreground/background text, first-column bounds and explicit erase");
}

static void expect_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    assert(text_present_count == 1);
    if (memcmp(presented, (uint32_t[]){x, y, w, h}, sizeof(presented)))
        fprintf(stderr, "damage=(%u,%u %ux%u), expected (%u,%u %ux%u)\n",
                presented[0], presented[1], presented[2], presented[3], x, y, w, h);
    assert(!memcmp(presented, (uint32_t[]){x, y, w, h}, sizeof(presented)));
    text_present_count = 0;
}

static void test_text_damage(void)
{
    static const char reset[] = "\033[?25l\033[0m\033[2J\033[H";
    console_vt_write(2, reset, sizeof(reset) - 1);
    text_present_count = 0;
    console_vt_write(2, "\033[32m", 5);
    assert(text_present_count == 0);
    console_vt_write(2, "A", 1);
    expect_damage(0, 0, 8, 16);
    console_vt_write(2, "\033[?25h", 6);
    expect_damage(8, 14, 8, 2);
    console_vt_write(2, "\r", 1);
    expect_damage(0, 14, 16, 2);
    console_vt_write(2, "\033[?25l", 6);
    expect_damage(0, 14, 8, 2);
    console_vt_write(3, "off-screen", 10);
    assert(text_present_count == 0);
    console_vt_write(2, "\xe4\xb8\xad", 3);
    expect_damage(0, 0, 16, 16);
    console_vt_write(2, "\033[1mB", sizeof("\033[1mB") - 1);
    expect_damage(16, 0, 8, 16);
    console_vt_write(2, "\033[3;10HC", sizeof("\033[3;10HC") - 1);
    expect_damage(72, 32, 8, 16);
    uint32_t top[80 * 16];
    memcpy(top, pixels, sizeof(top));
    console_vt_write(2, "\033[2;3r\033[42m\033[9999S",
                     sizeof("\033[2;3r\033[42m\033[9999S") - 1);
    expect_damage(0, 16, 80, 32);
    assert(!memcmp(top, pixels, sizeof(top)));
    for (unsigned i = 80 * 16; i < 80 * 48; ++i)
        assert(pixels[i] == 0x006fc77au);
    console_vt_write(2, "\033[1;3r\033[0m", sizeof("\033[1;3r\033[0m") - 1);
    console_vt_write(2, "\033[Habcdefg\033[3G", sizeof("\033[Habcdefg\033[3G") - 1);
    text_present_count = 0;
    console_vt_write(2, "\033[2@", sizeof("\033[2@") - 1);
    expect_damage(16, 0, 64, 16);
    console_vt_write(2, "\033[P", sizeof("\033[P") - 1);
    expect_damage(16, 0, 64, 16);
    console_vt_write(2, "\033[2X", sizeof("\033[2X") - 1);
    expect_damage(16, 0, 16, 16);
}

int main(void)
{
    console_log_configure("logcolor=0");
    console_enable_framebuffer();
    console_info("boot ready\n");
    uint32_t boot_col = fb_col;
    uint32_t boot_row = fb_row;
    uint32_t boot_clear_count = full_clear_count;
    size_t boot_history_length = log_len - console_log_replay_start();
    console_vt_activate(1, false);
    assert(full_clear_count == boot_clear_count);
    assert(fb_col == boot_col && fb_row == boot_row);
    assert(vt_text[0].length == boot_history_length);
    assert(strstr(vt_text[0].bytes, "[INFO] boot ready\n") != NULL);
    console_vt_write(1, "init\n", 5);
    assert(full_clear_count == boot_clear_count);
    assert(vt_text[0].length == boot_history_length + 5u);
    assert(memcmp(vt_text[0].bytes + boot_history_length, "init\n", 5) == 0);
    static const char openrc_output[] = "\033[1;32mOpenRC\033[0m starting\r\n";
    size_t before_openrc = vt_text[0].length;
    uint32_t before_openrc_present = text_present_count;
    console_vt_write(1, openrc_output, sizeof(openrc_output) - 1u);
    size_t before_serial_log = log_len;
    console_serial_write_len(openrc_output, sizeof(openrc_output) - 1u);
    assert(log_len == before_serial_log);
    size_t startup_history_length = before_openrc + sizeof(openrc_output) - 1u;
    assert(vt_text[0].length == startup_history_length);
    assert(memcmp(vt_text[0].bytes + before_openrc, openrc_output,
                  sizeof(openrc_output) - 1u) == 0);
    assert(strstr(serial_output, openrc_output) != NULL);
    assert(text_present_count == before_openrc_present + 1u);
    assert(fb_col == 0 && fb_console_fg_color == console_fg());
    assert(full_clear_count == boot_clear_count);
    uint32_t init_col = fb_col;
    uint32_t init_row = fb_row;
    console_vt_activate(1, false);
    assert(full_clear_count == boot_clear_count);
    assert(fb_col == init_col && fb_row == init_row);
    console_vt_activate(2, false);
    console_vt_activate(1, false);
    assert(vt_text[0].length == startup_history_length);
    assert(strstr(vt_text[0].bytes, "[INFO] boot ready\ninit\n") != NULL);
    assert(strstr(vt_text[0].bytes, openrc_output) != NULL);
    console_vt_activate(2, false);
    assert(fb_console_tty_cursor_visible && fb_console_tty_cursor_x == 0);
    console_vt_write(2, "a", 1);
    assert(fb_console_tty_cursor_x == 8);
    console_vt_write(2, "b", 1);
    assert(fb_console_tty_cursor_x == 16);
    uint32_t before[80 * 48];
    memcpy(before, pixels, sizeof(pixels));
    console_write("background kernel log\n");
    console_write_len("kmsg diagnostic\n", sizeof("kmsg diagnostic\n") - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(vt_text[0].length == startup_history_length);
    assert(strstr(serial_output, "kmsg diagnostic\n") != NULL);
    static const char service_output[] = " * Starting reliefos-dhcp ... [ ok ]\n";
    console_vt_write(1, service_output, sizeof(service_output) - 1u);
    console_serial_write_len(service_output, sizeof(service_output) - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(vt_text[1].length == 2u);
    assert(memcmp(vt_text[0].bytes + startup_history_length, service_output,
                  sizeof(service_output) - 1u) == 0);
    assert(strstr(serial_output, service_output) != NULL);
    console_vt_write(3, "another terminal", 16);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    console_vt_activate(1, true);
    memcpy(before, pixels, sizeof(pixels));
    static const char graphical_output[] = "service still running\n";
    console_vt_write(1, graphical_output, sizeof(graphical_output) - 1u);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(strstr(vt_text[0].bytes, graphical_output) != NULL);
    console_vt_activate(1, false);
    assert(strstr(vt_text[0].bytes, openrc_output) != NULL);
    assert(strstr(vt_text[0].bytes, service_output) != NULL);
    assert(strstr(vt_text[0].bytes, graphical_output) != NULL);
    char key[8];
    console_vt_activate(2, false);
    console_vt_write(2, "\033[?1h", sizeof("\033[?1h") - 1);
    assert(vga_console_key(2, 72, 1, key) == 3 && !memcmp(key, "\033OA", 3));
    assert(vga_console_key(1, 72, 1, key) == 3 && !memcmp(key, "\033[A", 3));
    assert(vga_console_key(2, 61, 1, key) == 3 && !memcmp(key, "\033OR", 3));
    assert(vga_console_key(2, 15, 2, key) == 3 && !memcmp(key, "\033[Z", 3));
    console_vt_write(2, "\033[?1l", sizeof("\033[?1l") - 1);
    assert(vga_console_key(2, 72, 1, key) == 3 && !memcmp(key, "\033[A", 3));
    console_vt_write(2, "\033(0q", sizeof("\033(0q") - 1);
    assert(last_codepoint == 0x2500);
    console_vt_write(2, "\033(Bq", sizeof("\033(Bq") - 1);
    assert(last_codepoint == 'q');
    console_vt_write(2, "\033)0\016x", sizeof("\033)0\016x") - 1);
    assert(last_codepoint == 0x2502);
    console_vt_write(2, "\017x", sizeof("\017x") - 1);
    assert(last_codepoint == 'x');
    console_vt_write(2, "\033[?25l", sizeof("\033[?25l") - 1);
    assert(!fb_console_tty_cursor_visible);
    console_vt_write(2, "\033[?25h", sizeof("\033[?25h") - 1);
    assert(fb_console_tty_cursor_visible);
    test_backspace_preserves_text();
    test_text_damage();
    static const char full_screen[] = "\033[?25l\033[44m\033[2J\033[H";
    console_vt_write(2, full_screen, sizeof(full_screen) - 1);
    for (unsigned i = 0; i < CONSOLE_VT_TEXT_CAP / 4u + 100u; ++i)
        console_vt_write(2, "\033[Hq", 4);
    memcpy(before, pixels, sizeof(pixels));
    uint32_t foreground = fb_console_fg_color;
    uint32_t background = fb_console_bg_color;
    console_vt_activate(3, false);
    console_vt_activate(2, false);
    assert(memcmp(before, pixels, sizeof(pixels)) == 0);
    assert(fb_console_fg_color == foreground && fb_console_bg_color == background);
    console_vt_activate(3, false);
    console_vt_write(2, "\033[42", 4);
    console_vt_write(3, "\033[41m", 5);
    console_vt_write(2, "m\033[2J\033[H", 8);
    for (unsigned i = 0; i < CONSOLE_VT_TEXT_CAP / 4u + 100u; ++i)
        console_vt_write(2, "\033[Hq", 4);
    console_vt_activate(2, false);
    assert(pixels[79 + 47 * 80] == 0x006fc77au);
    assert(fb_console_bg_color == 0x006fc77au);
    console_vt_write(2, "\xe4", 1);
    console_vt_activate(3, false);
    console_vt_write(3, "x", 1);
    console_vt_write(2, "\xb8\xad", 2);
    assert(last_codepoint == 0x4e2d);
    puts("VT continuity, isolation, framebuffer ncurses keys, DEC graphics and cursor modes passed");
    return 0;
}
