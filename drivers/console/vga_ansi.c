#include "vga_terminal.h"

static const uint8_t ansi_to_vga[16] = {0,4,2,6,1,5,3,7,8,12,10,14,9,13,11,15};
static const uint32_t vga_rgb[16] = {
    0x000000,0x0000aa,0x00aa00,0x00aaaa,0xaa0000,0xaa00aa,0xaa5500,0xaaaaaa,
    0x555555,0x5555ff,0x55ff55,0x55ffff,0xff5555,0xff55ff,0xffff55,0xffffff,
};

/** @brief Quantize an RGB request to the loaded, fixed 16-color VGA palette. */
static uint8_t nearest(unsigned r, unsigned g, unsigned b)
{
    unsigned best = 0, distance = UINT32_MAX;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    for (unsigned i = 0; i < 16; ++i) {
        int dr = (int)r - (int)(vga_rgb[i] >> 16);
        int dg = (int)g - (int)((vga_rgb[i] >> 8) & 255);
        int db = (int)b - (int)(vga_rgb[i] & 255);
        unsigned d = (unsigned)(dr*dr + dg*dg + db*db);
        if (d < distance) { best = i; distance = d; }
    }
    return (uint8_t)best;
}

/** @brief Reduce xterm's 256-color cube/gray ramp to hardware attributes. */
static uint8_t indexed(unsigned n)
{
    static const unsigned cube[] = {0,95,135,175,215,255};
    if (n < 16) return ansi_to_vga[n];
    if (n < 232) { n -= 16; return nearest(cube[n/36],cube[(n/6)%6],cube[n%6]); }
    if (n > 255) n = 255;
    n = 8 + (n - 232) * 10;
    return nearest(n,n,n);
}

/** @brief Parse an extended SGR operand, consuming its full color specification. */
static unsigned extended(struct vga_terminal *t, unsigned at, uint8_t *out)
{
    if (at + 1 >= t->count) return at;
    unsigned kind = t->params[at + 1];
    if (kind == 5 && at + 2 < t->count) { *out = indexed(t->params[at+2]); return at+2; }
    if (kind == 2) {
        unsigned first = at + 2;
        /* ISO colon form may include an empty colorspace slot. */
        if (t->colon[at] && first + 3 < t->count && t->colon[first+2] && !t->params[first]) ++first;
        if (first + 2 < t->count) {
            *out = nearest(t->params[first],t->params[first+1],t->params[first+2]);
            return first+2;
        }
    }
    /* Truncated colors are one invalid operand, not unrelated style codes. */
    return t->count - 1;
}

/** @brief Change colors/reverse/conceal; unsupported typeface effects are ignored. */
static void sgr(struct vga_terminal *t)
{
    struct vga_cursor *c = &t->page[t->alternate].cursor;
    for (unsigned i = 0; i < t->count; ++i) {
        unsigned n = t->params[i];
        if (!n) { c->fg = 7; c->bg = 0; c->reverse = c->conceal = false; }
        else if (n == 7 || n == 27) c->reverse = n == 7;
        else if (n == 8 || n == 28) c->conceal = n == 8;
        else if (n >= 30 && n <= 37) c->fg = ansi_to_vga[n - 30];
        else if (n >= 40 && n <= 47) c->bg = ansi_to_vga[n - 40];
        else if (n >= 90 && n <= 97) c->fg = ansi_to_vga[n - 90 + 8];
        else if (n >= 100 && n <= 107) c->bg = ansi_to_vga[n - 100 + 8];
        else if (n == 39) c->fg = 7;
        else if (n == 49) c->bg = 0;
        else if (n == 38 || n == 48 || n == 58) {
            uint8_t color = n == 48 ? c->bg : c->fg;
            i = extended(t,i,&color);
            if (n == 38) c->fg = color;
            if (n == 48) c->bg = color;
        }
    }
}

/** @brief Enter/leave alternate screens without losing primary terminal state. */
static void mode(struct vga_terminal *t, unsigned n, bool on)
{
    struct vga_page *p = &t->page[t->alternate];
    if (!t->prefix) {
        if (n == 4) p->insert = on;
        if (n == 20) p->newline = on;
        return;
    }
    if (t->prefix != '?') return;
    if (n == 1) t->application_cursor = on;
    else if (n == 5) t->reverse_screen = on;
    else if (n == 6) {
        p->cursor.origin = on; p->cursor.x = 0; p->cursor.y = on ? p->top : 0;
    } else if (n == 7) { p->cursor.wrap = on; p->pending_wrap = false; }
    else if (n == 25) t->visible = on;
    else if (n == 1048) {
        if (on) p->saved = p->cursor;
        else p->cursor = p->saved;
    } else if (n == 47 || n == 1047 || n == 1049) {
        if (on && !t->alternate) {
            if (n == 1049) t->page[0].saved = t->page[0].cursor;
            if (n != 47) vga_terminal_page_reset(&t->page[1]);
            t->alternate = 1;
        } else if (!on && t->alternate) {
            t->alternate = 0;
            if (n == 1049) t->page[0].cursor = t->page[0].saved;
        }
    }
}

/** @brief Append a bounded response; the caller delivers it without input echo. */
static void reply(struct vga_terminal *t, const char *s)
{
    while (*s && t->reply_length < sizeof(t->reply)) t->reply[t->reply_length++] = *s++;
}

/** @brief Append a small decimal cursor coordinate without formatted libc I/O. */
static void number(struct vga_terminal *t, unsigned n)
{
    char buf[12]; unsigned count = 0;
    do { buf[count++] = (char)('0' + n%10); n /= 10; } while (n);
    while (count && t->reply_length < sizeof(t->reply)) t->reply[t->reply_length++] = buf[--count];
}

/** @brief Clamp coordinate arithmetic before touching the fixed cell array. */
static unsigned minimum(unsigned a, unsigned b) { return a < b ? a : b; }

/** @brief Execute supported ECMA-48/VT cursor, edit, mode, color and query traffic. */
void vga_terminal_csi(struct vga_terminal *t, uint8_t cmd)
{
    struct vga_page *p = &t->page[t->alternate];
    struct vga_cursor *c = &p->cursor;
    unsigned zero = t->params[0], n = zero ? zero : 1;
    unsigned second = t->count > 1 && t->params[1] ? t->params[1] : 1;
    unsigned low = c->origin ? p->top : 0, high = c->origin ? p->bottom : 24;
    if (cmd == 'h' || cmd == 'l') {
        for (unsigned i = 0; i < t->count; ++i) mode(t,t->params[i],cmd == 'h');
        return;
    }
    if (cmd == 'n' && (!t->prefix || t->prefix == '?')) {
        if (zero == 5 && !t->prefix) reply(t,"\033[0n");
        if (zero == 6) {
            reply(t,t->prefix ? "\033[?" : "\033["); number(t,c->y-low+1);
            reply(t,";"); number(t,c->x+1); reply(t,"R");
        }
        return;
    }
    if (cmd == 'c') {
        if (!zero && !t->prefix) reply(t,"\033[?6c");
        if (!zero && t->prefix == '>') reply(t,"\033[>0;1;0c");
        return;
    }
    if (t->prefix) return;
    if (t->intermediate) {
        if (t->intermediate == ' ' && cmd == 'q' && zero <= 6) t->cursor_style = zero;
        return;
    }
    if (cmd != 'm' && cmd != 's' && cmd != 'u' && cmd != 'b') p->pending_wrap = false;
    switch (cmd) {
    case 'A': c->y = c->y > low + n ? c->y-n : low; break;
    case 'B': case 'e': c->y = minimum(c->y+n,high); break;
    case 'C': case 'a': c->x = minimum(c->x+n,79); break;
    case 'D': c->x = c->x > n ? c->x-n : 0; break;
    case 'E': c->y = minimum(c->y+n,high); c->x = 0; break;
    case 'F': c->y = c->y > low+n ? c->y-n : low; c->x = 0; break;
    case 'G': case '`': c->x = minimum(n-1,79); break;
    case 'd': c->y = minimum(low+n-1,high); break;
    case 'H': case 'f': c->y = minimum(low+n-1,high); c->x = minimum(second-1,79); break;
    case 'J':
        if (zero == 0) vga_terminal_erase(p,c->y*80+c->x,2000);
        if (zero == 1) vga_terminal_erase(p,0,c->y*80+c->x+1);
        if (zero == 2) vga_terminal_erase(p,0,2000);
        break;
    case 'K':
        if (zero == 0) vga_terminal_erase(p,c->y*80+c->x,(c->y+1)*80);
        if (zero == 1) vga_terminal_erase(p,c->y*80,c->y*80+c->x+1);
        if (zero == 2) vga_terminal_erase(p,c->y*80,(c->y+1)*80);
        break;
    case 'S': case 'T': vga_terminal_scroll(p,p->top,p->bottom,cmd == 'S' ? (int)n : -(int)n); break;
    case 'L': case 'M':
        if (c->y >= p->top && c->y <= p->bottom)
            vga_terminal_scroll(p,c->y,p->bottom,cmd == 'M' ? (int)n : -(int)n);
        break;
    case '@': case 'P': case 'X': {
        unsigned at = c->y*80+c->x;
        n = minimum(n,80-c->x);
        if (cmd == '@') {
            __builtin_memmove(p->cells+at+n,p->cells+at,(80-c->x-n)*sizeof(uint16_t));
            vga_terminal_erase(p,at,at+n);
        } else if (cmd == 'P') {
            __builtin_memmove(p->cells+at,p->cells+at+n,(80-c->x-n)*sizeof(uint16_t));
            vga_terminal_erase(p,(c->y+1)*80-n,(c->y+1)*80);
        } else vga_terminal_erase(p,at,at+n);
        break;
    }
    case 'r': {
        unsigned bottom = t->count > 1 && t->params[1] ? t->params[1] : 25;
        if (n < bottom && bottom <= 25) {
            p->top = n-1; p->bottom = bottom-1; c->x = 0; c->y = c->origin ? p->top : 0;
        }
        break;
    }
    case 's': p->saved = *c; break;
    case 'u': *c = p->saved; p->pending_wrap = false; break;
    case 'm': sgr(t); break;
    case 'g':
        if (!zero) t->tabs[c->x] = false;
        if (zero == 3) __builtin_memset(t->tabs,0,sizeof(t->tabs));
        break;
    case 'I': case 'Z':
        n = minimum(n,80);
        while (n--) {
            do {
                if ((cmd == 'I' && c->x == 79) || (cmd == 'Z' && !c->x)) break;
                if (cmd == 'I') ++c->x; else --c->x;
            } while (!t->tabs[c->x]);
        }
        break;
    case 'b':
        n = minimum(n,2000);
        if (t->last) while (n--) vga_terminal_scalar(t,t->last);
        break;
    default: break;
    }
}
