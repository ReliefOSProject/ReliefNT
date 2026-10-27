/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_INITRAMFS_BOOT_H
#define RELIEFNT_INITRAMFS_BOOT_H
#include <reliefnt/types.h>
struct boot_info;
/** @brief Classify and map the unique reserved Multiboot module named initramfs.
 * @param boot Borrowed decoded handoff, NULL means no external archive.
 * @param image Required borrowed immutable mapping output, NULL on failure/absence.
 * @param length Required complete extent output, zero on failure/absence.
 * @return One for a mapped archive, zero absent, or -EINVAL/-EEXIST/-ENOEXEC.
 * BSP after memory reservation/direct-map initialization; the reserved source
 * outlives unpack. Installer filesystem images are separate and never copied
 * or mistaken for archives. No allocation, disk I/O or locks.
 */
int initramfs_boot_source(const struct boot_info *boot,const void **image,size_t *length);
/** @brief Construct the selected initial memory root before ordinary root probing.
 * @param boot Borrowed decoded handoff, NULL permitted.
 * @return One after complete root publication, zero without an archive, or
 * negative source/unpack/busy errno. Failures do not fall back to a disk root.
 * BSP exclusive execution before namespace/tasks; no global namespace is
 * published here. Source ranges remain reserved, all failed backing is freed.
 */
int storage_initramfs_boot(const struct boot_info *boot);
/** @brief Observe which PID1 contract was selected during root construction.
 * @return True after successful newc-root construction, otherwise false.
 * Boot-writer/execution-owned readers only, no allocation/I/O/lock or ownership.
 */
bool storage_initramfs_active(void);
#endif
