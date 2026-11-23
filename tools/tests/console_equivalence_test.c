/* Compare the two independent terminal implementations after each operation,
 * using the real framebuffer font to render the VGA engine's expected cells. */
#define main framebuffer_edit_main
#include "framebuffer_edit_test.c"
#undef main

static struct vga_terminal reference;

static void compare(const char *sequence)
{
    write_text(2, sequence);
    for (const unsigned char *s = (const unsigned char *)sequence; *s; ++s)
        vga_terminal_feed(&reference, *s);
    const struct vga_page *p = &reference.page[reference.alternate];
    if (fb_col != p->cursor.x || fb_row != p->cursor.y ||
        fb_modes.pending_wrap != p->pending_wrap) {
        fprintf(stderr, "sequence %s: framebuffer cursor %u,%u wrap=%d, VGA %u,%u wrap=%d\n",
                sequence, fb_col, fb_row, fb_modes.pending_wrap,
                p->cursor.x, p->cursor.y, p->pending_wrap);
        abort();
    }
    struct framebuffer expected = fb;
    expected.pixels = (uint32_t *)(void *)expected_pixels;
    for (unsigned y = 0; y < 25; ++y)
        for (unsigned x = 0; x < 80; ++x)
            framebuffer_codepoint_to(&expected, x * 8, y * 16,
                p->cells[y * 80 + x] & 255, console_fg(), console_panel());
    for (unsigned row = 0; row < fb.height; ++row)
        if (memcmp((uint8_t *)fb.pixels + (size_t)row * fb.pitch,
                    expected_pixels + (size_t)row * fb.pitch, fb.width * fb.bytes_per_pixel)) {
            fprintf(stderr, "sequence %s: framebuffer/VGA pixels differ on scanline %u\n", sequence, row);
            abort();
        }
    check_guards();
}

int main(void)
{
    memset(storage, 0xa5, sizeof(storage));
    fb = (struct framebuffer){
        .pixels = (uint32_t *)(void *)(storage + GUARD),
        .width = 640, .height = 400, .pitch = 640 * 4 + 13,
        .bpp = 32, .bytes_per_pixel = 4, .available = true,
        .type = MULTIBOOT2_FRAMEBUFFER_TYPE_RGB,
        .red_field_position = 16, .red_mask_size = 8,
        .green_field_position = 8, .green_mask_size = 8,
        .blue_field_position = 0, .blue_mask_size = 8,
    };
    console_enable_framebuffer();
    console_vt_activate(2, false);
    reset_screen();
    vga_terminal_reset(&reference);
    static const char *const sequences[] = {
        "abcdefghij\r\tX", "\033[3g\033[5G\033H\r\t!", "\033c\033[?25l",
        "\033[1;80HX", "\bY", "\033[1;80HZ\033[@", "\033[?6hQ",
        "\033[?6l\033[2;24r", "\033[2;1HABC\033[L", "\033[M", "\033[S", "\033[T",
        "\033[2;1H\033M", "\033[25;10H\nX", "\033[r", "\033[0d", "\033[0G",
        "\0337\033[10;20Htext\0338!", "\033[?1049hALT", "\033[?1049l",
        "\033[4h\033[HXY\033[4l", "\033[3;5H\033[@A\033[2PB", "\033[20h\nC\033[20l",
    };
    for (unsigned i = 0; i < sizeof(sequences) / sizeof(sequences[0]); ++i) compare(sequences[i]);
    uint32_t random = 0x4102;
    for (unsigned i = 0; i < 240; ++i) {
        random = random * 1664525u + 1013904223u;
        unsigned n = random % 90;
        char sequence[64];
        static const char commands[] = "ABCDGHdefa@PXLMTZS";
        switch (i % 8) {
        case 0: snprintf(sequence, sizeof(sequence), "\033[%u%c", n, commands[random % (sizeof(commands) - 1)]); break;
        case 1: snprintf(sequence, sizeof(sequence), "%c\t%c\b!", 'a' + n % 26, 'A' + n % 26); break;
        case 2: snprintf(sequence, sizeof(sequence), "\033[%u;%uH", n % 26, n); break;
        case 3: snprintf(sequence, sizeof(sequence), "\033[?%u%c", n & 1 ? 6 : 7, n & 2 ? 'h' : 'l'); break;
        case 4: snprintf(sequence, sizeof(sequence), "\033[%u;24r", n % 24); break;
        case 5: snprintf(sequence, sizeof(sequence), "\033[?1049%c", n & 1 ? 'h' : 'l'); break;
        case 6: snprintf(sequence, sizeof(sequence), "\0337\n\033D\0338"); break;
        default: snprintf(sequence, sizeof(sequence), "\033[%uJ\033[%uK", n % 4, n % 3); break;
        }
        compare(sequence);
    }
    puts("PASS framebuffer/VGA equivalence: 265 cursor, edit, tab, wrap, scrolling and alternate-screen operations");
    return 0;
}
