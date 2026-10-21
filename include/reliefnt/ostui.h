/*
 * ReliefOS kernel TUI interface.
 * Provides a small VT100/xterm renderer for framebuffer and serial output.
 */
#ifndef RELIEFNT_OSTUI_H
#define RELIEFNT_OSTUI_H

#include <reliefnt/types.h>

void ostui_init(void);
void ostui_clear(void);
void ostui_write(const char *text);
void ostui_write_u64(uint64_t value);
int ostui_poll_key(void);

#endif
