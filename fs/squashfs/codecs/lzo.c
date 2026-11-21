/* SPDX-License-Identifier: MIT
 * LZO1X decoder adapted from lzokay by the AxioDL contributors.
 * See lzokay.LICENSE for attribution and terms.
 */
#include "../internal.h"
#include "interface.h"

static int extended_length(const uint8_t *src, uint32_t size, uint32_t *input,
                           uint32_t *length)
{
    while (*input < size) {
        unsigned byte = src[(*input)++];
        if (byte) {
            *length += byte;
            return 0;
        }
        /* No SquashFS block can expand beyond 1 MiB. */
        if (*length >= (1u << 20)) return -EIO;
        *length += 255;
    }
    return -EIO;
}

static int copy_literals(const uint8_t *src, uint32_t size, uint32_t *input,
                         uint8_t *dst, uint32_t capacity, uint32_t *output, uint32_t length)
{
    if (length > size - *input || length > capacity - *output) return -EIO;
    __builtin_memcpy(dst + *output, src + *input, length);
    *input += length;
    *output += length;
    return 0;
}

int sqfs_lzo_decode(const uint8_t *src, uint32_t stored, uint8_t *dst, uint32_t capacity,
                    uint32_t *size)
{
    if (stored < 3) return -EIO;
    uint32_t input = 0, output = 0, state = 0;
    if (src[0] > 17) {
        uint32_t literals = src[input++] - 17;
        int rc = copy_literals(src, stored, &input, dst, capacity, &output, literals);
        if (rc) return rc;
        state = literals < 4 ? literals : 4;
    }
    while (input < stored) {
        unsigned instruction = src[input++];
        uint32_t length, distance, literals;
        if (instruction >= 64) {
            if (input == stored) return -EIO;
            length = (instruction >> 5) + 1;
            distance = 1 + ((instruction >> 2) & 7) + ((uint32_t)src[input++] << 3);
            literals = instruction & 3;
        } else if (instruction >= 16) {
            bool far = instruction < 32;
            length = (instruction & (far ? 7u : 31u)) + 2;
            if (length == 2) {
                length += far ? 7 : 31;
                int rc = extended_length(src, stored, &input, &length);
                if (rc) return rc;
            }
            if (stored - input < 2) return -EIO;
            uint32_t field = sqfs_u16(src + input);
            input += 2;
            distance = field >> 2;
            literals = field & 3;
            if (far) {
                distance += (instruction & 8u) << 11;
                if (!distance) {
                    if (input != stored || length != 3 || literals) return -EIO;
                    *size = output;
                    return 0;
                }
                distance += 16384;
            } else {
                ++distance;
            }
        } else if (!state) {
            length = instruction + 3;
            if (length == 3) {
                length += 15;
                int rc = extended_length(src, stored, &input, &length);
                if (rc) return rc;
            }
            int rc = copy_literals(src, stored, &input, dst, capacity, &output, length);
            if (rc) return rc;
            state = 4;
            continue;
        } else {
            if (input == stored) return -EIO;
            distance = 1 + (instruction >> 2) + ((uint32_t)src[input++] << 2);
            length = 2;
            if (state == 4) {
                distance += 2048;
                length = 3;
            }
            literals = instruction & 3;
        }
        if (distance > output || length > capacity - output) return -EIO;
        if (length <= distance) {
            __builtin_memcpy(dst + output, dst + output - distance, length);
            output += length;
        } else {
            for (; length; --length, ++output) dst[output] = dst[output - distance];
        }
        int rc = copy_literals(src, stored, &input, dst, capacity, &output, literals);
        if (rc) return rc;
        state = literals;
    }
    return -EIO;
}
