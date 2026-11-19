/* SPDX-License-Identifier: Apache-2.0 */
/* Real mount, inode, cache and block paths from the existing image fixture. */
#define main storage_fixture_unused_main
#include "storage_mkdir_mount_test.c"
#undef main

/** @brief Decode the fixture oracle's independent little-endian geometry value.
 * @param bytes Readable four-byte wire field. @return Native unsigned value.
 */
static uint32_t wire32(const uint8_t *bytes)
{ return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24; }

/** @brief Validate a new root-directory snapshot through the actual mount parser.
 * @param expected Required success/error result. No cached root bytes survive.
 */
static void expect_mount(int expected)
{
    exfat_cache_invalidate();
    assert(exfat_mount() == expected);
}

/** @brief Test opaque label reserved bytes without accepting invalid characters.
 * @param argc Two arguments. @param argv Program and unmodified formatter image.
 * @return Zero after all mount assertions and an exact metadata restoration.
 */
int main(int argc, char **argv)
{
    assert(argc == 2);
    FILE *file = fopen(argv[1], "rb"); assert(file);
    assert(!fseek(file, 0, SEEK_END)); long size = ftell(file); assert(size > 0); rewind(file);
    uint8_t *image = malloc((size_t)size);
    assert(image && fread(image, 1, (size_t)size, file) == (size_t)size && !fclose(file));
    g_active_volume = &g_volumes[0];
    *g_active_volume = (struct storage_volume){.volume_id = 0, .exfat_sector_count = size / 512u};
    test_memory_bind(g_active_volume, image, (uint64_t)size);
    expect_mount(0); /* The external formatter image is accepted as supplied. */
    g_storage.fs_ops = &storage_exfat_ops;
    uint32_t heap = wire32(image + 88), root = wire32(image + 96);
    assert(root >= 2 && image[109] <= 8);
    uint64_t root_offset = (uint64_t)heap * 512u + (uint64_t)(root - 2u) * (512u << image[109]);
    assert(root_offset < (uint64_t)size && (uint64_t)size - root_offset >= 512u);
    uint8_t *label = image + root_offset;
    assert(label[0] == 0x83);
    uint8_t original[512]; memcpy(original, label, sizeof(original));
    label[1] = 0; memset(label + 2, 0, 22);
    for (unsigned i = 24; i < 32; ++i) label[i] = (uint8_t)(0x80u + i);
    expect_mount(0); /* Reserved bytes 24..31 are not label characters. */
    label[2] = 'X'; expect_mount(-5); label[2] = 0;
    label[1] = 12; expect_mount(-5); label[1] = 0;
    unsigned end = 32;
    while (end < 512 && label[end]) end += 32;
    assert(end < 512 && !label[end]);
    memcpy(label + end, label, 32); expect_mount(-5); /* Duplicate labels fail. */
    memcpy(label + end, original + end, 32);
    label[1] = 11;
    for (unsigned i = 0; i < 11; ++i) {
        label[2 + i * 2] = (uint8_t)('A' + i); label[3 + i * 2] = 0;
    }
    expect_mount(0); /* All eleven characters fit before the reserved boundary. */
    memcpy(label, original, sizeof(original)); expect_mount(0);
    assert(!memcmp(label, original, sizeof(original)));
    test_memory_release(g_active_volume);
    puts("PASS exFAT mount: 7 real-parser checks; opaque label reserved bytes, invalid count/padding, duplicate labels and full eleven-character labels");
    return 0;
}
