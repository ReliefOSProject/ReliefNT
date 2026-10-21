/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_FS_MOUNT_API_H
#define RELIEFNT_FS_MOUNT_API_H

#include "../block/core/storage_types.h"

/**
 * @file fs_mount_api.h
 * @brief Narrow mount callback used by storage transports during discovery.
 */

/**
 * @brief Offers a newly discovered disk to normal root-volume selection.
 * @param disk Probed disk state with transport and capacity information.
 * @return Zero when accepted, or a negative storage error.
 */
int storage_try_mount_root_disk(struct install_disk_state *disk);

#endif /* RELIEFNT_FS_MOUNT_API_H */
