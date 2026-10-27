/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_BOOT_BLOCK_H
#define RELIEFNT_BOOT_BLOCK_H
#include <reliefnt/types.h>
struct boot_info;
/** @brief Bind a reserved boot-image physical extent to the permanent RAM device.
 * @param physical Physical source start, never a CPU pointer; mm already reserved it.
 * @param length Sector-aligned source extent, retained through kernel shutdown.
 * @param device Required Linux dev_t output, zero on failure.
 * @return Zero or EINVAL/ENOEXEC/EBUSY/registration errno. Identical rebinding is
 * idempotent; another extent cannot replace live backing. BSP exclusive boot
 * setup before storage users, after direct-map initialization. Only registry
 * metadata is allocated; payload is never copied. Subsequent writes are coherent
 * ephemeral memory writes, never writes to the ISO/disk from which GRUB loaded it.
 */
int block_boot_image_bind(uint64_t physical,uint64_t length,uint64_t *device);
/** @brief Register the separate installer/live filesystem-image boot module.
 * @param boot Borrowed decoded handoff, NULL means no source.
 * @param device Required Linux dev_t output, zero on absence/failure.
 * @return One registered/already bound, zero absent, or EINVAL/EEXIST/ENOEXEC/
 * EBUSY/registration errno. Accepts the existing reliefos-installer-root name
 * and historical leonos-installer-root alias; duplicates fail. The initramfs
 * module is never interpreted as a block image. BSP exclusive boot setup after
 * memory reservation, no payload allocation/copy or disk/filesystem I/O.
 */
int block_boot_image_register(const struct boot_info *boot,uint64_t *device);
#endif
