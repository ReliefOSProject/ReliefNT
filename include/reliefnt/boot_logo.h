/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_BOOT_LOGO_H
#define RELIEFNT_BOOT_LOGO_H

#include <reliefnt/types.h>
struct framebuffer;

/**
 * @brief Paint embedded CPU logos above the boot console, without allocation.
 * @param target Validated framebuffer surface, or NULL to skip drawing.
 * @param cpu_count Logical CPUs discovered by the boot topology scan.
 * @return Font-aligned height reserved above text, or zero if unavailable.
 *
 * Called by the BSP while the boot console owns the display. Logos wrap and
 * shrink when necessary; at least four text rows remain. An exceptionally
 * small screen may fit fewer logos. Never changes the framebuffer descriptor.
 */
uint32_t boot_logo_draw(const struct framebuffer *target, uint32_t cpu_count);

#endif
