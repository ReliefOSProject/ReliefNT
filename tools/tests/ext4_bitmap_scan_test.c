/* Compare allocation search against a byte/bit oracle independent of the
 * optimized scan, including reservation splitting and partial-word bounds. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define RELIEFNT_EXT4_ALLOC_TEST
#include "../../fs/ext_common/alloc.c"

static uint32_t random_state = 0x69e2871u;
static uint32_t random32(void)
{
    uint32_t x = random_state;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return random_state = x;
}

static struct ext4_scan_result empty_result(void)
{
    return (struct ext4_scan_result){.hit = -1, .above = -1, .below = -1,
        .best_start = -1};
}

static void oracle_run(struct ext4_scan_result *out, uint32_t begin, uint32_t end,
                        uint64_t hint, uint32_t need)
{
    if (begin == end) return;
    if (end - begin > out->best_len) {
        out->best_len = end - begin; out->best_start = begin;
    }
    if (end - begin < need) return;
    if (begin <= hint && hint + need <= end) out->hit = hint;
    else if (begin > hint) { if (out->above < 0) out->above = begin; }
    else out->below = begin;
}

static struct ext4_scan_result oracle(const uint8_t *bitmap, uint64_t first,
    uint32_t lo, uint32_t hi, uint64_t goal, uint64_t reserved_end,
    uint64_t hint, uint32_t need, bool owner)
{
    struct ext4_scan_result out = empty_result();
    uint32_t begin = lo;
    for (uint32_t bit = lo; bit < hi; ++bit) {
        bool free = !(bitmap[bit / 8] & (1u << (bit % 8)));
        if (!owner && first + bit >= goal && first + bit < reserved_end) free = false;
        if (!free) {
            oracle_run(&out, begin, bit, hint, need); begin = bit + 1;
        }
    }
    oracle_run(&out, begin, hi, hint, need);
    return out;
}

static void assert_equal(struct ext4_scan_result a, struct ext4_scan_result b)
{
    assert(a.hit == b.hit && a.above == b.above && a.below == b.below);
    assert(a.best_start == b.best_start && a.best_len == b.best_len);
}

static void partial_words(void)
{
    for (uint32_t hi = 1; hi <= 257; ++hi) {
        size_t bytes = (hi + 7) / 8;
        /* Last byte touches ASan's allocation boundary; the bitmap starts
         * unaligned, so an unconditional last-word load would be detected. */
        uint8_t *allocation = malloc(bytes + 1), *bitmap = allocation + 1;
        assert(allocation);
        for (unsigned pattern = 0; pattern < 5; ++pattern) {
            for (size_t byte = 0; byte < bytes; ++byte)
                bitmap[byte] = pattern == 0 ? 0 : pattern == 1 ? 255 :
                    pattern == 2 ? 0x55 : pattern == 3 ? 0xaa : random32();
            for (uint32_t lo = 0; lo <= hi; ++lo) for (unsigned set = 0; set < 2; ++set) {
                uint32_t wanted = lo;
                while (wanted < hi && !!(bitmap[wanted / 8] & (1u << (wanted % 8))) != set)
                    ++wanted;
                assert(ext4_bitmap_next(bitmap, lo, hi, set) == wanted);
            }
        }
        free(allocation);
    }
}

int main(void)
{
    partial_words();
    struct storage_volume volume = {0};
    uint8_t bitmap[4096];
    for (unsigned trial = 0; trial < 8000; ++trial) {
        uint32_t hi = 1 + random32() % 32768;
        uint32_t lo = random32() % hi;
        uint64_t first = trial % 2 ? UINT64_MAX - 65536 : 9000;
        for (unsigned byte = 0; byte < sizeof(bitmap); ++byte) {
            uint32_t value = random32();
            switch (trial % 6) {
            case 0: bitmap[byte] = 0; break;
            case 1: bitmap[byte] = 255; break;
            case 2: bitmap[byte] = value % 31 == 0 ? 1u << (value % 8) : 0; break;
            case 3: bitmap[byte] = value % 31 == 0 ? ~(1u << (value % 8)) : 255; break;
            case 4: bitmap[byte] = 0x55; break;
            default: bitmap[byte] = value; break;
            }
        }
        uint64_t goal = first - 100 + random32() % (hi + 200);
        uint64_t reserved_end = goal + random32() % 1000;
        if (trial % 7 == 0) reserved_end = 0;
        volume.ext4.next_goal_block = goal; volume.ext4.reserved_window_end = reserved_end;
        uint64_t hint = random32() % (hi + 17);
        uint32_t need = 1 + random32() % 1025;
        bool owner = random32() & 1;
        struct ext4_scan_result actual = empty_result();
        ext4_scan_bits(&volume, bitmap, first, lo, hi, hint, need, owner, &actual);
        assert_equal(actual, oracle(bitmap, first, lo, hi, goal, reserved_end, hint, need, owner));
    }
    memset(bitmap, 255, sizeof(bitmap));
    for (unsigned bit = 32700; bit < 32768; ++bit) bitmap[bit / 8] &= ~(1u << (bit % 8));
    struct ext4_scan_result actual = empty_result();
    ext4_alloc_scan_words = ext4_alloc_scan_bit_tests = 0;
    ext4_scan_bits(&volume, bitmap, 0, 0, 32768, 32700, 32, true, &actual);
    assert(actual.hit == 32700 && actual.best_len == 68 && ext4_alloc_scan_words == 512);
    assert(ext4_alloc_scan_bit_tests == 64);
    printf("{\"bitmap_bits\":32768,\"baseline_bit_tests\":32768,\"scan_word_loads\":%llu,\"mixed_word_bit_tests\":%llu}\n",
           (unsigned long long)ext4_alloc_scan_words,
           (unsigned long long)ext4_alloc_scan_bit_tests);
    puts("PASS bitmap partial words and 8000 independent allocation/reservation models");
    return 0;
}
