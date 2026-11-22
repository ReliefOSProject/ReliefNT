#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modules/framebuffer.h"
#define CONSOLE_TEST_REPLY
#include "modules/console.h"

#define GUARD 32u
#define CAPACITY (2u * 1024u * 1024u)
static uint8_t storage[GUARD + CAPACITY + GUARD];
static uint8_t expected_pixels[CAPACITY];

static bool fail_allocations;
static char replies[CONSOLE_VT_COUNT][128];
static size_t reply_length[CONSOLE_VT_COUNT];
void pty_vt_reply(uint32_t number, const char *text, size_t length)
{
    assert(number >= 1 && number <= CONSOLE_VT_COUNT);
    size_t *used = &reply_length[number - 1];
    assert(*used + length <= sizeof(replies[0]));
    memcpy(replies[number - 1] + *used, text, length);
    *used += length;
}
void *kernel_malloc(size_t n) { return fail_allocations ? NULL : malloc(n); }
void kernel_free(void *p) { free(p); }
void serial_write(const char *s) { (void)s; }
void vga_putc(char c) { (void)c; }
uint64_t time_uptime_us(void) { return 0; }
uint64_t kernel_irq_save(void) { return 0; }
void kernel_irq_restore(uint64_t flags) { (void)flags; }
uint32_t smp_current_cpu(void) { return 0; }
uint32_t smp_cpu_count(void) { return 1; }
void smp_membarrier_poll(void) {}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ assert(lock->state == 1 && !flags); lock->state = 0; }
uint32_t boot_logo_draw(const struct framebuffer *target, uint32_t cpus)
{ (void)target; (void)cpus; return 0; }
void graphics_service_panic_damage(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)x; (void)y; (void)w; (void)h; assert(!"panic rendering"); }

static void write_text(uint32_t vt, const char *s)
{ console_vt_write(vt, s, strlen(s)); }

static void reset_screen(void)
{ write_text(2, "\033c\033[?25l"); }

static void check_guards(void)
{
    const uint8_t *p = (const uint8_t *)fb.pixels;
    size_t bytes = (size_t)fb.pitch * fb.height;
    for (unsigned i = 0; i < GUARD; ++i) {
        assert(storage[i] == 0xa5);
        assert(p[bytes + i] == 0xa5);
    }
    for (unsigned y = 0; y < fb.height; ++y)
        for (unsigned x = fb.width * fb.bytes_per_pixel; x < fb.pitch; ++x)
            assert(p[(size_t)y * fb.pitch + x] == 0xa5);
}

/** @brief Compare edited pixels against a fresh rendering of the expected text. */
static void expect_screen(const char *input, const char *expected, unsigned col, unsigned row)
{
    reset_screen();
    write_text(2, input);
    if (fb_row != row || fb_col != col)
        fprintf(stderr, "edit %s: cursor %u,%u, expected %u,%u\n", expected, fb_col, fb_row, col, row);
    assert(fb_row == row && fb_col == col);
    memcpy(expected_pixels, fb.pixels, (size_t)fb.pitch * fb.height);
    reset_screen();
    write_text(2, expected);
    if (memcmp(expected_pixels, fb.pixels, (size_t)fb.pitch * fb.height)) {
        fprintf(stderr, "framebuffer edit should display: %s\n", expected);
        abort();
    }
    check_guards();
}

static void expect_edit(const char *input, const char *expected, unsigned col)
{ expect_screen(input, expected, col, 0); }

static void test_edits(void)
{
    /* Readline emits ICH/DCH after moving left over an existing suffix. */
    expect_edit("abcdefg\033[5D\033[1@X", "abXcdefg", 3);
    expect_edit("abcdefg\033[5D\033[2@YZ", "abYZcdefg", 4);
    expect_edit("abcdefghij\033[7D\033[1P", "abcefghij", 3);
    expect_edit("abcdefg\b\b\b\b\b\033[@X", "abXcdefg", 3);
    expect_edit("abcdefg\033[5D\033[0@X", "abXcdefg", 3);
    expect_edit("abcdefg\033[5D\033[P", "abdefg", 2);
    expect_edit("abcdefg\033[5D\033[0P", "abdefg", 2);
    expect_edit("abcdefg\033[5D\033[2X", "ab  efg", 2);
    expect_edit("abcdefg\033[5DX", "abXdefg", 3);
    expect_edit("\033[4habc\033c\033[?25labcdef\033[4DX", "abXdef", 3);
    expect_edit("abcdefgh\033[6D\033[4hXY\033[4l", "abXYcdefgh", 4);
    expect_edit("abcdef\033[4D\033[4h\033[0mXY\033[4lZ", "abXYZdef", 5);
    expect_edit("abcdef\033[4D\033[4h\xe4\xb8\xad\033[4l", "ab\xe4\xb8\xad" "cdef", 4);
    expect_edit("AB\033[31;4mCD\033[0mEF\033[4D\033[2@XY",
                "ABXY\033[31;4mCD\033[0mEF", 4);
    expect_edit("AB\033[31;44mCD\033[0mEFGH\033[6D\033[2P", "ABEFGH", 2);
    expect_edit("ABCD\033[3D\033[1mX", "A\033[1mX\033[0mCD", 2);
    expect_edit("ABCD\033[3D\033[1m \033[0m", "A CD", 2);
    expect_edit("ABCDEFGHIJKL\033[1;11H\033[9999@X", "ABCDEFGHIJX ", 11);
    expect_edit("ABCDEFGHIJKL\033[1;11H\033[9999PX", "ABCDEFGHIJX ", 11);
    expect_edit("ABCDEFGHIJKL\033[1;12H\033[@X", "ABCDEFGHIJKX", 11);
    expect_edit("ABCDEFGHIJKL\033[1;12H\033[P", "ABCDEFGHIJK ", 11);
    puts("PASS framebuffer ICH/DCH/IRM: text, attributes, wide characters, clipping and erase");
}

static void test_terminal_controls(void)
{
    expect_edit("ABCDEFGHIJKL\r\tX", "ABCDEFGHXJKL", 9);
    expect_edit("ABCDEFGHIJKL\t", "ABCDEFGHIJKL", 11);
    expect_edit("\033[3g\033[4G\033H\r\tX", "   X", 4);
    expect_edit("abcdefghijkl\033[ZXY", "abcdefghXYkl", 10);
    expect_edit("abcdef\033[0dX", "abcdefX", 7);
    expect_edit("ABCDEFGHIJKL\bX", "ABCDEFGHIJXL", 11);
    expect_edit("ABCDEFGHIJKL\033[?7lAB", "ABCDEFGHIJKB", 11);
    expect_edit("ABCDEFGHIJKL\033[@", "ABCDEFGHIJK ", 11);
    expect_screen("ABCDEFGHIJKL\033[31mX", "ABCDEFGHIJKL\r\n\033[31mX", 1, 1);
    expect_screen("ab\nX", "ab\033[2;3HX", 3, 1);
    expect_screen("ab\033D" "X", "ab\033[2;3HX", 3, 1);
    expect_screen("ab\033[20h\nX", "ab\r\nX", 1, 1);
    expect_edit("\033[31;4mAB\0337\033[H\033[0mXY\0338Z", "XY\033[31;4mZ", 3);
    expect_edit("A\033[!pB\033[3 qC\033%GD", "ABCD", 4);
    expect_screen("\033[?6h\0337\033[2;3r\0338X", "\033[2;1HX", 1, 1);
    expect_edit("A\033]ignored\033Xpayload\033\\B", "AB", 2);
    expect_edit("A\033Pignored\007payload\033\\B", "AB", 2);
    expect_edit("AB\033[2;\033[1GX", "XB", 1);
    expect_edit("\033[3\t1mX", "\033[9G\033[31mX", 9);
    expect_edit("abc\r\033[99K", "abc", 0);
    expect_edit("abc\r\033[99J", "abc", 0);
    expect_edit("\033[38;2;1mX", "X", 1);
    expect_edit("\033[31;1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16mX", "X", 1);

    static const char rows[] = "top\033[2;1Hmiddle\033[3;1Hbottom";
    char input[256];
    snprintf(input, sizeof(input), "%s\033[2;1H\033[L", rows);
    expect_screen(input, "top\033[3;1Hmiddle", 0, 1);
    snprintf(input, sizeof(input), "%s\033[2;1H\033[M", rows);
    expect_screen(input, "top\033[2;1Hbottom", 0, 1);
    snprintf(input, sizeof(input), "%s\033[2;4H\033[S", rows);
    expect_screen(input, "middle\033[2;1Hbottom", 3, 1);
    snprintf(input, sizeof(input), "%s\033[2;4H\033[T", rows);
    expect_screen(input, "\033[2;1Htop\033[3;1Hmiddle", 3, 1);
    snprintf(input, sizeof(input), "%s\033[2;3r\033[2;1H\033M", rows);
    expect_screen(input, "top\033[3;1Hmiddle", 0, 1);
    expect_edit("\033[2;3r\033[r\033[?6hX", "X", 1);
    expect_screen("\033[2;3r\033[?6h\033[H\033[99BX", "\033[3;1HX", 1, 2);
    puts("PASS framebuffer tabs, margins, line edits, reverse scroll, wrap, saved rendition and bounded parsing");
}

static void test_alternate_screens(void)
{
    expect_edit("primary\033[?1049hOTHER\033[?1049lX", "primaryX", 8);
    expect_edit("primary\033[?1049hALT\033[?1049h!\033[?1049lX", "primaryX", 8);
    reset_screen();
    write_text(2, "\033[31mPRIMARY\033[?1049hALT\033[?25l");
    console_vt_activate(3, false);
    write_text(3, "\033c\033[?25lVISIBLE");
    size_t bytes = (size_t)fb.pitch * fb.height;
    memcpy(expected_pixels, fb.pixels, bytes);
    write_text(2, "\033[?1049lX");
    assert(!memcmp(expected_pixels, fb.pixels, bytes));
    console_vt_activate(2, false);
    assert(!vt_text[1].alternate && fb_col == 8);
    memcpy(expected_pixels, fb.pixels, bytes);
    reset_screen();
    write_text(2, "\033[31mPRIMARYX");
    assert(!memcmp(expected_pixels, fb.pixels, bytes));
    expect_edit("P\033[?47hALT\033[?47lQ\033[?47h!", "ALT!", 4);
    write_text(2, "\033[?47l");
    check_guards();
    puts("PASS framebuffer primary/alternate screen restoration, including hidden VT output");
}

static void test_replies_and_failed_alternate(void)
{
    reset_screen();
    memset(reply_length, 0, sizeof(reply_length));
    write_text(2, "\033[2;5H\033[6");
    console_vt_activate(3, false);
    write_text(3, "\033c\033[?25lVISIBLE");
    size_t bytes = (size_t)fb.pitch * fb.height;
    memcpy(expected_pixels, fb.pixels, bytes);
    write_text(2, "n\033[5n\033[c\033[>c");
    static const char expected[] = "\033[2;5R\033[0n\033[?6c\033[>0;1;0c";
    assert(reply_length[1] == sizeof(expected) - 1);
    assert(!memcmp(replies[1], expected, sizeof(expected) - 1));
    assert(reply_length[2] == 0 && !memcmp(expected_pixels, fb.pixels, bytes));
    write_text(2, "\033[6 n\033[??6n\033[>6n");
    assert(reply_length[1] == sizeof(expected) - 1);
    reply_length[1] = 0;
    write_text(2, "\033[2;3r\033[?6h\033[?6n");
    assert(reply_length[1] == 7 && !memcmp(replies[1], "\033[?1;1R", 7));
    console_vt_activate(4, false);
    write_text(4, "\033c\033[?25lPRIMARY");
    memcpy(expected_pixels, fb.pixels, bytes);
    fail_allocations = true;
    write_text(4, "\033[?1049h");
    fail_allocations = false;
    assert(!vt_text[3].alternate && fb_col == 7);
    assert(!memcmp(expected_pixels, fb.pixels, bytes));
    write_text(4, "\033[?1049hALT\033c\033[?25lRESET\033[?47h");
    assert(vt_text[3].alternate && fb_col == 0);
    write_text(4, "\033[?47l");
    assert(fb_col == 5);
    console_vt_activate(2, false);
    check_guards();
    puts("PASS framebuffer queries: fragmented/hidden replies, origin coordinates, malformed requests and alternate allocation failure/reset");
}

static void test_background_edits(void)
{
    reset_screen();
    write_text(2, "abcdefg\033[5D\033[4h\033[1");
    console_vt_activate(3, false);
    write_text(3, "\033c\033[?25lother\033[3DX");
    size_t bytes = (size_t)fb.pitch * fb.height;
    memcpy(expected_pixels, fb.pixels, bytes);
    /* Complete a fragmented ICH, then use IRM on a retained off-screen VT. */
    write_text(2, "@\033[4lX\033[4hY");
    assert(memcmp(expected_pixels, fb.pixels, bytes) == 0);
    console_vt_activate(2, false);
    assert(fb_col == 4 && fb_row == 0);
    memcpy(expected_pixels, fb.pixels, bytes);
    reset_screen();
    write_text(2, "abXYcdefg");
    assert(memcmp(expected_pixels, fb.pixels, bytes) == 0);
    check_guards();
    puts("PASS framebuffer edits and insert mode survive fragmented writes and VT switches");
}

static void test_offsets(void)
{
    reset_screen();
    fb_console_initialize_fullscreen();
    /* initialize_fullscreen resets offsets; retain a deliberately inset grid. */
    fb_x = 3; fb_y = 2;
    reset_screen();
    write_text(2, "\033[2;1Hnext row\033[Habcdefghijkl");
    write_text(2, "\033[4G");
    memcpy(expected_pixels, fb.pixels, (size_t)fb.pitch * fb.height);
    write_text(2, "\033[2@");
    for (unsigned y = 0; y < fb.height; ++y) {
        const uint8_t *before = expected_pixels + (size_t)y * fb.pitch;
        const uint8_t *after = (const uint8_t *)fb.pixels + (size_t)y * fb.pitch;
        if (y < fb_y || y >= fb_y + RELIEFOS_FONT_H)
            assert(memcmp(before, after, fb.pitch) == 0);
        else {
            unsigned first = (fb_x + 3 * RELIEFOS_FONT_W) * fb.bytes_per_pixel;
            unsigned last = (fb_x + fb_cols * RELIEFOS_FONT_W) * fb.bytes_per_pixel;
            assert(memcmp(before, after, first) == 0);
            assert(memcmp(before + last, after + last, fb.pitch - last) == 0);
        }
    }
    check_guards();
    puts("PASS framebuffer character edits preserve other rows, insets and pitch padding");
}

static void test_resized_alternate(void)
{
    console_vt_activate(5, false);
    vt_text[4].length = 0;
    reply_length[4] = 0;
    write_text(5, "\033c\033[?25lPRIMARY\033[6n\033[?1049hALT");
    size_t answered = reply_length[4];
    assert(answered && vt_text[4].alternate);
    console_vt_activate(3, false);
    fb.width = 107; fb.height = 69;
    fb.pitch = fb.width * fb.bytes_per_pixel + 13;
    memset(storage, 0xa5, sizeof(storage));
    /* Off-screen output reconstructs both pages in the new layout. */
    write_text(5, "X");
    assert(reply_length[4] == answered);
    console_vt_activate(5, false);
    assert(vt_text[4].alternate && fb_col == 4 && fb_row == 0);
    write_text(5, "\033[?1049lY");
    assert(!vt_text[4].alternate && fb_col == 8);
    size_t bytes = (size_t)fb.pitch * fb.height;
    memcpy(expected_pixels, fb.pixels, bytes);
    write_text(5, "\033c\033[?25lPRIMARYY");
    assert(!memcmp(expected_pixels, fb.pixels, bytes));
    check_guards();
    puts("PASS framebuffer alternate pages survive layout reconstruction without duplicate terminal replies");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    unsigned bpp = (unsigned)strtoul(argv[1], NULL, 10);
    assert(bpp == 24 || bpp == 32);
    memset(storage, 0xa5, sizeof(storage));
    fb = (struct framebuffer){
        .pixels = (uint32_t *)(void *)(storage + GUARD),
        .width = 99, .height = 53, .pitch = 99 * (bpp / 8) + 13,
        .bpp = bpp, .bytes_per_pixel = bpp / 8, .available = true,
        .type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB,
        .red_field_position = 16, .red_mask_size = 8,
        .green_field_position = 8, .green_mask_size = 8,
        .blue_field_position = 0, .blue_mask_size = 8,
    };
    console_enable_framebuffer();
    console_vt_activate(2, false);
    test_edits();
    test_background_edits();
    test_terminal_controls();
    test_alternate_screens();
    test_replies_and_failed_alternate();
    test_offsets();
    test_resized_alternate();
    return 0;
}
