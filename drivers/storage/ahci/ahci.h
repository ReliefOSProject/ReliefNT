/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_AHCI_PRIVATE_H
#define RELIEFNT_AHCI_PRIVATE_H
#include <reliefnt/dma.h>
#include "hardware.h"

/** @brief Flush a polling AHCI port through its owned nondata command slot.
 * @param port Stable mapped port, transport lock held.
 * @return Zero, -ENODEV, -EAGAIN for resumable wait, or native I/O errno.
 */
int ahci_flush_cache(struct ahci_hba_port *port);
/** @brief Read ATAPI blocks using independently owned physical DMA pages.
 * @param port Stable mapped port, transport lock held.
 * @param lba First 32-bit READ(10) block address.
 * @param block_count Nonzero 2048-byte blocks, at most 32.
 * @param buffer Writable kernel extent block_count*2048, current call only.
 * @return Zero, -EINVAL, -EAGAIN or native errno; never DMA to caller memory.
 */
int ahci_read_atapi_blocks_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t block_count, void *buffer);
/** @brief Read an ATA extent through driver-owned coherent physical backing.
 * @param port Stable mapped port, transport lock held.
 * @param lba First 48-bit sector address, full extent must fit.
 * @param sector_count Nonzero sectors, at most AHCI_MAX_SECTORS.
 * @param buffer Writable kernel extent sector_count*512, current call only.
 * @return Zero, -EINVAL, -EAGAIN or native errno; retries preserve DMA ownership.
 */
int ahci_read_lba_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t sector_count, void *buffer);
/** @brief STOP, allocate/reset per-port backing and publish physical addresses.
 * @param port Stable mapped port, transport lock held or BSP boot-only.
 * @param mask Inclusive controller DMA limit, UINT32_MAX unless CAP.S64A set.
 * @return Zero or native errno; failed STOP retains/quarantines all old pages.
 */
int ahci_setup_port(struct ahci_hba_port *port, uint64_t mask);
/** @brief STOP and release driver ports after external users/admission drain.
 * @return Zero or -EIO, retaining failed ports for retry; transport lock required.
 */
int ahci_release_ports(void);

/** @brief Write an ATA extent after copying valid CPU bytes into owned DMA pages.
 * @param port Stable mapped port, transport lock held.
 * @param lba First 48-bit sector address, full extent must fit.
 * @param sector_count Nonzero sectors, at most AHCI_MAX_SECTORS.
 * @param buffer Readable kernel extent sector_count*512, current call only.
 * @return Zero, -EINVAL, -EAGAIN or native errno; retries keep exact payload.
 */
int ahci_write_lba_retry(struct ahci_hba_port *port, uint64_t lba, uint32_t sector_count, const void *buffer);
/** @brief Fetch ATA IDENTIFY using the same per-port owned DMA/command engine.
 * @param port Stable mapped ATA port, transport lock held.
 * @param buffer Writable kernel extent of at least 512 bytes.
 * @return Zero, -EINVAL, -EAGAIN or native transport errno.
 */
int ahci_identify_device(struct ahci_hba_port *port, void *buffer);

/** @brief Drop only call identity under transport exclusion/BSP boot; still-busy
 * DMA is retained/quarantined until proven STOP. Never frees or copies old bytes.
 */
void ahci_pending_clear(void);

#define AHCI_DMA_FIS_OFFSET 1024u
#define AHCI_DMA_TABLE_OFFSET 1280u
#define AHCI_DMA_DATA_BYTES 65536u

/* Driver-owned list, serialized by storage_transport_lock (or BSP boot). Each
 * engine gets distinct CLB/FIS/table/data pages; no object is module BSS DMA. */
struct ahci_port_dma {
    struct ahci_port_dma *next;
    struct ahci_hba_port *port;
    struct dma_buffer descriptors, data;
    uint64_t mask;
    bool quarantined;
};

/** @brief Find a borrowed per-port object under transport exclusion.
 * @param port Stable mapped hardware port, required.
 * @return Borrowed context or NULL; valid until serialized successful STOP/free.
 */
struct ahci_port_dma *ahci_dma_find(struct ahci_hba_port *port);
/** @brief Obtain the driver list head under transport exclusion.
 * @return Borrowed first context or NULL; no allocation, I/O or owner changes.
 */
struct ahci_port_dma *ahci_dma_first(void);
/** @brief Claim per-port backing before publishing descriptor addresses to DMA.
 * @param port Stable stopped port; identity only, not dereferenced here.
 * @param mask Inclusive controller DMA mask, unchanged for an existing context.
 * @param out Required borrowed result, NULL on failure; caller serializes.
 * @return Zero, -EINVAL, -EBUSY for a changed mask, or allocation error. All
 *         partial allocations rolled back; existing port backing is preserved.
 */
int ahci_dma_create(struct ahci_hba_port *port, uint64_t mask,
                    struct ahci_port_dma **out);
/** @brief Return slot-zero command header within owned CPU descriptor pages.
 * @param dma Stable allocated per-port context under transport exclusion.
 * @return Borrowed writable slot-zero header; never use this CPU pointer as DMA.
 */
struct ahci_cmd_header *ahci_dma_header(struct ahci_port_dma *dma);
/** @brief Return slot-zero command table within owned CPU descriptor pages.
 * @param dma Stable allocated per-port context under transport exclusion.
 * @return Borrowed writable command table, or NULL for an invalid context.
 */
struct ahci_cmd_table *ahci_dma_table(struct ahci_port_dma *dma);
/** @brief Clear descriptors and discard old transfer only after proven STOP.
 * @param dma Stable exclusively owned per-port context.
 * @param stopped True only when hardware FR and CR have both cleared.
 * @return Zero, -EINVAL, or -EBUSY; no buffer state changes on failed STOP.
 */
int ahci_dma_reset(struct ahci_port_dma *dma, bool stopped);
/** @brief Stage real transfer and program full-width PRDT physical addresses.
 * @param dma Stable per-port context under transport exclusion, idle engine.
 * @param bytes Nonzero transfer bytes, at most AHCI_DMA_DATA_BYTES.
 * @param direction Read or write direction.
 * @param source Valid kernel write bytes, NULL for device read.
 * @param flags AHCI header CFL/ATAPI/W bits, already validated by command builder.
 * @return Zero or -EINVAL/-EBUSY; publishes no MMIO, retains transfer until end.
 */
int ahci_dma_prepare(struct ahci_port_dma *dma, size_t bytes,
                     enum dma_direction direction, const void *source,
                     uint16_t flags);
/** @brief Unlink and release backing only after successful hardware STOP.
 * @param dma Driver-owned linked context under transport exclusion, invalid on success.
 * @param stopped True only after FR/CR clear; false retains every allocation.
 * @return Zero, -EINVAL for an unknown object, or -EBUSY; no hardware calls.
 */
int ahci_dma_destroy(struct ahci_port_dma *dma, bool stopped);
/** @brief STOP both native engines while retaining DMA backing on failure.
 * @param port Exclusively owned mapped port, required.
 * @return Zero only after CR/FR clear, otherwise -EINVAL/-EIO. Task context
 * under transport exclusion; no allocation, callback or software CI clearing.
 */
int ahci_stop_port(struct ahci_hba_port *port);
/** @brief Free one already-quiesced port after all device/code users drain.
 * @param port Exclusively STOP-proven mapped port. Clears descriptor addresses
 * and consumes owned DMA, never dereferences pending caller buffer identities.
 * Transport exclusion required, no hardware restart or fallible wait occurs.
 */
void ahci_release_stopped_port(struct ahci_hba_port *port);
/** @brief Cancel/discard one task's pending transfer through owned driver buffers.
 * @param pid Task identity, zero ignored. Transport/execution exclusion required;
 * no old caller pointer is dereferenced, failed DMA remains until proven STOP.
 */
void ahci_cancel_task(uint32_t pid);
/** @brief Read the standard 8-byte big-endian READ CAPACITY(10) response.
 * @param port Stable ATAPI port. @param response Writable 8-byte kernel extent.
 * @return Zero or transport errno including -EAGAIN. Transport exclusion
 * required; DMA targets driver-owned backing independent of the caller buffer.
 */
int ahci_read_capacity(struct ahci_hba_port *port,uint8_t response[8]);
#endif
