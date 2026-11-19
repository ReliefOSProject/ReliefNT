/* SPDX-License-Identifier: Apache-2.0 */
#include "../../fs/exfat/internal.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Independent on-disk boot regions; no filesystem parser constructs them. */
static uint8_t media[24u * 512u];
static uint64_t failed_sector = UINT64_MAX;
static int read_error;
static unsigned checks;

/** @brief Read the exact emulated sectors with transport retry flags checked.
 * @param first First media-relative sector. @param count Complete sector count.
 * @param buffer Writable current output. @return Zero or injected transport errno.
 */
int storage_read_sectors(uint64_t first, uint32_t count, void *buffer)
{
    assert(first < 24u && count && count <= 24u - first);
    assert(!storage_io_async_context && storage_io_write_started);
    if (first <= failed_sector && failed_sector - first < count) return read_error;
    memcpy(buffer, media + first * 512u, count * 512u);
    return 0;
}

/** @brief Silence expected corruption diagnostics at the host console boundary.
 * @param level Unused level. @param format Unused diagnostic format.
 */
void console_log(enum console_log_level level, const char *format, ...)
{ (void)level; (void)format; }

/** @brief Encode one independently computed little-endian wire value.
 * @param target Required four-byte output. @param value Native value to encode.
 */
static void put32(uint8_t *target, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) target[i] = (uint8_t)(value >> (8u * i));
}

/** @brief Build the reference checksum using the exFAT specification's arithmetic.
 * @param base Sector offset of one twelve-sector boot region.
 */
static void checksum(unsigned base)
{
    uint32_t value = 0;
    for (unsigned index = 0; index < 11u * 512u; ++index) {
        if (index == 106 || index == 107 || index == 112) continue;
        value = (value / 2u) + (value % 2u ? UINT32_C(0x80000000) : 0u) +
                media[base * 512u + index];
    }
    for (unsigned offset = 0; offset < 512; offset += 4)
        put32(media + (base + 11u) * 512u + offset, value);
}

/** @brief Create standard boot signatures and independently checksummed regions.
 * @param base First sector, either main zero or backup twelve.
 */
static void initialize(unsigned base)
{
    uint8_t *boot = media + base * 512u;
    memset(boot, 0, 12u * 512u);
    memcpy(boot + 3, "EXFAT   ", 8);
    boot[510] = 0x55; boot[511] = 0xaa;
    for (unsigned sector = 1; sector <= 8; ++sector)
        put32(boot + sector * 512u + 508, UINT32_C(0xaa550000));
    checksum(base);
}

/** @brief Assert validation result and exact restoration of async call context.
 * @param base First main/backup sector. @param expected Required errno or success.
 */
static void expect(unsigned base, int expected)
{
    uint8_t boot[512]; memset(boot, 0xa5, sizeof(boot));
    storage_io_async_context = true; storage_io_write_started = false;
    assert(exfat_boot_region_valid(base, boot) == expected);
    assert(storage_io_async_context && !storage_io_write_started);
    if (!expected) assert(!memcmp(boot, media + base * 512u, sizeof(boot)));
    ++checks;
}

/** @brief Exercise valid OEM parameters and independent integrity failures.
 * @return Zero after both main/backup regions preserve all validation boundaries.
 */
int main(void)
{
    static const uint8_t flash_guid[16] = {
        0x46, 0x7e, 0x0c, 0x0a, 0x99, 0x33, 0x21, 0x40,
        0x90, 0xc8, 0xfa, 0x6d, 0x38, 0x9c, 0x4b, 0xa2
    };
    for (unsigned base = 0; base <= 12; base += 12) {
        initialize(base); expect(base, 0); /* Ten Null Parameters. */
        uint8_t *oem = media + (base + 9u) * 512u;
        memcpy(oem, flash_guid, sizeof(flash_guid));
        put32(oem + 16, 131072); put32(oem + 20, 4096);
        checksum(base); expect(base, 0); /* Standard Flash Parameters GUID. */
        /* Unknown GUIDs/custom fields and the last byte of Parameters[9] are
         * opaque. Full boot checksums, rather than guessed GUID contents,
         * determine whether these manufacturer bytes are intact. */
        for (unsigned i = 48; i < 480; ++i) oem[i] = (uint8_t)(i * 13u + 71u);
        oem[479] = 0xa7;
        checksum(base); expect(base, 0);
        oem[100] ^= 1; expect(base, -5); oem[100] ^= 1;
        media[(base + 11u) * 512u + 508] ^= 1;
        expect(base, -5); checksum(base);
        /* Readers preserve and do not interpret the reserved OEM tail. The
         * widely deployed exfatprogs 1.1.3 formatter fills all 512 OEM bytes
         * with FF; the complete sector still participates in its checksum. */
        memset(oem, 0xff, 512); checksum(base); expect(base, 0);
        for (unsigned byte = 480; byte <= 511; byte += 31) {
            oem[byte] ^= 1; expect(base, -5);
            checksum(base); expect(base, 0);
        }
        for (unsigned byte = 0; byte <= 511; byte += 511) {
            media[(base + 10u) * 512u + byte] = 1;
            checksum(base); expect(base, -5);
            media[(base + 10u) * 512u + byte] = 0; checksum(base);
        }
        for (unsigned sector = 1; sector <= 8; ++sector)
            for (unsigned byte = 508; byte <= 511; ++byte) {
                media[(base + sector) * 512u + byte] ^= 1;
                checksum(base); expect(base, -5);
                media[(base + sector) * 512u + byte] ^= 1; checksum(base);
            }
        failed_sector = base + 9u;
        read_error = -5; expect(base, -5);
        read_error = -11; expect(base, -11);
        failed_sector = UINT64_MAX;
        expect(base, 0);
    }
    printf("PASS exFAT boot: %u main/backup checks; standard/opaque OEM and tail, reserved sector, full signatures, checksum and I/O failures\n", checks);
    return 0;
}
