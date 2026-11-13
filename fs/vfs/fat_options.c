/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/fat_options.h>
#include <linux/errno.h>

/** @brief Decode one delimited unsigned decimal ID or octal permission mask.
 * @param start First digit, required. @param end Exclusive token end.
 * @param base Numeric base, eight or ten. @param limit Largest accepted value.
 * @param out Required result, unchanged on invalid input.
 * @return Zero or -EINVAL. Pure bounded parser without allocation or I/O.
 */
static int parse_value(const char *start, const char *end, uint32_t base,
                       uint32_t limit, uint32_t *out)
{
    if (start == end) return -EINVAL;
    uint32_t value = 0;
    for (const char *p = start; p != end; ++p) {
        uint32_t digit = (unsigned char)*p - '0';
        if (digit >= base || value > (limit - digit) / base) return -EINVAL;
        value = value * base + digit;
    }
    *out = value;
    return 0;
}

/** @brief Validate the complete FAT ownership/mask option set before commit.
 * @param text Optional comma-separated Linux option string.
 * @param out Required result, unchanged on failure.
 * @return Zero or -EINVAL. Defaults are uid/gid zero and masks 0022. Each
 * umask applies to both masks at its position in the option sequence.
 * Empty input retains the legacy filesystem metadata policy. No I/O or locks.
 */
int fat_options_parse(const char *text, struct fat_mount_options *out)
{
    if (!out) return -EINVAL;
    struct fat_mount_options options = {.fmask = 0022, .dmask = 0022};
    for (const char *p = text; p && *p;) {
        const char *end = p;
        while (*end && *end != ',') ++end;
        const char *value = p;
        while (value != end && *value != '=') ++value;
        if (value == end) return -EINVAL;
        uint32_t number;
        bool uid = value - p == 3 && !__builtin_memcmp(p, "uid", 3);
        bool gid = value - p == 3 && !__builtin_memcmp(p, "gid", 3);
        bool file = value - p == 5 && !__builtin_memcmp(p, "fmask", 5);
        bool directory = value - p == 5 && !__builtin_memcmp(p, "dmask", 5);
        bool mask = value - p == 5 && !__builtin_memcmp(p, "umask", 5);
        if (!uid && !gid && !file && !directory && !mask) return -EINVAL;
        int rc = parse_value(value + 1, end, uid || gid ? 10 : 8,
                             uid || gid ? UINT32_MAX - 1 : 0777, &number);
        if (rc) return rc;
        if (uid) options.uid = number;
        if (gid) options.gid = number;
        if (file || mask) options.fmask = number;
        if (directory || mask) options.dmask = number;
        options.enabled = true;
        p = *end ? end + 1 : end;
        if (*end && !*p) return -EINVAL;
    }
    *out = options;
    return 0;
}
