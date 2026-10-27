#include <reliefnt/console.h>
/* SPDX-License-Identifier: Apache-2.0 */
#include "ahci.h"
#include "../../../block/core/storage_core_api.h"
#include <reliefnt/sched.h>
#include <reliefnt/syscall.h>
#include <reliefnt/time.h>

/* The transport lock serializes one command across ports. Pending records keep
 * caller pointers only as identity; DMA always targets independently owned pages. */
enum ahci_operation { AHCI_READ, AHCI_WRITE, AHCI_PACKET_READ, AHCI_PACKET_CAPACITY, AHCI_IDENTIFY, AHCI_FLUSH };
struct ahci_pending_command {
    struct ahci_hba_port *port;
    struct ahci_port_dma *dma;
    uint64_t lba;
    uint32_t sector_count;
    const void *buffer;
    uint64_t start_tick;
    uint32_t owner_pid;
    enum ahci_operation operation;
    uint8_t write, active;
};
static struct ahci_pending_command ahci_pending_command;

/** @brief Yield execution resources in a bounded hardware polling loop. */
static void ahci_cpu_relax(void) { __asm__ volatile("pause"); }

/** @brief Order coherent descriptor/data accesses against AHCI MMIO submission. */
static void ahci_memory_barrier(void) { __asm__ volatile("mfence" ::: "memory"); }

/** @brief Briefly poll an engine without holding a syscall for a full PIT tick.
 * @param port Stable mapped port under transport exclusion.
 * @return Zero if taskfile idle, otherwise -EAGAIN; never changes hardware.
 */
static int ahci_async_fast_poll_idle(struct ahci_hba_port *port)
{
    for (uint32_t i = 0; i < AHCI_ASYNC_FAST_POLL_SPINS; ++i) {
        if (!(port->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ))) return 0;
        ahci_cpu_relax();
    }
    return -RELIEFOS_EAGAIN;
}

/** @brief Briefly wait for slot-zero hardware completion, without consuming data.
 * @param port Stable mapped port under transport exclusion.
 * @return Zero for completion, -EIO for TFES, or -EAGAIN while hardware owns CI.
 */
static int ahci_async_fast_poll_command(struct ahci_hba_port *port)
{
    for (uint32_t i = 0; i < AHCI_ASYNC_FAST_POLL_SPINS; ++i) {
        if (port->is & AHCI_PORT_IS_TFES) return -5;
        if (!(port->ci & 1u)) return 0;
        ahci_cpu_relax();
    }
    return -RELIEFOS_EAGAIN;
}

/** @brief Wait for the taskfile before preparing a command, preserving async retry.
 * @param port Stable mapped port under transport exclusion.
 * @return Zero, -EAGAIN in resumable context, or -EIO after bounded sync timeout.
 */
static int ahci_wait_idle(struct ahci_hba_port *port)
{
    if (!port) return -22;
    if (storage_async_can_yield()) return ahci_async_fast_poll_idle(port);
    for (uint32_t i = 0; i < AHCI_WAIT_SPINS; ++i) {
        if (!(port->tfd & (AHCI_PORT_TFD_BSY | AHCI_PORT_TFD_DRQ))) return 0;
        ahci_cpu_relax();
    }
    return -5;
}

/** @brief Wait for all hardware-owned command slots before changing command RAM.
 * @param port Stable mapped port under transport exclusion.
 * @return Zero, -EAGAIN in resumable context, or -EIO after bounded sync timeout.
 */
static int ahci_wait_cmd_slot(struct ahci_hba_port *port)
{
    if (!port) return -22;
    uint32_t limit = storage_async_can_yield() ? AHCI_ASYNC_FAST_POLL_SPINS : AHCI_WAIT_SPINS;
    for (uint32_t i = 0; i < limit; ++i) {
        if (!(port->ci | port->sact)) return 0;
        ahci_cpu_relax();
    }
    return storage_async_can_yield() ? -RELIEFOS_EAGAIN : -5;
}

/** @brief Forget only pending-call identity, retaining any still-live DMA backing.
 * Caller holds transport exclusion/BSP boot. Live data is quarantined until STOP;
 * this function never frees pages or copies to the saved caller pointer.
 */
void ahci_pending_clear(void)
{
    if (ahci_pending_command.dma && ahci_pending_command.dma->data.busy)
        ahci_pending_command.dma->quarantined = true;
    ahci_pending_command = (struct ahci_pending_command){0};
}

/** @brief Emit exact pending command/controller state before retaining failed DMA.
 * @param reason Stable diagnostic string.
 * @param port Mapped failing port; NULL suppresses the diagnostic.
 */
static void ahci_log_pending_failure(const char *reason, struct ahci_hba_port *port)
{
    if (!port) return;
    console_error("[reliefnt] ahci %s op=%s owner=%u lba=%llu sectors=%u "
                  "ci=0x%x is=0x%x tfd=0x%x serr=0x%x\n", reason,
                  ahci_pending_command.write ? "write" : "read",
                  ahci_pending_command.owner_pid,
                  (unsigned long long)ahci_pending_command.lba,
                  ahci_pending_command.sector_count, port->ci, port->is,
                  port->tfd, port->serr);
}

/** @brief Consume completion into a currently valid read buffer, or discard it.
 * @param destination Valid current read bytes, NULL for cancellation/other command.
 * @return Zero, -EINVAL for no pending command, -EAGAIN for resumable wait, or
 *         -EIO. Failed live DMA is retained/quarantined for STOP/recovery; no saved
 *         task pointer is dereferenced. Caller holds transport exclusion.
 */
static int ahci_pending_poll(void *destination)
{
    struct ahci_hba_port *port = ahci_pending_command.port;
    struct ahci_port_dma *dma = ahci_pending_command.dma;
    if (!ahci_pending_command.active || !port) return -22;
    if (port->is & AHCI_PORT_IS_TFES) goto failed;
    if (port->ci & 1u) {
        if (storage_async_can_yield()) {
            int result = ahci_async_fast_poll_command(port);
            if (result != -RELIEFOS_EAGAIN) goto complete;
            if (time_ticks() - ahci_pending_command.start_tick < AHCI_ASYNC_TIMEOUT_TICKS)
                return -RELIEFOS_EAGAIN;
            ahci_log_pending_failure("timeout", port);
            ahci_pending_clear();
            return -5;
        }
        for (uint32_t i = 0; i < AHCI_WAIT_SPINS; ++i) {
            if (port->is & AHCI_PORT_IS_TFES) goto failed;
            if (!(port->ci & 1u)) goto complete;
            ahci_cpu_relax();
        }
        ahci_log_pending_failure("wait timeout", port);
        ahci_pending_clear();
        return -5;
    }
complete:
    if (port->is & AHCI_PORT_IS_TFES) goto failed;
    ahci_memory_barrier();
    if (dma && dma->data.busy) {
        int result = dma_buffer_end(&dma->data, destination, true);
        if (result) return result;
    }
    ahci_pending_clear();
    return 0;
failed:
    ahci_log_pending_failure("task-file error", port);
    if (dma && dma->data.busy && !(port->ci & 1u))
        (void)dma_buffer_end(&dma->data, NULL, true);
    ahci_pending_clear();
    return -5;
}

/** @brief Match a resumed call, including task identity and command protocol.
 * @param port Current mapped port.
 * @param lba Current command LBA.
 * @param count Current sector or ATAPI block count.
 * @param buffer Current valid caller buffer used only as an identity comparison.
 * @param operation Exact ATA/ATAPI/identify/flush protocol operation.
 * @return Nonzero only for the same pending request owned by the current task.
 */
static int ahci_pending_matches(struct ahci_hba_port *port, uint64_t lba,
                               uint32_t count, const void *buffer,
                               enum ahci_operation operation)
{
    return ahci_pending_command.active && ahci_pending_command.port == port &&
        ahci_pending_command.lba == lba && ahci_pending_command.sector_count == count &&
        ahci_pending_command.buffer == buffer && ahci_pending_command.operation == operation &&
        ahci_pending_command.owner_pid == sched_current_pid();
}

/** @brief Drain or quarantine an exiting task's DMA without touching task memory.
 * @param pid Exiting task identity, zero is ignored. Reaper context only; current
 * transport exclusion is supplied by the single-CPU async ownership protocol.
 * DMA pages belong to the driver even on timeout, so task pages can be reclaimed.
 */
void ahci_cancel_task(uint32_t pid)
{
    if (!pid) return;
    if (ahci_pending_command.active && ahci_pending_command.owner_pid == pid) {
        bool saved_async = storage_io_async_context;
        bool saved_write = storage_io_write_started;
        storage_io_async_context = false;
        storage_io_write_started = true;
        (void)ahci_pending_poll(NULL);
        storage_io_async_context = saved_async;
        storage_io_write_started = saved_write;
    }
}

/** @brief Stop both engines before rewriting or releasing port DMA pages.
 * @param port Stable mapped port under transport exclusion.
 * @return Zero only after CR/FR clear, -EINVAL for NULL, otherwise -EIO. Timeout
 * leaves hardware-owned memory intact; software never clears hardware CI bits.
 */
int ahci_stop_port(struct ahci_hba_port *port)
{
    if (!port) return -22;
    port->cmd &= ~AHCI_PORT_CMD_ST;
    /* Stop command processing and observe CR=0 before stopping FIS reception. */
    for (uint32_t i = 0; i < AHCI_WAIT_SPINS; ++i) {
        if (!(port->cmd & AHCI_PORT_CMD_CR)) {
            port->cmd &= ~AHCI_PORT_CMD_FRE;
            for (uint32_t j = 0; j < AHCI_WAIT_SPINS; ++j) {
                if (!(port->cmd & AHCI_PORT_CMD_FR)) return 0;
                ahci_cpu_relax();
            }
            break;
        }
        ahci_cpu_relax();
    }
    return -5;
}

/** @brief Initialize/recover a port with independently owned physical DMA pages.
 * @param port Mapped AHCI port under transport exclusion/BSP boot.
 * @param mask Inclusive controller DMA limit derived from CAP.S64A.
 * @return Zero, -EIO for failed STOP, or allocation/argument errno. Failed STOP
 * quarantines and retains old backing; descriptor publication follows reset.
 */
int ahci_setup_port(struct ahci_hba_port *port, uint64_t mask)
{
    struct ahci_port_dma *dma = ahci_dma_find(port);
    if (ahci_stop_port(port)) {
        if (dma) dma->quarantined = true;
        return -5;
    }
    int result = ahci_dma_create(port, mask, &dma);
    if (result) return result;
    (void)ahci_dma_reset(dma, true);
    if (ahci_pending_command.port == port) ahci_pending_clear();
    uint64_t base = dma->descriptors.physical;
    port->clb = (uint32_t)base;
    port->clbu = (uint32_t)(base >> 32);
    base += AHCI_DMA_FIS_OFFSET;
    port->fb = (uint32_t)base;
    port->fbu = (uint32_t)(base >> 32);
    port->ie = 0; /* Polling driver: no unowned interrupt handler. */
    port->is = 0xffffffffu;
    port->serr = 0xffffffffu;
    ahci_memory_barrier();
    port->cmd |= AHCI_PORT_CMD_FRE;
    port->cmd |= AHCI_PORT_CMD_ST;
    return 0;
}

/** @brief Stop and release idle driver ports, retaining every failed-stop object.
 * @return Zero or -EIO; caller has closed admission and holds transport exclusion.
 * Successfully stopped ports are removed, failed ports remain for a later retry.
 */
int ahci_release_ports(void)
{
    int error = 0;
    struct ahci_port_dma *dma = ahci_dma_first();
    while (dma) {
        struct ahci_port_dma *next = dma->next;
        if (ahci_stop_port(dma->port)) {
            dma->quarantined = true;
            error = -5;
        } else {
            if (ahci_pending_command.port == dma->port) {
                if (dma->data.busy) (void)dma_buffer_end(&dma->data, NULL, true);
                ahci_pending_clear();
            }
            dma->port->clb = dma->port->clbu = 0;
            dma->port->fb = dma->port->fbu = 0;
            (void)ahci_dma_destroy(dma, true);
        }
        dma = next;
    }
    return error;
}

/** @brief Reset the exact failed port without freeing DMA still owned by hardware.
 * @param port Mapped failing port under transport exclusion.
 * @param attempt Diagnostic retry count, starting at one.
 * @return Zero or native setup error; missing context is -ENODEV.
 */
static int ahci_recover_port(struct ahci_hba_port *port, uint32_t attempt)
{
    struct ahci_port_dma *dma = ahci_dma_find(port);
    if (!dma) return -19;
    console_warn("[reliefnt] ahci recovering port attempt=%u ci=0x%x is=0x%x\n",
                 attempt, port->ci, port->is);
    return ahci_setup_port(port, dma->mask);
}

/** @brief Fill an ATA register FIS using a validated 48-bit LBA range.
 * @param fis Writable command FIS within idle driver-owned descriptor pages.
 * @param command ATA opcode.
 * @param lba Validated LBA, ignored for identify/flush.
 * @param count Sector count, zero for flush, one for identify.
 */
static void ahci_ata_fis(struct fis_reg_h2d *fis, uint8_t command,
                         uint64_t lba, uint32_t count)
{
    fis->fis_type = FIS_TYPE_REG_H2D; fis->c = 1; fis->command = command;
    fis->device = command == ATA_CMD_IDENTIFY_DEVICE ? 0 : (1u << 6);
    fis->lba0 = lba; fis->lba1 = lba >> 8; fis->lba2 = lba >> 16;
    fis->lba3 = lba >> 24; fis->lba4 = lba >> 32; fis->lba5 = lba >> 40;
    fis->countl = count; fis->counth = count >> 8;
}

/** @brief Prepare one real slot-zero command only after all slots are idle.
 * @param dma Stable idle per-port backing under transport exclusion.
 * @param operation Validated protocol operation.
 * @param lba Validated ATA/ATAPI address.
 * @param count Sector/block count, zero for nondata flush.
 * @param buffer Current valid kernel read/write bytes, NULL for flush.
 * @return Zero or native DMA errno; no MMIO or pending-call publication.
 */
static int ahci_prepare_command(struct ahci_port_dma *dma, enum ahci_operation operation,
                               uint64_t lba, uint32_t count, const void *buffer)
{
    uint16_t flags = sizeof(struct fis_reg_h2d) / sizeof(uint32_t);
    if (operation == AHCI_WRITE) flags |= 1u << 6;
    if (operation == AHCI_PACKET_READ || operation == AHCI_PACKET_CAPACITY) flags |= 1u << 5;
    if (operation != AHCI_FLUSH) {
        size_t bytes = operation==AHCI_PACKET_CAPACITY ? 8u : (size_t)count * (operation == AHCI_PACKET_READ ? ISO9660_BLOCK_SIZE : SECTOR_SIZE);
        int result = ahci_dma_prepare(dma, bytes,
            operation == AHCI_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE,
            operation == AHCI_WRITE ? buffer : NULL, flags);
        if (result) return result;
    } else {
        if (dma->data.busy || dma->quarantined) return -16;
        storage_memzero(ahci_dma_table(dma), sizeof(struct ahci_cmd_table));
        struct ahci_cmd_header *header = ahci_dma_header(dma);
        header->flags = flags; header->prdtl = 0; header->prdbc = 0;
    }
    struct ahci_cmd_table *table = ahci_dma_table(dma);
    struct fis_reg_h2d *fis = (void *)table->cfis;
    if (operation == AHCI_PACKET_READ || operation == AHCI_PACKET_CAPACITY) {
        fis->fis_type = FIS_TYPE_REG_H2D; fis->c = 1;
        fis->command = ATA_CMD_PACKET; fis->featurel = 1u;
        uint8_t *packet = table->acmd;
        packet[0] = operation==AHCI_PACKET_CAPACITY ? 0x25u : SCSI_CMD_READ10;
        packet[2] = lba >> 24; packet[3] = lba >> 16;
        packet[4] = lba >> 8; packet[5] = lba;
        if(operation==AHCI_PACKET_READ){packet[7] = count >> 8; packet[8] = count;}
    } else {
        uint8_t opcode = operation == AHCI_WRITE ? ATA_CMD_WRITE_DMA_EXT :
            operation == AHCI_IDENTIFY ? ATA_CMD_IDENTIFY_DEVICE :
            operation == AHCI_FLUSH ? 0xe7 : ATA_CMD_READ_DMA_EXT;
        ahci_ata_fis(fis, opcode, lba, count);
    }
    return 0;
}

/** @brief Resume/submit a validated request and deliver only to its current caller.
 * @param port Stable mapped port under transport exclusion.
 * @param operation Protocol operation.
 * @param lba Validated command address.
 * @param count Validated transfer count.
 * @param buffer Current stable kernel buffer, NULL only for flush.
 * @return Zero, -EAGAIN preserving pending ownership, or native errno. A different
 * resumed call drains/discards the old result; it never touches a saved pointer.
 */
static int ahci_transfer_once(struct ahci_hba_port *port, enum ahci_operation operation,
                              uint64_t lba, uint32_t count, void *buffer)
{
    if (ahci_pending_command.active) {
        int matches = ahci_pending_matches(port, lba, count, buffer, operation);
        void *destination = matches && operation != AHCI_WRITE && operation != AHCI_FLUSH ? buffer : NULL;
        int result = ahci_pending_poll(destination);
        if (matches || result) return result;
    }
    struct ahci_port_dma *dma = ahci_dma_find(port);
    if (!dma) return -19;
    if (dma->quarantined) return -5;
    int result = ahci_wait_idle(port);
    if (!result) result = ahci_wait_cmd_slot(port);
    if (result) return result;
    result = ahci_prepare_command(dma, operation, lba, count, buffer);
    if (result) return result;
    ahci_pending_command = (struct ahci_pending_command){
        .port = port, .dma = dma, .lba = lba, .sector_count = count,
        .buffer = buffer, .start_tick = time_ticks(), .owner_pid = sched_current_pid(),
        .operation = operation, .write = operation == AHCI_WRITE || operation == AHCI_FLUSH,
        .active = 1};
    port->is = 0xffffffffu;
    ahci_memory_barrier();
    port->ci = 1u;
    return ahci_pending_poll(operation == AHCI_WRITE || operation == AHCI_FLUSH ? NULL : buffer);
}

/** @brief Retry an exact validated request after bounded per-port STOP/recovery.
 * @param port Stable mapped port under transport exclusion.
 * @param operation Protocol operation.
 * @param lba Validated command address.
 * @param count Validated transfer count.
 * @param buffer Stable kernel bytes until return/EAGAIN; no retained dereference.
 * @return Zero, -EAGAIN, or final native error. Retries keep exact write bytes.
 */
static int ahci_transfer_retry(struct ahci_hba_port *port, enum ahci_operation operation,
                               uint64_t lba, uint32_t count, void *buffer)
{
    int result = -5;
    for (uint32_t attempt = 0; attempt < AHCI_IO_RETRY_COUNT; ++attempt) {
        result = ahci_transfer_once(port, operation, lba, count, buffer);
        if (result >= 0 || result == -RELIEFOS_EAGAIN) return result;
        if (ahci_recover_port(port, attempt + 1u)) break;
    }
    return result;
}

/** @brief Read ATA sectors into current kernel memory through owned DMA pages.
 * @param port Stable mapped port under transport exclusion.
 * @param lba First 48-bit ATA sector address.
 * @param sector_count Nonzero sectors, at most AHCI_MAX_SECTORS.
 * @param buffer Writable kernel extent sector_count*512, retained by current call.
 * @return Zero, -EINVAL for range/buffer, -EAGAIN for resumable I/O, or native errno.
 */
int ahci_read_lba_retry(struct ahci_hba_port *port, uint64_t lba,
                        uint32_t sector_count, void *buffer)
{
    if (!port || !buffer || !sector_count || sector_count > AHCI_MAX_SECTORS ||
        lba >= (1ull << 48) || sector_count - 1u > ((1ull << 48) - 1u) - lba) return -22;
    return ahci_transfer_retry(port, AHCI_READ, lba, sector_count, buffer);
}

/** @brief Write ATA sectors from a borrowed CPU buffer via owned coherent pages.
 * @param port Stable mapped port under transport exclusion.
 * @param lba First 48-bit ATA sector address.
 * @param sector_count Nonzero sectors, at most AHCI_MAX_SECTORS.
 * @param buffer Readable kernel extent sector_count*512, copied before submission.
 * @return Zero, -EINVAL for range/buffer, -EAGAIN, or native transport errno.
 */
int ahci_write_lba_retry(struct ahci_hba_port *port, uint64_t lba,
                         uint32_t sector_count, const void *buffer)
{
    if (!port || !buffer || !sector_count || sector_count > AHCI_MAX_SECTORS ||
        lba >= (1ull << 48) || sector_count - 1u > ((1ull << 48) - 1u) - lba) return -22;
    return ahci_transfer_retry(port, AHCI_WRITE, lba, sector_count, (void *)buffer);
}

/** @brief Read ATAPI READ(10) blocks through the port's distinct DMA backing.
 * @param port Stable mapped ATAPI port under transport exclusion.
 * @param lba First 32-bit logical block.
 * @param block_count Nonzero 2048-byte blocks, at most 32.
 * @param buffer Writable block_count*2048 kernel bytes, valid for current call.
 * @return Zero, -EINVAL for range/buffer, -EAGAIN, or native transport errno.
 */
int ahci_read_atapi_blocks_retry(struct ahci_hba_port *port, uint64_t lba,
                                 uint32_t block_count, void *buffer)
{
    if (!port || !buffer || !block_count || block_count > 32 || lba > UINT32_MAX ||
        block_count - 1u > UINT32_MAX - lba) return -22;
    return ahci_transfer_retry(port, AHCI_PACKET_READ, lba, block_count, buffer);
}

/** @brief Fetch ATA IDENTIFY data without exposing CPU pointers to the controller.
 * @param port Stable ATA port under transport exclusion.
 * @param buffer Writable kernel extent of at least 512 bytes, current call only.
 * @return Zero, -EINVAL, -EAGAIN, or native transport errno.
 */
int ahci_identify_device(struct ahci_hba_port *port, void *buffer)
{
    if (!port || !buffer) return -22;
    return ahci_transfer_retry(port, AHCI_IDENTIFY, 0, 1, buffer);
}

/** @brief Flush the device's volatile cache using the same owned command engine.
 * @param port Stable ATA port under transport exclusion; no data DMA is submitted.
 * @return Zero, -ENODEV for absent port, -EAGAIN, or native transport errno.
 */
int ahci_flush_cache(struct ahci_hba_port *port)
{
    if (!port) return -19;
    return ahci_transfer_retry(port, AHCI_FLUSH, 0, 0, NULL);
}

/** @brief Fetch standard READ CAPACITY(10) independently of any filesystem parser.
 * @param port Stable ATAPI port under transport exclusion.
 * @param response Writable 8-byte big-endian last-LBA/block-size response.
 * @return Zero or native transport errno; owned DMA remains separate from CPU
 * response storage. Caller handles unsupported UINT32_MAX capacity explicitly.
 */
int ahci_read_capacity(struct ahci_hba_port *port,uint8_t response[8])
{
    if(!port || !response)return -22;
    return ahci_transfer_retry(port,AHCI_PACKET_CAPACITY,0,1,response);
}

/** @brief Release one port's backing after its controller proved both engines STOP.
 * @param port Exclusively quiesced mapped port; no callback/admission remains.
 * Caller holds transport exclusion. Consumes owned DMA pages, discards pending
 * data without dereferencing saved task memory, and clears descriptor addresses.
 * No hardware restart, allocation or registry callback occurs.
 */
void ahci_release_stopped_port(struct ahci_hba_port *port)
{
    struct ahci_port_dma *dma=ahci_dma_find(port);if(!dma)return;
    if(ahci_pending_command.port==port)ahci_pending_clear();
    if(dma->data.busy)(void)dma_buffer_end(&dma->data,NULL,true);
    port->clb=port->clbu=port->fb=port->fbu=0;
    (void)ahci_dma_destroy(dma,true);
}
