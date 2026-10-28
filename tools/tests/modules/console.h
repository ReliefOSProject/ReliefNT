/* Test-only white-box composition of the separately built production units. */
#ifndef RELIEFNT_TEST_MODULE_CONSOLE_H
#define RELIEFNT_TEST_MODULE_CONSOLE_H
#include <reliefnt/vga_text.h>
#include <reliefnt/pty.h>
#ifndef VGA_CONSOLE_TEST_HW
int vga_text_hw_init(bool legacy) { (void)legacy; return -19; }
int vga_text_hw_enter(void) { return 0; }
int vga_text_hw_leave(void) { return 0; }
void vga_text_hw_present(const uint16_t *c,unsigned x,unsigned y,bool v,unsigned s,bool r)
{ (void)c;(void)x;(void)y;(void)v;(void)s;(void)r; }
void vga_text_hw_panic(void) {}
void pty_vt_reply(uint32_t n,const char *b,size_t l) {(void)n;(void)b;(void)l;}
#endif
#include "../../../drivers/console/vga_terminal.c"
#include "../../../drivers/console/vga_ansi.c"
#include "../../../drivers/console/vga_console.c"
#include "../../../drivers/console/render.c"
#include "../../../drivers/console/vt.c"
#include "../../../drivers/console/log.c"
#include "../../../drivers/console/ansi.c"
#include "../../../drivers/console/core.c"
#include "../../../drivers/console/panic.c"
#endif
