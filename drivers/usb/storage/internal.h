/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_USB_STORAGE_INTERNAL_H
#define RELIEFNT_USB_STORAGE_INTERNAL_H

#include <reliefnt/usb_device.h>
#include <reliefnt/blkdev.h>

#define USB_STORAGE_MAX_LUNS 16u
/* Bound each transfer while retaining useful filesystem request sizes. */
#define USB_STORAGE_MAX_BLOCKS 128u

struct usb_storage;
struct usb_storage_lun {
    struct usb_storage *storage;
    struct block_device_registration registration;
    uint8_t number;
    bool registered, revoked, write_through, write_attempted, dpofua_valid, dpofua;
};

struct usb_storage {
    struct usb_device *device;
    struct usb_endpoint in, out;
    uint32_t tag;
    uint8_t interface_number;
    bool detached, broken;
    struct usb_storage_lun luns[USB_STORAGE_MAX_LUNS];
};

/** @brief Execute one BOT command while the caller holds the host gate.
 * @param storage Stable class state. @param lun Logical unit number 0..15.
 * @param cdb Readable command bytes. @param cdb_length CDB length 1..16.
 * @param data CPU buffer, nullable for no data. @param length Expected bytes.
 * @param input True for device-to-host data. @param actual Relevant data bytes.
 * @return Zero for passed command, one for failed command, or negative errno.
 */
int usb_bot_command(struct usb_storage *storage, uint8_t lun, const uint8_t *cdb,
                    uint8_t cdb_length, void *data, uint32_t length, bool input,
                    uint32_t *actual);

/** @brief Discover and publish a single SCSI LUN under the held host gate.
 * @param storage Stable class state. @param number Logical unit number.
 * @param fatal Receives true when a probe error must not be skipped.
 * @return Zero or negative errno; successful registration retains state.
 */
int usb_scsi_probe_lun(struct usb_storage *storage, uint8_t number, bool *fatal);

/** @brief Observe class or host disconnect with acquire ordering.
 * @param storage Stable class state.
 * @return True after either disconnect indication; no I/O or ownership change.
 */
static inline bool usb_storage_gone(struct usb_storage *storage)
{
    return __atomic_load_n(&storage->detached, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&storage->device->disconnected, __ATOMIC_ACQUIRE);
}

#endif
