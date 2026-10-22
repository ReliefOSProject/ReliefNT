/* Linux root=PARTUUID selection, shared with the existing devfs UUID resolver. */
#include "internal.h"

static char requested_text[37];
static uint8_t requested_guid[16];
static int request_state;
static bool filter_active;

/** @brief Decode one hexadecimal byte nibble, or reject non-UUID characters. */
static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

/** @brief Decode a complete nonzero UUID without publishing partial state. */
static int parse_identity(const char *text, size_t length)
{
    if (length != 36) return -22;
    char canonical[37];
    uint8_t decoded[16];
    static const uint8_t order[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
    unsigned byte = 0, nonzero = 0;
    for (unsigned i = 0; i < 36;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return -22;
            canonical[i++] = '-';
            continue;
        }
        int high = hex_value(text[i]), low = hex_value(text[i + 1]);
        if (high < 0 || low < 0 || byte >= 16) return -22;
        static const char digits[] = "0123456789abcdef";
        canonical[i] = digits[high]; canonical[i + 1] = digits[low];
        decoded[order[byte++]] = (uint8_t)((high << 4) | low);
        nonzero |= (unsigned)((high << 4) | low);
        i += 2;
    }
    if (byte != 16 || !nonzero) return -22;
    canonical[36] = 0;
    __builtin_memcpy(requested_text, canonical, sizeof(canonical));
    __builtin_memcpy(requested_guid, decoded, sizeof(decoded));
    return 1;
}

/**
 * @brief Parse the last root argument before disk discovery, without allocation.
 * Legacy root=/ remains automatic. Unsupported/malformed explicit requests are
 * retained as errors so discovery cannot silently select a different root.
 * BSP boot context; no concurrent VFS users or retained command-line pointer.
 */
void storage_boot_root_configure(const char *cmdline)
{
    request_state = 0;
    filter_active = false;
    __builtin_memset(requested_text, 0, sizeof(requested_text));
    __builtin_memset(requested_guid, 0, sizeof(requested_guid));
    if (!cmdline) return;
    const char *cursor = cmdline;
    while (*cursor) {
        while (*cursor && (unsigned char)*cursor <= ' ') ++cursor;
        const char *start = cursor;
        while (*cursor && (unsigned char)*cursor > ' ') ++cursor;
        size_t length = (size_t)(cursor - start);
        if (length < 5 || __builtin_memcmp(start, "root=", 5)) continue;
        start += 5; length -= 5;
        if (length == 1 && *start == '/') request_state = 0;
        else if (length >= 9 && !__builtin_memcmp(start, "PARTUUID=", 9))
            request_state = parse_identity(start + 9, length - 9);
        else request_state = -22;
    }
}

/** @brief Tell discovery to defer automatic mounting for any explicit request. */
bool storage_boot_root_requested(void) { return request_state != 0; }

/** @brief Use the validated, ambiguity-rejecting devfs resolver after disk sorting. */
int storage_boot_root_select(void)
{
    if (request_state != 1) return -22;
    char path[64], target[48];
    __builtin_memcpy(path, "/dev/disk/by-partuuid/", 22);
    __builtin_memcpy(path + 22, requested_text, sizeof(requested_text));
    int result = storage_devlink_target(path, target);
    if (result) return result;
    if (__builtin_strncmp(target, "../../", 6)) return -22;
    uint32_t disk;
    int32_t partition;
    result = storage_parse_block_name(target + 6, &disk, &partition);
    if (result) return result;
    if (partition < 0 || disk >= g_install_disk_count || !g_install_disks[disk].present) return -22;
    return (int)disk;
}

/** @brief Restrict only the selected boot mount, preserving later management semantics. */
void storage_boot_root_filter_enable(bool enabled) { filter_active = enabled; }

/** @brief Match the requested partition while a scoped root mount is in progress. */
bool storage_boot_root_accept_guid(const uint8_t guid[16])
{
    return !filter_active || (request_state == 1 && !__builtin_memcmp(guid, requested_guid, 16));
}
