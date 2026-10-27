/* SPDX-License-Identifier: Apache-2.0 */
#include "ahci.h"
#include <reliefnt/heap.h>
#include <linux/errno.h>

static struct ahci_port_dma *ports;

/** @brief Look up a live driver-owned backing without acquiring a self reference.
 * @param port Mapped hardware-port identity, never dereferenced by lookup.
 * @return Borrowed context or NULL; caller holds transport exclusion through use.
 */
struct ahci_port_dma *ahci_dma_find(struct ahci_hba_port *port)
{
    for (struct ahci_port_dma *dma = ports; dma; dma = dma->next)
        if (dma->port == port) return dma;
    return NULL;
}

/** @brief Expose list head for serialized driver quiesce/retry.
 * @return Borrowed head or NULL; transport exclusion must remain held.
 */
struct ahci_port_dma *ahci_dma_first(void) { return ports; }

/** @brief Allocate complete per-port descriptor and payload ownership atomically.
 * @param port Stable stopped hardware-port identity.
 * @param mask Inclusive bus mask for both descriptor and data extents.
 * @param out Required borrowed result, NULL on failure.
 * @return Zero or -EINVAL/-EBUSY/allocation errno; no hardware or callback calls.
 * Caller holds transport exclusion; publication occurs only after both allocations.
 */
int ahci_dma_create(struct ahci_hba_port *port, uint64_t mask,
                    struct ahci_port_dma **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!port) return -EINVAL;
    struct ahci_port_dma *dma = ahci_dma_find(port);
    if (dma) {
        if (dma->mask != mask) return -EBUSY;
        *out = dma;
        return 0;
    }
    dma = kernel_malloc(sizeof(*dma));
    if (!dma) return -ENOMEM;
    *dma = (struct ahci_port_dma){.port = port, .mask = mask};
    int error = dma_buffer_alloc(&dma->descriptors, 4096, mask);
    if (!error) error = dma_buffer_alloc(&dma->data, AHCI_DMA_DATA_BYTES, mask);
    if (error) {
        (void)dma_buffer_free(&dma->descriptors);
        kernel_free(dma);
        return error;
    }
    dma->next = ports;
    ports = dma;
    *out = dma;
    return 0;
}

/** @brief Translate only the CPU alias of the owned slot-zero header.
 * @param dma Stable per-port backing, serialized by transport exclusion.
 * @return Borrowed header or NULL for absent backing.
 */
struct ahci_cmd_header *ahci_dma_header(struct ahci_port_dma *dma)
{
    return dma ? dma->descriptors.cpu : NULL;
}

/** @brief Translate only the CPU alias of the owned slot-zero command table.
 * @param dma Stable per-port backing, serialized by transport exclusion.
 * @return Borrowed table or NULL for absent backing.
 */
struct ahci_cmd_table *ahci_dma_table(struct ahci_port_dma *dma)
{
    return dma && dma->descriptors.cpu ?
        (void *)((unsigned char *)dma->descriptors.cpu + AHCI_DMA_TABLE_OFFSET) : NULL;
}

/** @brief Reinitialize command RAM after stopping both command and FIS engines.
 * @param dma Exclusively locked stable per-port backing.
 * @param stopped Hardware CR/FR have cleared; false changes no backing.
 * @return Zero, -EINVAL for absent backing, or -EBUSY for failed STOP.
 */
int ahci_dma_reset(struct ahci_port_dma *dma, bool stopped)
{
    if (!dma || !dma->descriptors.cpu) return -EINVAL;
    if (!stopped) return -EBUSY;
    if (dma->data.busy) (void)dma_buffer_end(&dma->data, NULL, true);
    unsigned char *bytes = dma->descriptors.cpu;
    for (size_t i = 0; i < dma->descriptors.capacity; ++i) bytes[i] = 0;
    struct ahci_cmd_header *header = ahci_dma_header(dma);
    uint64_t table = dma->descriptors.physical + AHCI_DMA_TABLE_OFFSET;
    header->ctba = (uint32_t)table;
    header->ctbau = (uint32_t)(table >> 32);
    header->prdtl = 1;
    dma->quarantined = false;
    return 0;
}

/** @brief Stage bytes and fill actual AHCI slot-zero physical DMA descriptors.
 * @param dma Exclusively owned idle per-port backing.
 * @param bytes Transfer bytes, nonzero and within the coherent data buffer.
 * @param direction Read/write direction.
 * @param source Borrowed valid kernel bytes for write, NULL for read.
 * @param flags Command-specific CFL/ATAPI/W bits supplied by the command builder.
 * @return Zero or -EINVAL/-EBUSY; failure cannot overwrite a live descriptor.
 */
int ahci_dma_prepare(struct ahci_port_dma *dma, size_t bytes,
                     enum dma_direction direction, const void *source,
                     uint16_t flags)
{
    if (!dma) return -EINVAL;
    if (dma->quarantined) return -EBUSY;
    int error = dma_buffer_begin(&dma->data, direction, source, bytes);
    if (error) return error;
    struct ahci_cmd_header *header = ahci_dma_header(dma);
    struct ahci_cmd_table *table = ahci_dma_table(dma);
    unsigned char *clear = (void *)table;
    for (size_t i = 0; i < sizeof(*table); ++i) clear[i] = 0;
    header->flags = flags;
    header->prdtl = 1;
    header->prdbc = 0;
    table->prdt[0].dba = (uint32_t)dma->data.physical;
    table->prdt[0].dbau = (uint32_t)(dma->data.physical >> 32);
    table->prdt[0].dbc = (uint32_t)(bytes - 1u);
    return 0;
}

/** @brief Release exact linked backing after external STOP proved hardware idle.
 * @param dma Driver-owned linked context, invalidated only on success.
 * @param stopped CR/FR clear and no future command admission; false retains all.
 * @return Zero, -EINVAL for unknown context, or -EBUSY. Caller serializes the list.
 */
int ahci_dma_destroy(struct ahci_port_dma *dma, bool stopped)
{
    if (!dma) return -EINVAL;
    if (!stopped) return -EBUSY;
    struct ahci_port_dma **link = &ports;
    while (*link && *link != dma) link = &(*link)->next;
    if (!*link) return -EINVAL;
    if (dma->data.busy) (void)dma_buffer_end(&dma->data, NULL, true);
    *link = dma->next;
    (void)dma_buffer_free(&dma->data);
    (void)dma_buffer_free(&dma->descriptors);
    kernel_free(dma);
    return 0;
}
