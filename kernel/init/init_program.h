/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_INIT_PROGRAM_H
#define RELIEFNT_INIT_PROGRAM_H
#include <reliefnt/types.h>
/** @brief Select the correct PID1 program for initial-memory or direct-root boot.
 * @param cmdline Borrowed NUL-terminated boot arguments, NULL means none.
 * @param memory_root True selects rdinit=/init; false selects init=/sbin/init.
 * @param out Required writable pathname output, unspecified on error.
 * @param capacity Destination bytes including the terminating NUL.
 * @return Zero, -EINVAL for operands/nonabsolute/empty selected override, or
 * -ENAMETOOLONG. Last matching token wins. The other boot-stage key remains
 * untouched for its consumer; pure, no allocation, locks or ownership transfer.
 */
int init_program_path(const char *cmdline,bool memory_root,char *out,size_t capacity);
#endif
