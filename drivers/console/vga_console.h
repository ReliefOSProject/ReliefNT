#ifndef RELIEFNT_VGA_CONSOLE_H
#define RELIEFNT_VGA_CONSOLE_H
#include <reliefnt/types.h>

/** @brief Parse exact console= tokens; an absent option leaves VGA disabled.
 * @param cmdline Nullable boot command line. @return True for last console=vga. */
bool vga_console_requested(const char *cmdline);
/** @brief Configure the opt-in hardware backend and replay retained boot output.
 * @param cmdline Nullable command line. @param legacy_boot Non-EFI handoff.
 * BSP initialization only; unsupported devices retain the original backend. */
void vga_console_configure(const char *cmdline, bool legacy_boot);
/** @brief Feed a kernel-log byte to the boot screen before service ownership.
 * @param ch Byte. Caller owns console serialization; no allocation. */
void vga_console_boot_byte(char ch);
/** @brief Consume output for one independent text VT, including hidden VTs.
 * @param number 1..6. @param s Bytes. @param len Byte count.
 * Execution-owned TTY path; never paints while the active VT is graphical. */
void vga_console_write(uint32_t number, const char *s, size_t len);
/** @brief Flush the active text grid and its hardware cursor; no-op in graphics. */
void vga_console_present(void);
/** @brief Restore the visible text grid after shared VT ownership changes. */
void vga_console_activate(void);
/** @brief Prepare hardware before changing VT ownership.
 * @param graphical Requested owner. @return Zero or an error preserving old mode. */
int vga_console_switch(bool graphical);
/** @brief Take over VGA for panic after IRQs/other CPUs have been stopped. */
void vga_console_panic_begin(void);
/** @brief Write literal emergency text without locks or allocations.
 * @param s Bytes. @param len Byte count. @param color Foreground VGA index. */
void vga_console_panic_write(const char *s, size_t len, uint8_t color);
#endif
