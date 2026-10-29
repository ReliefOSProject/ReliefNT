/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_BLKDEV_H
#define RELIEFNT_BLKDEV_H
#include <reliefnt/types.h>

struct module;
struct block_device;
#define BLOCK_DEVICE_READ_ONLY 1u
#define BLOCK_DEVICE_REMOVABLE 2u
#define BLOCK_DEVICE_OPTICAL 4u
#define BLOCK_DEVICE_WRITE_THROUGH 8u
/* Scalar inventory transport for reserved boot-image backing, not a pointer. */
#define BLOCK_TRANSPORT_MEMORY 4u
#define BLOCK_TRANSPORT_USB 5u

struct block_device_info {
    uint64_t device, blocks;
    uint32_t block_size, max_blocks, flags;
    /* Immutable human/inventory identity only; never a provider pointer. */
    uint8_t transport,bus,slot,function,port;
    uint16_t controller;
    uint32_t namespace_id;
    char model[41];
};

/* Task-context transport callbacks. The caller pins this registration's module
 * and CPU buffer, but holds no registry/module lock. Drivers serialize their
 * engines and DMA backing; zero means the complete block extent transferred,
 * negative errno includes resumable -EAGAIN. CPU pointers are never bus addresses.
 * All buffer extents are count*info.block_size bytes; first/count were checked. */
struct block_device_operations {
    int (*read)(void *context,uint64_t first,uint32_t count,void *buffer);
    int (*write)(void *context,uint64_t first,uint32_t count,const void *buffer);
    int (*flush)(void *context);
    /* Optional task-exit drain; discard pending reads, never touch old buffers. */
    void (*cancel)(void *context,uint32_t pid);
};

/* Persistent immutable descriptor/context/operations, retained until final
 * unregistration/owner cleanup. Registration itself never pins its module.
 * Flush may be absent only for read-only or explicitly write-through devices. */
struct block_device_registration {
    struct block_device_info info;
    const struct block_device_operations *operations;
    void *context;
    struct module *owner;
};

/* Owned noncopyable external reference. Mounts/OFDs/in-flight work retain one;
 * copying a token without another get is invalid. Empty tokens are all-zero. */
struct block_device_ref { struct block_device *device; };

/** @brief Encode a Linux wide dev_t without narrowing its minor number.
 * @param major Linux major <=4095. @param minor Linux minor <=1048575.
 * @return Encoded identity, or zero for unrepresentable input. Pure arithmetic;
 * no allocation, lock, provider callback or ownership transfer.
 */
static inline uint64_t block_device_make_number(uint32_t major,uint32_t minor)
{ return major>4095 || minor>1048575 ? 0 : ((uint64_t)major<<8)|(minor&255u)|((uint64_t)(minor&~255u)<<12); }
/** @brief Reserve a never-reused native disk or optical device identity.
 * @param optical True for the standard major 11 optical namespace; false for
 * dynamically numbered major 259 disks with 256 reserved partition minors.
 * @return Nonzero Linux dev_t or zero on exhausted namespace. IRQ-safe short
 * registry lock; no allocation/callback or module self-reference. Gaps survive
 * failed probes and unload, preventing a stale inventory/OFD from aliasing reload.
 */
uint64_t block_device_allocate_number(bool optical);
/** @brief Discard an exiting task's pending native transfers through pinned code.
 * @param pid Nonzero task identity; zero is ignored.
 * Execution/storage exclusion held by caller; no registry lock spans callbacks.
 * Each callback receives only task identity and driver-owned backing; no old task
 * pointer is retained or dereferenced. GOING controllers drain in owner quiesce.
 */
void block_device_cancel_all(uint32_t pid);

/** @brief Publish a block transport after its owner cleanup obligation exists.
 * @param registration Persistent immutable descriptor with nonzero dev_t, blocks,
 * power-of-two block_size>=512, max_blocks, read and authorized write/flush ops.
 * @return Zero or -EINVAL/-EBUSY/-ENOMEM/-EOVERFLOW/owner admission errno.
 * Init or already-pinned task context; COMING owners cannot admit readers. No
 * allocation, hardware or provider callback runs under registry/module locks.
 */
int register_block_device(const struct block_device_registration *registration);
/** @brief Unpublish an idle exact descriptor without stopping its hardware.
 * @param registration Stable exact descriptor; owned context remains caller's.
 * @return Zero, -ENOENT if absent, -EBUSY while external users remain, or -EINVAL.
 * Task context. Busy leaves admission unchanged; module unload closes owner
 * admission before quiesce. Controller STOP/free belongs to the driver's ledger.
 */
int unregister_block_device(const struct block_device_registration *registration);
/** @brief Revoke a physically removed device while retaining its held identities.
 * @param registration Persistent exact descriptor, held until idle unregister.
 * @return Zero or -EINVAL/-ENOENT. Task context; no hardware call or wait.
 * New gets/enumeration and held read/write/flush fail ENODEV. Existing tokens
 * still retain immutable metadata and owner code until their normal put.
 */
int disconnect_block_device(const struct block_device_registration *registration);
/** @brief Resolve dev_t and acquire both core identity and LIVE module ownership.
 * @param device Nonzero native dev_t identity.
 * @param out Required empty output, unchanged on error.
 * @return Zero, -EINVAL/-EBUSY/-ENODEV/-EOVERFLOW. IRQ-safe registry-to-module
 * lock order; no callbacks/I/O/allocations. Caller later consumes with put.
 */
int block_device_get(uint64_t device,struct block_device_ref *out);
/** @brief Enumerate admitted devices in monotonic registration order.
 * @param cursor Required in/out registration serial, zero starts enumeration.
 * @param out Required empty owned reference output, unchanged on failure.
 * @return Zero, -ENOENT at current end, or -EINVAL/-EBUSY/-EOVERFLOW. IRQ-safe;
 * skips closed/COMING/GOING owners, no callback. Successful get advances cursor.
 */
int block_device_next(uint64_t *cursor,struct block_device_ref *out);
/** @brief Consume an owned device/code reference without entering provider code.
 * @param reference Owned token cleared before release; NULL/empty accepted.
 * IRQ-safe; final heap release outside registry lock, no hardware/provider call.
 */
void block_device_put(struct block_device_ref *reference);
/** @brief Snapshot immutable geometry through a held transport reference.
 * @param reference Borrowed owned reference, held through the call.
 * @param out Required writable snapshot, no pointer or ownership transfer.
 * @return Zero or -EINVAL; no allocation, callback or I/O.
 */
int block_device_info(const struct block_device_ref *reference,struct block_device_info *out);
/** @brief Read one complete bounded transport extent with provider execution pinned.
 * @param reference Borrowed owned device reference, held through the callback.
 * @param first First logical block. @param count Nonzero blocks<=max_blocks.
 * @param buffer Writable stable kernel extent count*block_size bytes.
 * @return Zero, -EINVAL/-ENODEV or native errno including -EAGAIN unchanged.
 * Task context under caller's storage exclusion; no registry lock spans I/O.
 */
int block_device_read(const struct block_device_ref *reference,uint64_t first,uint32_t count,void *buffer);
/** @brief Write one complete bounded transport extent through held code ownership.
 * @param reference Borrowed owned device reference, held through the callback.
 * @param first First logical block. @param count Nonzero blocks<=max_blocks.
 * @param buffer Readable stable kernel extent count*block_size bytes.
 * @return Zero, -EINVAL/-EROFS/-ENODEV or native errno including -EAGAIN.
 * Task context under caller's storage exclusion; no registry lock spans I/O.
 */
int block_device_write(const struct block_device_ref *reference,uint64_t first,uint32_t count,const void *buffer);
/** @brief Flush native volatile cache through a held device/code reference.
 * @param reference Borrowed owned reference, held through the callback.
 * @return Zero, -EINVAL/-ENODEV or native errno. An absent callback is successful
 * only for explicitly read-only/write-through devices. Task context, no registry
 * lock across I/O; a native failure is never converted into success.
 */
int block_device_flush(const struct block_device_ref *reference);
#endif
