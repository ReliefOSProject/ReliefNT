/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_I225_INTERNAL_H
#define RELIEFNT_I225_INTERNAL_H

#include <reliefos/driver.h>
#include <reliefnt/lock.h>

#define I225_CTRL          0x00000u
#define I225_STATUS        0x00008u
#define I225_EECD          0x00010u
#define I225_MDIC          0x00020u
#define I225_SWSM          0x05b50u
#define I225_SW_FW_SYNC    0x05b5cu
#define I225_EERD          0x12014u
#define I225_ICR           0x01500u
#define I225_IMC           0x0150cu
#define I225_RCTL          0x00100u
#define I225_TCTL          0x00400u
#define I225_SRRCTL0       0x0c00cu
#define I225_RDBAL0        0x0c000u
#define I225_RDBAH0        0x0c004u
#define I225_RDLEN0        0x0c008u
#define I225_RDH0          0x0c010u
#define I225_RDT0          0x0c018u
#define I225_RXDCTL0       0x0c028u
#define I225_TDBAL0        0x0e000u
#define I225_TDBAH0        0x0e004u
#define I225_TDLEN0        0x0e008u
#define I225_TDH0          0x0e010u
#define I225_TDT0          0x0e018u
#define I225_TXDCTL0       0x0e028u
#define I225_RAL0          0x05400u
#define I225_RAH0          0x05404u
#define I225_MTA           0x05200u
#define I225_MMIO_BYTES    0x13000u

#define I225_CTRL_RST      (1u << 26)
#define I225_CTRL_SLU      (1u << 6)
#define I225_CTRL_GIO_DIS  (1u << 2)
#define I225_STATUS_LINK   (1u << 1)
#define I225_STATUS_MASTER (1u << 19)
#define I225_EECD_AUTO_RD  (1u << 9)
#define I225_SWSM_SMBI     (1u << 0)
#define I225_SWSM_SWESMBI  (1u << 1)
#define I225_SWFW_EEP      (1u << 0)
#define I225_SWFW_PHY0     (1u << 1)
#define I225_EERD_START    (1u << 0)
#define I225_EERD_DONE     (1u << 1)
#define I225_EERD_ADDR(a)  ((uint32_t)(a) << 2)
#define I225_EERD_DATA(v)  ((uint16_t)((v) >> 16))
#define I225_RAH_AV        (1u << 31)
#define I225_RCTL_EN       (1u << 1)
#define I225_RCTL_MPE      (1u << 4)
#define I225_RCTL_BAM      (1u << 15)
#define I225_RCTL_SECRC    (1u << 26)
#define I225_TCTL_EN       (1u << 1)
#define I225_TCTL_PSP      (1u << 3)
#define I225_SRRCTL_ADV    (1u << 25)
#define I225_SRRCTL_2048   2u
#define I225_RXDCTL_ENABLE (1u << 25)
#define I225_TXDCTL_ENABLE (1u << 25)
#define I225_TX_DTYP_DATA  0x00300000u
#define I225_TX_DEXT       (1u << 29)
#define I225_TX_IFCS       (1u << 25)
#define I225_TX_EOP        (1u << 24)
#define I225_TX_RS         (1u << 27)
#define I225_TX_DONE       (1u << 0)
#define I225_RX_DD         (1u << 0)
#define I225_RX_EOP        (1u << 1)
#define I225_RX_ERR_MASK   0xff000000u
#define I225_MDIC_READY    (1u << 28)
#define I225_MDIC_ERROR    (1u << 30)

#define I225_RX_COUNT      256u
#define I225_TX_COUNT      64u
#define I225_BUFFER_BYTES  2048u
#define I225_MAX_FRAME     1518u
#define I225_RX_PAGES      (I225_RX_COUNT * I225_BUFFER_BYTES / 4096u)
#define I225_TX_PAGES      (I225_TX_COUNT * I225_BUFFER_BYTES / 4096u)

struct i225_rx_desc {
    union {
        struct { uint64_t packet, header; } read;
        struct {
            uint32_t packet_info, rss;
            uint32_t status_error;
            uint16_t length, vlan;
        } writeback;
    };
} __attribute__((packed));

struct i225_tx_desc {
    union {
        struct { uint64_t address; uint32_t command, offload; } read;
        struct { uint64_t reserved; uint32_t sequence, status; } writeback;
    };
} __attribute__((packed));

_Static_assert(sizeof(struct i225_rx_desc) == 16, "I225 advanced RX descriptor");
_Static_assert(sizeof(struct i225_tx_desc) == 16, "I225 advanced TX descriptor");
_Static_assert(I225_RX_COUNT * sizeof(struct i225_rx_desc) == 4096, "I225 RX ring page");
_Static_assert(I225_TX_COUNT * sizeof(struct i225_tx_desc) <= 4096, "I225 TX ring page");

struct i225_device {
    struct i225_device *next;
    const struct reliefos_driver_kernel_api *api;
    struct reliefos_driver_pci_device pci;
    volatile uint8_t *registers;
    uint16_t original_command;
    bool command_saved, present, active, dropping_fragments;
    uint8_t mac[6];
    uint64_t rx_physical, tx_physical, rx_data_physical, tx_data_physical;
    volatile struct i225_rx_desc *rx;
    volatile struct i225_tx_desc *tx;
    uint8_t *rx_data, *tx_data;
    unsigned rx_next, tx_next, tx_clean;
    struct kernel_spinlock rx_lock, tx_lock;
};

static inline uint32_t i225_read(const struct i225_device *d, unsigned offset)
{ return *(volatile uint32_t *)(d->registers + offset); }

static inline void i225_write(struct i225_device *d, unsigned offset, uint32_t value)
{ *(volatile uint32_t *)(d->registers + offset) = value; }

static inline void i225_flush(struct i225_device *d)
{ (void)i225_read(d, I225_STATUS); }

int i225_hardware_start(struct i225_device *d);
int i225_hardware_stop(struct i225_device *d);
int i225_activate_busmaster(struct i225_device *d);
int i225_allocate_rings(struct i225_device *d);
void i225_configure_rings(struct i225_device *d);
void i225_enable_rings(struct i225_device *d);
void i225_free_rings(struct i225_device *d);
int i225_send(struct i225_device *d, const void *frame, uint32_t length);
int i225_receive(struct i225_device *d, void *frame, uint32_t capacity, uint32_t *length);

#endif
