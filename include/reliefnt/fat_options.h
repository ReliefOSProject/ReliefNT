/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FAT_OPTIONS_H
#define RELIEFNT_FAT_OPTIONS_H
#include <reliefnt/types.h>
struct fat_mount_options {
    uint32_t uid, gid, fmask, dmask;
    bool enabled;
};
/** @brief Parse Linux FAT ownership and octal permission-mask mount options.
 * @param text Optional comma-separated uid/gid/umask/fmask/dmask string.
 * @param out Required destination, changed only after complete validation.
 * @return Zero or -EINVAL for malformed/unknown/overflowing options. Empty
 * input disables overrides. Pure task-context parser, no allocation or I/O.
 */
int fat_options_parse(const char *text, struct fat_mount_options *out);
#endif
