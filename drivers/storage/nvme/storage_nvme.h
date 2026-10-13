/* NVMe driver-private controller state and transport geometry.  Only the
 * NVMe driver and the legacy storage probe paths that fill or inspect these
 * fields may include this. */
#ifndef RELIEFOS_STORAGE_NVME_H
#define RELIEFOS_STORAGE_NVME_H

#include <stdint.h>
#include <reliefnt/lock.h>
#include "../../block/storage_internal.h"

struct nvme_command;
struct nvme_completion;

#define NVME_PAGE_SIZE 4096u
#define NVME_IO_BUFFER_PAGES 32u
#define NVME_MAX_SECTORS (NVME_IO_BUFFER_PAGES * (NVME_PAGE_SIZE / SECTOR_SIZE))

struct nvme_controller {
    uint8_t present;
    uint8_t ready;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint16_t controller_index;
    uint8_t cq_phase_admin;
    uint8_t cq_phase_io;
    uint16_t queue_depth;
    uint16_t admin_sq_tail;
    uint16_t admin_cq_head;
    uint16_t io_sq_tail;
    uint16_t io_cq_head;
    uint16_t next_command_id;
    uint32_t doorbell_stride;
    uint32_t namespace_count;
    uint64_t cap;
    volatile uint8_t *mmio;
    uint64_t admin_sq_phys;
    uint64_t admin_cq_phys;
    uint64_t io_sq_phys;
    uint64_t io_cq_phys;
    uint64_t identify_phys;
    uint64_t namespace_list_phys;
    uint64_t io_buffer_phys;
    uint64_t prp_list_phys;
    struct nvme_command *admin_sq;
    struct nvme_completion *admin_cq;
    struct nvme_command *io_sq;
    struct nvme_completion *io_cq;
    uint8_t *identify_buffer;
    uint8_t *namespace_list;
    uint8_t *io_buffer;
    uint64_t *prp_list;
    uint32_t io_buffer_pages;
    char model[41];
    struct kernel_spinlock lock;
};

#endif /* RELIEFOS_STORAGE_NVME_H */
