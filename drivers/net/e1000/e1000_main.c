#include <reliefos/driver.h>

#include <reliefnt/pci.h>
#include <reliefnt/heap.h>
#include <reliefnt/net_device.h>

static const struct reliefos_driver_kernel_api *kernel_api;

struct e1000_state;
static void e1000_release_rings(struct e1000_state *d);

static void e1000_log(enum console_log_level level, const char *message)
{
    if (kernel_api && kernel_api->console_log)
        kernel_api->console_log(level, "[e1000] %s\n", message);
}

#define mm_alloc_pages(pages) kernel_api->alloc_pages((pages))
#define mm_free_pages(address, pages) kernel_api->free_pages((address), (pages))
#define mm_phys_to_virt(address) kernel_api->phys_to_virt((address))
#define pci_config_read16(bus, slot, function, offset) \
    kernel_api->pci_read16((bus), (slot), (function), (offset))
#define pci_config_write16(bus, slot, function, offset, value) \
    kernel_api->pci_write16((bus), (slot), (function), (offset), (value))
#define pci_config_read32(bus, slot, function, offset) \
    kernel_api->pci_read32((bus), (slot), (function), (offset))

#define E1000_VENDOR_INTEL 0x8086u
#define E1000_DEVICE_82540EM 0x100eu
#define E1000_DEVICE_82545EM 0x100fu
#define E1000_DEVICE_82543GC 0x1004u
#define E1000_DEVICE_82544GC 0x100du

#define E1000_REG_CTRL 0x0000u
#define E1000_REG_STATUS 0x0008u
#define E1000_REG_EERD 0x0014u
#define E1000_REG_ICR 0x00c0u
#define E1000_REG_IMC 0x00d8u
#define E1000_REG_RCTL 0x0100u
#define E1000_REG_TCTL 0x0400u
#define E1000_REG_TIPG 0x0410u
#define E1000_REG_RDBAL 0x2800u
#define E1000_REG_RDBAH 0x2804u
#define E1000_REG_RDLEN 0x2808u
#define E1000_REG_RDH 0x2810u
#define E1000_REG_RDT 0x2818u
#define E1000_REG_TDBAL 0x3800u
#define E1000_REG_TDBAH 0x3804u
#define E1000_REG_TDLEN 0x3808u
#define E1000_REG_TDH 0x3810u
#define E1000_REG_TDT 0x3818u
#define E1000_REG_MTA 0x5200u
#define E1000_REG_RAL 0x5400u
#define E1000_REG_RAH 0x5404u
#define E1000_REGISTER_BYTES (E1000_REG_RAH + sizeof(uint32_t))
#define E1000_CTRL_SLU (1u << 6)
#define E1000_CTRL_RST (1u << 26)

#define E1000_RCTL_EN (1u << 1)
#define E1000_RCTL_UPE (1u << 3)
#define E1000_RCTL_MPE (1u << 4)
#define E1000_RCTL_BAM (1u << 15)
#define E1000_RCTL_BSIZE_2048 (0u << 16)
#define E1000_RCTL_SECRC (1u << 26)
#define E1000_TCTL_EN (1u << 1)
#define E1000_TCTL_PSP (1u << 3)
#define E1000_RAH_AV (1u << 31)
#define E1000_RX_STATUS_DD 0x01u
#define E1000_RX_STATUS_EOP 0x02u
#define E1000_TX_STATUS_DD 0x01u
#define E1000_TX_CMD_EOP 0x01u
#define E1000_TX_CMD_IFCS 0x02u
#define E1000_TX_CMD_RS 0x08u

#define PCI_COMMAND_IO 0x0001u
#define PCI_COMMAND_MEMORY 0x0002u
#define PCI_COMMAND_BUS_MASTER 0x0004u

/* Linux e1000's default accommodates receive bursts while a task is running. */
#define E1000_RX_COUNT 256u
#define E1000_TX_COUNT 16u
#define E1000_BUFFER_SIZE 2048u
#define E1000_TX_WAIT_SPINS 250000u

struct e1000_rx_desc {
    uint64_t addr;
    uint16_t length;
    uint16_t checksum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} __attribute__((packed));

struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

_Static_assert(E1000_RX_COUNT * sizeof(struct e1000_rx_desc) <= 4096,
               "RX descriptors must fit their allocated page");

struct module_e1000_info {
    uint32_t present;
    uint32_t active;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
    uint8_t mac[6];
};

struct e1000_state {
    struct e1000_state *next;
    uint32_t present;
    uint32_t active;
    uint32_t pci_command_valid;
    uint16_t pci_command_original;
    struct pci_device pci;
    uintptr_t mmio_base;
    volatile struct e1000_rx_desc *rx;
    volatile struct e1000_tx_desc *tx;
    uint64_t rx_phys;
    uint64_t tx_phys;
    uint64_t rx_buf_phys[E1000_RX_COUNT];
    uint64_t tx_buf_phys[E1000_TX_COUNT];
    uint8_t *rx_buf[E1000_RX_COUNT];
    uint8_t *tx_buf[E1000_TX_COUNT];
    uint32_t rx_index;
    uint32_t tx_tail;
    uint8_t mac[6];
};

static struct e1000_state *devices;

static void e1000_cpu_relax(void)
{
    __asm__ volatile("pause");
}

static void e1000_memzero(void *dst, uint32_t len)
{
    uint8_t *p = (uint8_t *)dst;
    while (len--) {
        *p++ = 0;
    }
}

static void e1000_memcpy(void *dst, const void *src, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--) {
        *d++ = *s++;
    }
}

static uint32_t e1000_reg_read(struct e1000_state *d, uint32_t offset)
{
    return *(volatile uint32_t *)(d->mmio_base + offset);
}

static void e1000_reg_write(struct e1000_state *d, uint32_t offset, uint32_t value)
{
    *(volatile uint32_t *)(d->mmio_base + offset) = value;
}

/** @brief Read the full physical address of an assigned memory BAR0.
 * @param dev Borrowed PCI identity. @return Physical BAR base, or zero for I/O.
 * The provider validates and maps the extent before any register is accessed.
 */
static uint64_t e1000_bar0_mmio(const struct pci_device *dev)
{
    uint32_t bar0 = pci_config_read32(dev->bus, dev->slot, dev->function, 0x10);
    uint64_t addr;
    if (bar0 & 0x1u) {
        return 0;
    }
    addr = (uint64_t)(bar0 & ~0x0fu);
    if ((bar0 & 0x6u) == 0x4u) {
        uint32_t high = pci_config_read32(dev->bus, dev->slot, dev->function, 0x14);
        addr |= (uint64_t)high << 32;
    }
    return addr;
}

static int e1000_read_mac(struct e1000_state *d)
{
    uint32_t ral = e1000_reg_read(d, E1000_REG_RAL);
    uint32_t rah = e1000_reg_read(d, E1000_REG_RAH);
    d->mac[0] = (uint8_t)ral;
    d->mac[1] = (uint8_t)(ral >> 8);
    d->mac[2] = (uint8_t)(ral >> 16);
    d->mac[3] = (uint8_t)(ral >> 24);
    d->mac[4] = (uint8_t)rah;
    d->mac[5] = (uint8_t)(rah >> 8);
    if (!(rah & E1000_RAH_AV) || (d->mac[0] & 1u) || !(ral | (rah & 0xffff))) {
        for (unsigned word = 0; word < 3; ++word) {
            e1000_reg_write(d, E1000_REG_EERD, 1u | (word << 8));
            uint32_t value = 0;
            for (unsigned spin = 0; spin < E1000_TX_WAIT_SPINS; ++spin) {
                value = e1000_reg_read(d, E1000_REG_EERD);
                if (value & (1u << 4)) break;
                e1000_cpu_relax();
            }
            if (!(value & (1u << 4))) return -1;
            d->mac[word * 2] = (uint8_t)(value >> 16);
            d->mac[word * 2 + 1] = (uint8_t)(value >> 24);
        }
    }
    if ((d->mac[0] & 1u) || !(d->mac[0] | d->mac[1] |
        d->mac[2] | d->mac[3] | d->mac[4] | d->mac[5])) return -1;
    ral = (uint32_t)d->mac[0] |
          ((uint32_t)d->mac[1] << 8) |
          ((uint32_t)d->mac[2] << 16) |
          ((uint32_t)d->mac[3] << 24);
    rah = (uint32_t)d->mac[4] |
          ((uint32_t)d->mac[5] << 8) |
          E1000_RAH_AV;
    e1000_reg_write(d, E1000_REG_RAL, ral);
    e1000_reg_write(d, E1000_REG_RAH, rah);
    return 0;
}

/** @brief Allocate CPU-addressable DMA rings while retaining their bus addresses.
 * @return Zero when all descriptor and packet pages are ready, otherwise -1
 * after releasing every partially allocated page.
 * Task-context initialization before DMA is enabled on the PCI function.
 */
static int e1000_alloc_rings(struct e1000_state *d)
{
    d->rx_phys = mm_alloc_pages(1);
    d->tx_phys = mm_alloc_pages(1);
    if (!d->rx_phys || !d->tx_phys) {
        e1000_release_rings(d);
        return -1;
    }
    d->rx = (volatile struct e1000_rx_desc *)mm_phys_to_virt(d->rx_phys);
    d->tx = (volatile struct e1000_tx_desc *)mm_phys_to_virt(d->tx_phys);
    if (!d->rx || !d->tx) {
        e1000_release_rings(d);
        return -1;
    }
    e1000_memzero((void *)d->rx, 4096);
    e1000_memzero((void *)d->tx, 4096);

    for (uint32_t i = 0; i < E1000_RX_COUNT; ++i) {
        uint64_t phys = mm_alloc_pages(1);
        if (!phys) {
            e1000_release_rings(d);
            return -1;
        }
        d->rx_buf[i] = (uint8_t *)mm_phys_to_virt(phys);
        if (!d->rx_buf[i]) {
            mm_free_pages(phys, 1);
            e1000_release_rings(d);
            return -1;
        }
        d->rx_buf_phys[i] = phys;
        e1000_memzero(d->rx_buf[i], E1000_BUFFER_SIZE);
        d->rx[i].addr = phys;
        d->rx[i].status = 0;
    }
    for (uint32_t i = 0; i < E1000_TX_COUNT; ++i) {
        uint64_t phys = mm_alloc_pages(1);
        if (!phys) {
            e1000_release_rings(d);
            return -1;
        }
        d->tx_buf[i] = (uint8_t *)mm_phys_to_virt(phys);
        if (!d->tx_buf[i]) {
            mm_free_pages(phys, 1);
            e1000_release_rings(d);
            return -1;
        }
        d->tx_buf_phys[i] = phys;
        e1000_memzero(d->tx_buf[i], E1000_BUFFER_SIZE);
        d->tx[i].addr = phys;
        d->tx[i].status = E1000_TX_STATUS_DD;
    }
    return 0;
}

static void e1000_configure_rx_tx(struct e1000_state *d)
{
    e1000_reg_write(d, E1000_REG_RCTL, 0);
    e1000_reg_write(d, E1000_REG_TCTL, 0);
    e1000_reg_write(d, E1000_REG_IMC, 0xffffffffu);
    (void)e1000_reg_read(d, E1000_REG_ICR);

    for (uint32_t i = 0; i < 128; ++i) {
        e1000_reg_write(d, E1000_REG_MTA + i * 4u, 0);
    }

    e1000_reg_write(d, E1000_REG_RDBAL, (uint32_t)d->rx_phys);
    e1000_reg_write(d, E1000_REG_RDBAH, (uint32_t)(d->rx_phys >> 32));
    e1000_reg_write(d, E1000_REG_RDLEN, E1000_RX_COUNT * sizeof(struct e1000_rx_desc));
    e1000_reg_write(d, E1000_REG_RDH, 0);
    e1000_reg_write(d, E1000_REG_RDT, E1000_RX_COUNT - 1u);
    d->rx_index = 0;

    e1000_reg_write(d, E1000_REG_TDBAL, (uint32_t)d->tx_phys);
    e1000_reg_write(d, E1000_REG_TDBAH, (uint32_t)(d->tx_phys >> 32));
    e1000_reg_write(d, E1000_REG_TDLEN, E1000_TX_COUNT * sizeof(struct e1000_tx_desc));
    e1000_reg_write(d, E1000_REG_TDH, 0);
    e1000_reg_write(d, E1000_REG_TDT, 0);
    d->tx_tail = 0;

    e1000_reg_write(d, E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));
    e1000_reg_write(d, E1000_REG_TCTL,
                    E1000_TCTL_EN | E1000_TCTL_PSP |
                        (0x10u << 4) | (0x40u << 12));
    e1000_reg_write(d, E1000_REG_RCTL,
                    E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                        E1000_RCTL_BAM | E1000_RCTL_BSIZE_2048 |
                        E1000_RCTL_SECRC);
}

/** @brief Initialize a selected controller through an owned kernel MMIO alias.
 * @param d Per-function state with the PCI identity already selected.
 * @return None. On success, the device owns its mapping until fini; on failure,
 * the partially initialized state remains available for fini cleanup.
 * Task-context driver initialization; all PCI decode and bus-master bits are
 * disabled while the BAR is mapped. DMA buffer addresses remain physical for
 * device programming.
 */
static void e1000_hardware_init(struct e1000_state *d)
{
    struct pci_device dev = d->pci;
    uint64_t mmio_phys;
    void *mmio_alias;
    uint16_t command;
    d->present = 1;
    d->pci = dev;
    command = pci_config_read16(dev.bus, dev.slot, dev.function, 0x04);
    d->pci_command_original = command;
    d->pci_command_valid = 1;
    command &= (uint16_t)~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY |
                           PCI_COMMAND_BUS_MASTER);
    pci_config_write16(dev.bus, dev.slot, dev.function, 0x04, command);

    mmio_phys = e1000_bar0_mmio(&dev);
    if (!mmio_phys) {
        e1000_log(CONSOLE_LOG_ERROR, "BAR0 MMIO is unassigned or is an I/O BAR");
        return;
    }
    mmio_alias = kernel_api->map_mmio(mmio_phys, E1000_REGISTER_BYTES);
    if (!mmio_alias) {
        e1000_log(CONSOLE_LOG_ERROR, "BAR0 MMIO mapping failed");
        return;
    }
    d->mmio_base = (uintptr_t)mmio_alias;

    command = pci_config_read16(dev.bus, dev.slot, dev.function, 0x04);
    command |= PCI_COMMAND_MEMORY;
    pci_config_write16(dev.bus, dev.slot, dev.function, 0x04, command);

    e1000_reg_write(d, E1000_REG_IMC, 0xffffffffu);
    e1000_reg_write(d, E1000_REG_RCTL, 0);
    e1000_reg_write(d, E1000_REG_TCTL, 0);
    (void)e1000_reg_read(d, E1000_REG_STATUS);
    kernel_api->sleep_ms(10);
    e1000_reg_write(d, E1000_REG_CTRL, e1000_reg_read(d, E1000_REG_CTRL) | E1000_CTRL_RST);
    kernel_api->sleep_ms(20);
    if (e1000_reg_read(d, E1000_REG_CTRL) & E1000_CTRL_RST) {
        e1000_log(CONSOLE_LOG_ERROR, "controller reset timed out");
        return;
    }
    e1000_reg_write(d, E1000_REG_IMC, 0xffffffffu);
    (void)e1000_reg_read(d, E1000_REG_ICR);
    if (e1000_read_mac(d) < 0) {
        e1000_log(CONSOLE_LOG_ERROR, "no valid hardware MAC in RAR or EEPROM");
        return;
    }
    e1000_reg_write(d, E1000_REG_CTRL, e1000_reg_read(d, E1000_REG_CTRL) | E1000_CTRL_SLU);
    if (e1000_alloc_rings(d) < 0) {
        e1000_log(CONSOLE_LOG_ERROR, "ring allocation failed");
        return;
    }
    command = pci_config_read16(dev.bus, dev.slot, dev.function, 0x04);
    command |= PCI_COMMAND_MEMORY | PCI_COMMAND_BUS_MASTER;
    command &= (uint16_t)~PCI_COMMAND_IO;
    pci_config_write16(dev.bus, dev.slot, dev.function, 0x04, command);
    e1000_configure_rx_tx(d);
    d->active = 1;
    e1000_log(CONSOLE_LOG_INFO, "descriptor rings initialized; hardware MAC loaded");
}

static int e1000_device_is_ready(void *context)
{
    struct e1000_state *d=context;
    return d->active && (e1000_reg_read(d, E1000_REG_STATUS) & (1u << 1));
}

static const uint8_t *e1000_device_mac(void *context)
{
    struct e1000_state *d=context;
    return d->mac;
}

static int e1000_device_send(void *context, const void *frame, uint32_t len)
{
    struct e1000_state *d=context;
    uint32_t index;
    uint32_t next;
    if (!d->active || !frame || len == 0 || len > E1000_BUFFER_SIZE) {
        return -1;
    }
    index = d->tx_tail;
    for (uint32_t spin = 0; spin < E1000_TX_WAIT_SPINS; ++spin) {
        if (d->tx[index].status & E1000_TX_STATUS_DD) {
            break;
        }
        if (spin + 1u == E1000_TX_WAIT_SPINS) {
            return -1;
        }
        e1000_cpu_relax();
    }
    e1000_memcpy(d->tx_buf[index], frame, len);
    d->tx[index].length = (uint16_t)len;
    d->tx[index].cso = 0;
    d->tx[index].cmd = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS | E1000_TX_CMD_RS;
    d->tx[index].status = 0;
    d->tx[index].css = 0;
    d->tx[index].special = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    next = (index + 1u) % E1000_TX_COUNT;
    d->tx_tail = next;
    e1000_reg_write(d, E1000_REG_TDT, next);
    for (uint32_t spin = 0; spin < E1000_TX_WAIT_SPINS; ++spin) {
        if (d->tx[index].status & E1000_TX_STATUS_DD) {
            return 0;
        }
        e1000_cpu_relax();
    }
    return -1;
}

static int e1000_device_poll(void *context, void *frame, uint32_t capacity, uint32_t *out_len)
{
    struct e1000_state *d=context;
    uint32_t index;
    uint32_t len;
    uint8_t status;
    int receive_error = 0;
    if (out_len) {
        *out_len = 0;
    }
    if (!d->active || !frame || !capacity) {
        return -1;
    }
    index = d->rx_index;
    status = d->rx[index].status;
    if ((status & E1000_RX_STATUS_DD) == 0) {
        return 0;
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    len = d->rx[index].length;
    if ((status & E1000_RX_STATUS_EOP) == 0 || d->rx[index].errors ||
        len > E1000_BUFFER_SIZE || len > capacity) {
        receive_error = len > capacity ? -90 : -74;
        len = 0;
    }
    if (len > capacity) {
        len = capacity;
    }
    if (len) {
        e1000_memcpy(frame, d->rx_buf[index], len);
    }
    d->rx[index].length = 0;
    d->rx[index].checksum = 0;
    d->rx[index].status = 0;
    d->rx[index].errors = 0;
    d->rx[index].special = 0;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    e1000_reg_write(d, E1000_REG_RDT, index);
    d->rx_index = (index + 1u) % E1000_RX_COUNT;
    if (out_len) {
        *out_len = len;
    }
    return receive_error ? receive_error : len ? 1 : 0;
}

static void e1000_device_get_info(struct e1000_state *d, struct module_e1000_info *info)
{
    if (!info) {
        return;
    }
    *info = (struct module_e1000_info){
        .present = d->present,
        .active = e1000_device_is_ready(d) != 0,
        .vendor_id = d->pci.vendor_id,
        .device_id = d->pci.device_id,
        .bus = d->pci.bus,
        .slot = d->pci.slot,
        .function = d->pci.function,
    };
    for (uint32_t i = 0; i < 6; ++i) {
        info->mac[i] = d->mac[i];
    }
}

/** @brief Copy this pinned device's identity for the network registry.
 * @param context Pinned controller. @param out Caller-owned nullable output.
 */
static void e1000_driver_get_info(void *context, struct e1000_info *out)
{
    struct module_e1000_info info;
    if (!out) return;
    e1000_device_get_info(context, &info);
    *out=(struct e1000_info){.present=info.present,.active=info.active,
        .vendor_id=info.vendor_id,.device_id=info.device_id,
        .bus=info.bus,.slot=info.slot,.function=info.function};
    for (uint32_t i=0;i<6;++i) out->mac[i]=info.mac[i];
}

/** @brief Release the physical DMA rings and buffers without dropping device state.
 * @return None. Ring state is cleared while the PCI identity and owned MMIO
 * alias remain valid for initialization rollback or ordered teardown.
 * Task-context cleanup after active ring DMA has not started or has been stopped.
 */
static void e1000_release_rings(struct e1000_state *d)
{
    for (uint32_t index = 0; index < E1000_RX_COUNT; ++index) {
        if (d->rx_buf[index]) {
            mm_free_pages(d->rx_buf_phys[index], 1);
        }
    }
    for (uint32_t index = 0; index < E1000_TX_COUNT; ++index) {
        if (d->tx_buf[index]) {
            mm_free_pages(d->tx_buf_phys[index], 1);
        }
    }
    if (d->rx_phys) {
        mm_free_pages(d->rx_phys, 1);
    }
    if (d->tx_phys) {
        mm_free_pages(d->tx_phys, 1);
    }
    d->rx = 0;
    d->tx = 0;
    d->rx_phys = 0;
    d->tx_phys = 0;
    e1000_memzero(d->rx_buf, sizeof(d->rx_buf));
    e1000_memzero(d->tx_buf, sizeof(d->tx_buf));
    e1000_memzero(d->rx_buf_phys, sizeof(d->rx_buf_phys));
    e1000_memzero(d->tx_buf_phys, sizeof(d->tx_buf_phys));
    d->rx_index = 0;
    d->tx_tail = 0;
    d->active = 0;
}

/** @brief Stop one controller and release resources only after confirmed reset.
 * @param d Exclusive device after callback drain, including partial probes.
 * @return Zero when safe, or -ETIMEDOUT while retaining memory and MMIO.
 */
static int e1000_stop_device(struct e1000_state *d)
{
    uintptr_t mmio_alias=d->mmio_base;
    if (mmio_alias) {
        e1000_reg_write(d,E1000_REG_RCTL,0);
        e1000_reg_write(d,E1000_REG_TCTL,0);
        e1000_reg_write(d,E1000_REG_IMC,0xffffffffu);
        (void)e1000_reg_read(d,E1000_REG_STATUS);
        kernel_api->sleep_ms(10);
    }
    if (d->pci_command_valid) {
        uint16_t command=pci_config_read16(d->pci.bus,d->pci.slot,d->pci.function,4);
        pci_config_write16(d->pci.bus,d->pci.slot,d->pci.function,4,
                           command & (uint16_t)~PCI_COMMAND_BUS_MASTER);
        (void)pci_config_read16(d->pci.bus,d->pci.slot,d->pci.function,4);
    }
    if (mmio_alias) {
        e1000_reg_write(d,E1000_REG_CTRL,e1000_reg_read(d,E1000_REG_CTRL)|E1000_CTRL_RST);
        kernel_api->sleep_ms(20);
        if (e1000_reg_read(d,E1000_REG_CTRL)&E1000_CTRL_RST) return -110;
    }
    e1000_release_rings(d);
    if (mmio_alias) kernel_api->unmap_mmio((void *)mmio_alias,E1000_REGISTER_BYTES);
    if (d->pci_command_valid)
        pci_config_write16(d->pci.bus,d->pci.slot,d->pci.function,4,d->pci_command_original);
    return 0;
}

/** @brief Probe each supported PCI function into an independent device state.
 * @param api Size-versioned service table retained for driver lifetime.
 * @return Zero with at least one NIC, else a negative errno.
 */
static int e1000_driver_init(const struct reliefos_driver_kernel_api *api)
{
    static const struct reliefnt_netdev_ops ops={
        RELIEFNT_NETDEV_OPS_VERSION,sizeof(ops),e1000_device_is_ready,e1000_device_mac,
        e1000_device_send,e1000_device_poll,e1000_driver_get_info
    };
    if (!api || api->abi_version!=RELIEFOS_DRIVER_ABI_VERSION ||
        api->struct_size<RELIEFOS_DRIVER_PHYS_MAP_API_SIZE || !api->console_log ||
        !api->phys_to_virt || !api->map_mmio || !api->unmap_mmio ||
        !api->alloc_pages || !api->free_pages || !api->pci_enumerate ||
        !api->pci_read16 || !api->pci_write16 || !api->pci_read32 ||
        !api->sleep_ms || !api->report_teardown_failure) return -22;
    kernel_api=api;
    int first_error=0; unsigned registered=0;
    for (uint32_t index=0;;++index) {
        struct reliefos_driver_pci_device pci;
        int rc=api->pci_enumerate(index,&pci);
        if (rc==-19) break;
        if (rc) return rc;
        if (pci.vendor_id!=E1000_VENDOR_INTEL || pci.class_code!=2 || pci.subclass!=0 ||
            (pci.device_id!=E1000_DEVICE_82540EM && pci.device_id!=E1000_DEVICE_82545EM &&
             pci.device_id!=E1000_DEVICE_82543GC && pci.device_id!=E1000_DEVICE_82544GC)) continue;
        struct e1000_state *d=kernel_malloc(sizeof(*d));
        if (!d) {
            api->console_log(CONSOLE_LOG_WARN,"[e1000] %02x:%02x.%u: no device state memory\n",
                             pci.bus,pci.slot,pci.function);
            if (!first_error) first_error=-12;
            break;
        }
        *d=(struct e1000_state){.next=devices,.pci={.bus=pci.bus,.slot=pci.slot,
            .function=pci.function,.vendor_id=pci.vendor_id,.device_id=pci.device_id,
            .class_code=pci.class_code,.subclass=pci.subclass,.prog_if=pci.prog_if}};
        devices=d; e1000_hardware_init(d);
        rc=d->active ? 0 : -19;
        if (!rc) { uint32_t ifindex; rc=driver_register_netdev(&ops,d,&ifindex); }
        if (rc) {
            api->console_log(CONSOLE_LOG_WARN,"[e1000] %02x:%02x.%u initialization failed: %d\n",
                             pci.bus,pci.slot,pci.function,rc);
            if (!first_error) first_error=rc;
            if (e1000_stop_device(d)) return rc;
            devices=d->next; kernel_free(d); continue;
        }
        ++registered;
    }
    return registered ? 0 : first_error ? first_error : -19;
}

/** @brief Stop all controllers and retain failed STOP resources for retry.
 * Called after all callbacks owned by this driver have drained.
 */
static void e1000_driver_fini(void)
{
    struct e1000_state **link=&devices;
    while (*link) {
        struct e1000_state *d=*link;
        int rc=e1000_stop_device(d);
        if (rc) { kernel_api->report_teardown_failure(rc); link=&d->next; continue; }
        *link=d->next; kernel_free(d);
    }
}

const struct reliefos_driver_module e1000_driver_module = {
    .magic = RELIEFOS_DRIVER_MODULE_MAGIC,
    .abi_version = RELIEFOS_DRIVER_ABI_VERSION,
    .struct_size = sizeof(struct reliefos_driver_module),
    .kind = RELIEFOS_DRIVER_KIND_NETWORK,
    .name = "e1000",
    .version = 1U,
    .reserved = 0,
    .init = e1000_driver_init,
    .fini = e1000_driver_fini,
};
