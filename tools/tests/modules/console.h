/* Test-only white-box composition of the separately built production units. */
#ifndef RELIEFNT_TEST_MODULE_CONSOLE_H
#define RELIEFNT_TEST_MODULE_CONSOLE_H
#include <reliefnt/console.h>
#include <reliefnt/serial.h>
#include <reliefnt/vga_text.h>
#include <reliefnt/pty.h>
#ifndef VGA_CONSOLE_TEST_HW
int vga_text_hw_init(bool legacy) { (void)legacy; return -19; }
int vga_text_hw_enter(void) { return 0; }
int vga_text_hw_leave(void) { return 0; }
void vga_text_hw_present(const uint16_t *c,unsigned x,unsigned y,bool v,unsigned s,bool r)
{ (void)c;(void)x;(void)y;(void)v;(void)s;(void)r; }
void vga_text_hw_panic(void) {}
#ifndef CONSOLE_TEST_REPLY
void pty_vt_reply(uint32_t n,const char *b,size_t l) {(void)n;(void)b;(void)l;}
#endif
#endif
/* These fixtures exercise console rendering and record formatting. Only the
 * standalone UART fixture supplies hardware port behavior. */
void serial_tx_record(const char *text, size_t length, bool important)
{
    (void)important;
    char terminated[1024];
    assert(length < sizeof(terminated));
    memcpy(terminated, text, length);
    terminated[length] = 0;
    serial_write(terminated);
}
#ifndef CONSOLE_TEST_PANIC_SERIAL
void serial_write_emergency(const char *text, size_t length)
{ serial_tx_record(text, length, true); }
void serial_panic_quiesce(void) {}
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
