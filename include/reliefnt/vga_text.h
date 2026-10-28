#ifndef RELIEFNT_VGA_TEXT_H
#define RELIEFNT_VGA_TEXT_H
#include <reliefnt/types.h>

/** @brief Probe a supported legacy VGA device, map its apertures and enter text.
 * @param legacy_boot True for a non-EFI handoff when no framebuffer exists.
 * @return Zero on success, negative ENODEV/EIO when safe takeover is unavailable.
 * BSP boot context, before other CPUs/user display clients; no allocation. */
int vga_text_hw_init(bool legacy_boot);
/** @brief Save/disable graphics and program the 80x25 VGA text scanout.
 * @return Zero or a negative error, leaving graphics intact if quiescence fails.
 * Execution-owned VT context; caller serializes console state. */
int vga_text_hw_enter(void);
/** @brief Restore the graphic backend and its previous hardware mode.
 * @return Zero on success or negative EIO; no framebuffer console rendering.
 * Execution-owned VT context, serialized with the VGA terminal. */
int vga_text_hw_leave(void);
/** @brief Present exactly 2000 validated cells and program the hardware cursor.
 * @param cells Readable 80x25 grid. @param x Cursor column below 80.
 * @param y Cursor row below 25. @param visible Cursor visibility.
 * @param style DEC cursor style; block or underline, bar degrades to underline.
 * @param reverse Reverse all attribute foreground/background nibbles.
 * @return None. Caller owns VGA state; no writes while graphics owns scanout. */
void vga_text_hw_present(const uint16_t *cells, unsigned x, unsigned y,
                         bool visible, unsigned style, bool reverse);
/** @brief Force text scanout for a fatal owner with IRQs/other CPUs stopped.
 * @return None. Bounded port writes only; no locks, allocation or GPU waits. */
void vga_text_hw_panic(void);
#endif
