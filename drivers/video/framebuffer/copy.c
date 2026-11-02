/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Upload exact 32-bit pixels with device-memory-sized stores.
 * @param destination Borrowed writable, four-byte-aligned pixel aperture.
 * @param source Borrowed readable, nonoverlapping bytes; may be unaligned.
 * @param pixels Exact number of pixels; zero accesses neither extent.
 * Task/IRQ context with both mappings retained and rendering serialized by
 * the caller. Uses no SIMD state, allocation or framebuffer readback. Unlike
 * the RAM REP MOVSB primitive, each store transfers a complete pixel into
 * UC or WC video memory. Caller orders WC writes before publishing damage.
 * No padding or pixels outside the extent are touched.
 */
void framebuffer_copy_io32(void *destination, const void *source, size_t pixels)
{
    __asm__ volatile("cld; rep movsl"
                     : "+D"(destination), "+S"(source), "+c"(pixels)
                     : : "memory", "cc");
}
