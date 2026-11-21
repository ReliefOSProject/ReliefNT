/* SPDX-License-Identifier: Zlib */
/* Canonical DEFLATE decoding adapted from zlib's contrib/puff/puff.c.
 * Copyright (C) 2002-2013 Mark Adler. See third_party/zlib/LICENSE.
 * Modified for ReliefNT: bounded RFC 1950 framing, explicit error returns
 * instead of setjmp/longjmp, no scan mode or shared writable tables. */
#include <stddef.h>
#include <stdint.h>
#include <linux/errno.h>

#define MAX_BITS 15u
#define MAX_LENS 288u
#define MAX_DISTS 32u

struct inflate_input {
    const uint8_t *next, *end;
    unsigned bits, count;
};

struct inflate_tree {
    uint16_t count[MAX_BITS + 1];
    uint16_t symbols[MAX_LENS];
};

static int take(struct inflate_input *input, unsigned count)
{
    while (input->count < count) {
        if (input->next == input->end) return -EIO;
        input->bits |= (unsigned)*input->next++ << input->count;
        input->count += 8;
    }
    unsigned value = input->bits & ((1u << count) - 1u);
    input->bits >>= count;
    input->count -= count;
    return (int)value;
}

/* A positive result is an incomplete tree, zero is complete. */
static int build(struct inflate_tree *tree, const uint8_t *length, unsigned total)
{
    uint16_t offset[MAX_BITS + 1] = {0};
    for (unsigned i = 0; i <= MAX_BITS; ++i) tree->count[i] = 0;
    for (unsigned i = 0; i < total; ++i) ++tree->count[length[i]];
    int left = 1;
    for (unsigned len = 1; len <= MAX_BITS; ++len) {
        left = 2 * left - tree->count[len];
        if (left < 0) return -EIO;
        if (len < MAX_BITS) offset[len + 1] = offset[len] + tree->count[len];
    }
    for (unsigned i = 0; i < total; ++i)
        if (length[i]) tree->symbols[offset[length[i]]++] = i;
    return left;
}

static int decode(struct inflate_input *input, const struct inflate_tree *tree)
{
    unsigned code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= MAX_BITS; ++len) {
        int bit = take(input, 1);
        if (bit < 0) return bit;
        code = code * 2 + (unsigned)bit;
        unsigned count = tree->count[len];
        if (code >= first && code - first < count)
            return tree->symbols[index + code - first];
        index += count;
        first = (first + count) * 2;
    }
    return -EIO;
}

static void fixed(struct inflate_tree *literals, struct inflate_tree *distances)
{
    uint8_t length[MAX_LENS];
    for (unsigned i = 0; i < MAX_LENS; ++i)
        length[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    (void)build(literals, length, MAX_LENS);
    for (unsigned i = 0; i < MAX_DISTS; ++i) length[i] = 5;
    (void)build(distances, length, MAX_DISTS);
}

static int dynamic(struct inflate_input *input, struct inflate_tree *literals,
                   struct inflate_tree *distances)
{
    static const uint8_t order[] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5,
        11, 4, 12, 3, 13, 2, 14, 1, 15};
    struct inflate_tree codes;
    uint8_t code_length[19] = {0}, length[MAX_LENS + MAX_DISTS];
    int nl = take(input, 5), nd = take(input, 5), nc = take(input, 4);
    if (nl < 0 || nl > 29 || nd < 0 || nc < 0) return -EIO;
    nl += 257; nd += 1; nc += 4;
    for (int i = 0; i < nc; ++i) {
        int value = take(input, 3);
        if (value < 0) return value;
        code_length[order[i]] = (uint8_t)value;
    }
    if (build(&codes, code_length, 19) != 0) return -EIO;
    for (int i = 0; i < nl + nd;) {
        int symbol = decode(input, &codes);
        if (symbol < 0) return symbol;
        if (symbol < 16) {
            length[i++] = (uint8_t)symbol;
            continue;
        }
        if (symbol == 16 && i == 0) return -EIO;
        uint8_t value = symbol == 16 ? length[i - 1] : 0;
        int repeat = take(input, symbol == 16 ? 2 : symbol == 17 ? 3 : 7);
        if (repeat < 0) return repeat;
        repeat += symbol == 18 ? 11 : 3;
        if (repeat > nl + nd - i) return -EIO;
        while (repeat--) length[i++] = value;
    }
    if (!length[256]) return -EIO;
    int rc = build(literals, length, (unsigned)nl);
    if (rc < 0 || (rc && literals->count[1] != (unsigned)nl - literals->count[0]))
        return -EIO;
    rc = build(distances, length + nl, (unsigned)nd);
    if (rc < 0 || (rc && distances->count[1] != (unsigned)nd - distances->count[0]))
        return -EIO;
    return 0;
}

static int inflate_codes(struct inflate_input *input, const struct inflate_tree *literals,
                         const struct inflate_tree *distances, uint8_t *output,
                         size_t capacity, size_t *used, unsigned window)
{
    static const uint16_t length_base[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13,
        15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163,
        195, 227, 258};
    static const uint16_t dist_base[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25,
        33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049,
        3073, 4097, 6145, 8193, 12289, 16385, 24577};
    for (;;) {
        int symbol = decode(input, literals);
        if (symbol < 0) return symbol;
        if (symbol == 256) return 0;
        if (symbol < 256) {
            if (*used == capacity) return -EIO;
            output[(*used)++] = (uint8_t)symbol;
            continue;
        }
        unsigned index = (unsigned)symbol - 257u;
        if (index >= sizeof(length_base) / sizeof(*length_base)) return -EIO;
        int extra = take(input, index < 8 || index == 28 ? 0 : (index - 4) / 4);
        if (extra < 0) return extra;
        unsigned length = length_base[index] + (unsigned)extra;
        int distance = decode(input, distances);
        if (distance < 0 || distance >= (int)(sizeof(dist_base) / sizeof(*dist_base))) return -EIO;
        extra = take(input, distance < 4 ? 0 : (unsigned)distance / 2 - 1);
        if (extra < 0) return extra;
        unsigned back = dist_base[distance] + (unsigned)extra;
        if (back > *used || back > window || length > capacity - *used) return -EIO;
        for (unsigned i = 0; i < length; ++i) {
            output[*used] = output[*used - back];
            ++*used;
        }
    }
}

static int stored(struct inflate_input *input, uint8_t *output, size_t capacity, size_t *used)
{
    input->bits = input->count = 0; /* The stored block starts on a byte boundary. */
    int length = take(input, 16), inverse = take(input, 16);
    if (length < 0 || inverse < 0 || (length ^ inverse) != 65535 ||
        (size_t)length > capacity - *used || length > input->end - input->next)
        return -EIO;
    __builtin_memcpy(output + *used, input->next, (unsigned)length);
    *used += (unsigned)length;
    input->next += length;
    return 0;
}

static uint32_t adler32(const uint8_t *data, size_t size)
{
    uint32_t a = 1, b = 0;
    while (size) {
        size_t chunk = size > 5552 ? 5552 : size;
        size -= chunk;
        while (chunk--) { a += *data++; b += a; }
        a %= 65521u;
        b %= 65521u;
    }
    return (b << 16) | a;
}

int sqfs_inflate(const uint8_t *bytes, size_t size, uint8_t *output,
                 size_t capacity, uint32_t *length)
{
    if (size < 8 || (bytes[0] & 15u) != 8 || (bytes[0] >> 4) > 7 ||
        ((unsigned)bytes[0] * 256 + bytes[1]) % 31 || (bytes[1] & 32u)) return -EIO;
    unsigned window = 1u << (8 + (bytes[0] >> 4));
    const uint8_t *trailer = bytes + size - 4;
    struct inflate_input input = {.next = bytes + 2, .end = trailer};
    struct inflate_tree literals, distances;
    size_t used = 0;
    int last;
    do {
        last = take(&input, 1);
        int type = take(&input, 2);
        if (last < 0 || type < 0 || type == 3) return -EIO;
        int rc;
        if (type == 0) rc = stored(&input, output, capacity, &used);
        else {
            if (type == 1) fixed(&literals, &distances);
            else if ((rc = dynamic(&input, &literals, &distances)) != 0) return rc;
            rc = inflate_codes(&input, &literals, &distances, output, capacity, &used, window);
        }
        if (rc) return rc;
    } while (!last);
    uint32_t sum = (uint32_t)trailer[0] << 24 | (uint32_t)trailer[1] << 16 |
                   (uint32_t)trailer[2] << 8 | trailer[3];
    if (input.next != trailer || adler32(output, used) != sum) return -EIO;
    *length = (uint32_t)used;
    return 0;
}
