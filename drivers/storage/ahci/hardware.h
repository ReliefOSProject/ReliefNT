/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_AHCI_HARDWARE_H
#define RELIEFNT_AHCI_HARDWARE_H
#include <stdint.h>
#include "../ata.h"
#define AHCI_GHC_AE 0x80000000u
#define AHCI_PORT_CMD_ST 0x0001u
#define AHCI_PORT_CMD_FRE 0x0010u
#define AHCI_PORT_CMD_FR 0x4000u
#define AHCI_PORT_CMD_CR 0x8000u
#define AHCI_PORT_TFD_BSY 0x80u
#define AHCI_PORT_TFD_DRQ 0x08u
#define AHCI_PORT_IS_TFES 0x40000000u
#define AHCI_PORT_SIG_ATA 0x00000101u
#define AHCI_PORT_SIG_ATAPI 0xeb140101u
#define AHCI_PORT_DET_PRESENT 0x3u
#define AHCI_PORT_IPM_ACTIVE 0x1u

#define FIS_TYPE_REG_H2D 0x27u


#define AHCI_CMDH_PRDTL 1u
#define AHCI_MAX_SECTORS 64u
/* AHCI completion is polled synchronously. Keep enough headroom for a busy
 * virtual disk: a too-short limit abandons a live command and turns the next
 * application image read into a spurious -EIO. Filesystem syscalls already
 * bound each transfer, which is the responsiveness control for this path. */
#define AHCI_WAIT_SPINS 40000000u
/* Most virtual AHCI reads complete just after the command is submitted.  In
 * asynchronous syscall context, poll briefly before yielding a whole PIT
 * tick: this keeps application image and UI-resource loading fast without
 * restoring the old unbounded Ring-0 busy wait on a slow disk. */
#define AHCI_ASYNC_FAST_POLL_SPINS 8192u
/* A pending asynchronous command may be polled by transparently resumed
 * syscalls for at most five seconds. A failed virtual controller must not
 * leave a task blocked forever. */
#define AHCI_ASYNC_TIMEOUT_TICKS (5u * 100u)
/* Retrying an individual sector command is safe: all callers resubmit the
 * same LBA range with the same payload.  This contains short-lived virtual
 * AHCI controller faults without replaying a higher-level FAT operation. */
#define AHCI_IO_RETRY_COUNT 3u

struct __attribute__((packed)) ahci_hba_port {
    volatile uint32_t clb;
    volatile uint32_t clbu;
    volatile uint32_t fb;
    volatile uint32_t fbu;
    volatile uint32_t is;
    volatile uint32_t ie;
    volatile uint32_t cmd;
    volatile uint32_t reserved0;
    volatile uint32_t tfd;
    volatile uint32_t sig;
    volatile uint32_t ssts;
    volatile uint32_t sctl;
    volatile uint32_t serr;
    volatile uint32_t sact;
    volatile uint32_t ci;
    volatile uint32_t sntf;
    volatile uint32_t fbs;
    volatile uint32_t reserved1[11];
    volatile uint32_t vendor[4];
};

struct __attribute__((packed)) ahci_hba_mem {
    volatile uint32_t cap;
    volatile uint32_t ghc;
    volatile uint32_t is;
    volatile uint32_t pi;
    volatile uint32_t vs;
    volatile uint32_t ccc_ctl;
    volatile uint32_t ccc_pts;
    volatile uint32_t em_loc;
    volatile uint32_t em_ctl;
    volatile uint32_t cap2;
    volatile uint32_t bohc;
    uint8_t reserved[0xa0 - 0x2c];
    uint8_t vendor[0x100 - 0xa0];
    struct ahci_hba_port ports[32];
};

struct __attribute__((packed)) ahci_cmd_header {
    uint16_t flags;
    uint16_t prdtl;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t reserved[4];
};

struct __attribute__((packed)) ahci_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t reserved0;
    uint32_t dbc;
};

struct __attribute__((packed)) ahci_cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct ahci_prdt_entry prdt[1];
};

struct __attribute__((packed)) fis_reg_h2d {
    uint8_t fis_type;
    uint8_t pmport : 4;
    uint8_t reserved0 : 3;
    uint8_t c : 1;
    uint8_t command;
    uint8_t featurel;
    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;
    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;
    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;
    uint8_t reserved1[4];
};


#endif /* RELIEFNT_AHCI_HARDWARE_H */
