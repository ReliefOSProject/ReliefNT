#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../drivers/console/vga_terminal.h"

static struct vga_terminal t;
static void feed(const char *s) { while (*s) vga_terminal_feed(&t, (unsigned char)*s++); }
static unsigned ch(unsigned x, unsigned y) { return t.page[t.alternate].cells[y * 80 + x] & 255; }
static unsigned attr(unsigned x, unsigned y) { return t.page[t.alternate].cells[y * 80 + x] >> 8; }

int main(void)
{
    vga_terminal_reset(&t);
    assert(t.page[0].cursor.fg == 7 && t.page[0].bottom == 24 && t.visible);
    feed("abc\rX\tZ\bQ");
    assert(ch(0,0)=='X' && ch(1,0)=='b' && ch(8,0)=='Q');
    feed("\033[2J\033[H\033[31;44mR\033[0mN");
    assert(ch(0,0)=='R' && attr(0,0)==0x14 && attr(1,0)==7);
    feed("\033[93;104mY\033[7mI\033[27mO\033[8mH\033[28mV");
    assert(attr(2,0)==0x9e && attr(3,0)==0xe9 && attr(5,0)==0x99);
    feed("\033[38;5;196;48;2;0;0;255mC");
    assert(attr(7,0)==0x14); /* nearest physical VGA shades */
    feed("\033[0m\033[3;5Hhello\033[3;7H\033[2@X");
    assert(ch(4,2)=='h' && ch(6,2)=='X' && ch(8,2)=='l');
    feed("\033[3;7H\033[2P");
    assert(ch(6,2)=='l' && ch(8,2)=='o');
    feed("\033[2X");
    assert(ch(6,2)==' ' && ch(7,2)==' ' && ch(8,2)=='o');
    feed("\033[2K");
    for(unsigned x=0;x<80;++x) assert(ch(x,2)==' ');
    feed("\033[2J\033[1;1HTOP\033[25;1HBOTTOM\033[2;4r\033[4;1HLAST\n");
    assert(ch(0,0)=='T' && ch(0,24)=='B' && ch(0,2)=='L' && ch(0,3)==' ');
    feed("\033[2;1H\033M");
    assert(ch(0,1)==' ' && ch(0,3)=='L');
    feed("\033[?6h\033[H");
    assert(t.page[0].cursor.y==1);
    feed("\033[999B"); assert(t.page[0].cursor.y==3);
    feed("\033[?6l\033[r\033[25;80HX");
    assert(ch(79,24)=='X' && t.page[0].pending_wrap);
    feed("\rY"); assert(ch(0,24)=='Y' && !t.page[0].pending_wrap);
    feed("\033[1;80HXZ");
    assert(ch(79,0)=='X' && ch(0,1)=='Z');
    feed("\033[?7l\033[1;80HAB"); assert(ch(79,0)=='B' && ch(0,1)=='Z');
    feed("\033[?7h\033[6;7H\033[32m\0337\033[H\033[31m\0338S");
    assert(ch(6,5)=='S' && attr(6,5)==2);
    feed("\033[?1049hALT\033[?1049h!");
    assert(t.alternate && ch(0,0)=='A' && ch(3,0)=='!');
    feed("\033[?1049lP");
    assert(!t.alternate && ch(6,5)=='S' && ch(7,5)=='P');
    feed("\033[?25l"); assert(!t.visible);
    feed("\033[?25h"); assert(t.visible);
    feed("\033[2J\033[H\033]0;hidden-title\033\\OK\033Pignored\033\\!\033[1;4;5mX");
    assert(ch(0,0)=='O' && ch(2,0)=='!' && ch(3,0)=='X');
    assert(attr(3,0)==2); /* unsupported font effects do not leak digits */
    feed("\033[999999999999999999999;9999999999H.");
    assert(ch(79,24)=='.');
    feed("\033[H\033[1;2;3;4;5;6;7;8;9;10;11;12;13;14;15;16;17;18;19;20;21;22;23;24;25;26;27;28;29;30;31;32;33mQ");
    assert(ch(0,0)=='Q'); /* overlong CSI is discarded as a whole */
    feed("\033[H\033[6n");
    assert(t.reply_length==6 && !memcmp(t.reply,"\033[1;1R",6));
    t.reply_length=0; feed("\033[5n\033[c");
    assert(t.reply_length==9 && !memcmp(t.reply,"\033[0n\033[?6c",9));
    feed("\033c\033(0lqk\033(B\xe4\xb8\xad");
    assert(ch(0,0)!='l' && ch(1,0)!='q' && ch(2,0)!='k' && ch(3,0)=='?');
    /* Fragmented UTF-8 and CSI persist across arbitrary write boundaries. */
    vga_terminal_reset(&t);
    feed("\033[3"); feed("1mR\xc3"); feed("\xa9");
    assert(attr(0,0)==4 && ch(1,0)!='?');
    for(unsigned i=0;i<200000;++i) vga_terminal_feed(&t,(unsigned char)(i*73));
    assert(t.page[t.alternate].cursor.x<80 && t.page[t.alternate].cursor.y<25);
    puts("PASS VGA terminal: colors, edits, margins, wrap, alternate screens, fragmented ANSI/UTF-8, queries and fuzz bounds");
}
