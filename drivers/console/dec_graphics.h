#ifndef RELIEFNT_CONSOLE_DEC_GRAPHICS_H
#define RELIEFNT_CONSOLE_DEC_GRAPHICS_H
#include <reliefnt/types.h>

/** @brief Translate the VT100 special-graphics charset for either text backend.
 * @param codepoint Decoded Unicode scalar. @return Mapped scalar or the input.
 */
static inline uint32_t console_dec_graphic(uint32_t codepoint)
{
    static const uint16_t table[] = {
        0x25c6,0x2592,0x2409,0x240c,0x240d,0x240a,0x00b0,0x00b1,
        0x2424,0x240b,0x2518,0x2510,0x250c,0x2514,0x253c,0x23ba,
        0x23bb,0x2500,0x23bc,0x23bd,0x251c,0x2524,0x2534,0x252c,
        0x2502,0x2264,0x2265,0x03c0,0x2260,0x00a3,0x00b7,
    };
    return codepoint >= 0x60 && codepoint <= 0x7e ? table[codepoint - 0x60] : codepoint;
}
#endif
