/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"


/**
 * @brief Report whether the reserved runtime-root filesystem instance is ready.
 * @return True after successful native root discovery, false before or on failure.
 * Execution-owned observation only; no I/O, allocation or ownership transfer.
 */
bool storage_ready(void)
{
    return g_volumes[0].ready;
}

/**
 * @brief Format a bounded diagnostic prefix and unsigned disk/partition number.
 * @param out Writable output, ignored if NULL. @param cap Byte capacity including NUL.
 * @param prefix Borrowed optional literal prefix.
 * @param value Unsigned diagnostic number. @param partition Negative to omit the
 * partition suffix, otherwise zero-based index rendered as p(index+1).
 * @return None. Truncates with a terminator whenever out/cap permit; no allocation,
 * I/O or reference transfer. Caller owns its buffer; no shared mutable state.
 */
void storage_format_u32(char *out, uint32_t cap, const char *prefix,
                               uint32_t value, int partition)
{
    char digits[11];
    uint32_t n = 0;
    uint32_t pos = 0;
    if (!out || cap == 0) return;
    if (prefix) {
        while (prefix[pos] && pos + 1u < cap) {
            out[pos] = prefix[pos];
            ++pos;
        }
    }
    do {
        digits[n++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value && n < sizeof(digits));
    while (n && pos + 1u < cap) out[pos++] = digits[--n];
    if (partition >= 0 && pos + 2u < cap) {
        out[pos++] = 'p';
        value = (uint32_t)partition + 1u;
        n = 0;
        do {
            digits[n++] = (char)('0' + value % 10u);
            value /= 10u;
        } while (value && n < sizeof(digits));
        while (n && pos + 1u < cap) out[pos++] = digits[--n];
    }
    out[pos] = 0;
}

/**
 * @brief Reports the filesystem implementation backing the runtime root at runtime.
 * @return A static lowercase name suitable for boot diagnostics.
 */
const char *storage_root_filesystem_name(void)
{
    switch (g_volumes[0].filesystem) {
    case STORAGE_FILESYSTEM_TMPFS:
        return "tmpfs";
    case STORAGE_FILESYSTEM_EXFAT:
        return "exfat";
    case STORAGE_FILESYSTEM_EXT2:
        return "ext2";
    case STORAGE_FILESYSTEM_EXT4:
        return "ext4";
    case STORAGE_FILESYSTEM_FAT32:
        return "fat32";
    case STORAGE_FILESYSTEM_ISO9660:
        return "iso9660";
    default:
        return "none";
    }
}

/**
 * @brief Report whether the current runtime root is the installer RAM image.
 * @return True for an active reserved installer root, false for disk/absent root.
 * Execution-owned observation only; retains no pointer or module reference.
 */
bool storage_installer_root_active(void)
{
    return g_installer_root_active != 0;
}
