/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_INTERNAL_H
#define RELIEFNT_STORAGE_INTERNAL_H

/**
 * @file storage_internal.h
 * @brief Private aggregate for block and filesystem implementation units.
 *
 * Transport drivers include storage_driver.h directly. Filesystem families
 * share their definitions through fs/fs_internal.h; the storage_volume
 * composite is assembled from the owned type headers.
 */

#include <reliefnt/mm.h>
#include <reliefnt/console.h>
#include <reliefnt/multiboot2.h>
#include <reliefnt/paging.h>
#include <reliefnt/pci.h>
#include <reliefnt/port.h>
#include <reliefnt/sched.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/audio.h>
#include <reliefnt/text_utf16.h>
#include <reliefnt/tmpfs.h>
#include <reliefnt/syscall.h>
#include <reliefnt/time.h>
#include <reliefnt/lock.h>
#include <reliefnt/pty.h>
#include <linux/mount.h>

#include "storage_hardware.h"
#include "storage_types.h"
#include "storage_core_api.h"
#include "../partition/storage_partition.h"
#include "../../fs/fs_internal.h"
#include "storage_driver.h"

#endif /* RELIEFNT_STORAGE_INTERNAL_H */
