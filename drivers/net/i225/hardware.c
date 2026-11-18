/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

#define I225_CTRL_EXT       0x00018u
#define I225_PHPM           0x00e14u
#define I225_EEER           0x00e30u
#define I225_IPCNFG         0x00e38u
#define I225_MANC           0x05820u
#define I225_CTRL_PHY_RST   (1u << 31)
#define I225_CTRL_DRV_LOAD  (1u << 28)
#define I225_PHPM_RST_COMP  (1u << 8)
#define I225_MANC_RESET_BLOCK (1u << 18)
#define I225_IPCNFG_EEE_ADV ((1u << 2) | (1u << 3) | (1u << 4))
#define I225_EEER_LPI       ((1u << 16) | (1u << 17) | (1u << 18))

/** @brief Wait for a register condition without an unbounded hardware spin.
 * @param d Mapped device. @param reg Register byte offset.
 * @param mask Compared bits. @param want Expected masked value.
 * @param milliseconds Maximum number of one millisecond sleeps.
 * @return Zero or -ETIMEDOUT.
 */
static int wait_bits(struct i225_device *d, unsigned reg, uint32_t mask,
                     uint32_t want, unsigned milliseconds)
{
    for (unsigned i = 0;; ++i) {
        if ((i225_read(d, reg) & mask) == want)
            return 0;
        if (i == milliseconds)
            return -110;
        d->api->sleep_ms(1);
    }
}

static bool valid_mac(const uint8_t mac[6])
{
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; ++i)
        any |= mac[i];
    return any && !(mac[0] & 1);
}

/** @brief Release both levels of an acquired SWSM semaphore.
 * @param d Device whose software semaphore was acquired by this driver.
 */
static void hw_semaphore_put(struct i225_device *d)
{
    uint32_t value = i225_read(d, I225_SWSM);
    i225_write(d, I225_SWSM, value & ~(I225_SWSM_SMBI | I225_SWSM_SWESMBI));
    i225_flush(d);
}

/** @brief Acquire software ownership before requesting firmware exclusion.
 * @param d Mapped device. Reading SMBI as zero acquires the software level.
 * @return Zero or -EBUSY; failures release only ownership acquired here.
 */
static int hw_semaphore_get(struct i225_device *d)
{
    unsigned attempt;
    for (attempt = 0; attempt < 100; ++attempt) {
        if (!(i225_read(d, I225_SWSM) & I225_SWSM_SMBI))
            break;
        d->api->sleep_ms(1);
    }
    if (attempt == 100)
        return -16;

    /* SMBI remains ours throughout this second phase, including retries. */
    for (attempt = 0; attempt < 100; ++attempt) {
        uint32_t value = i225_read(d, I225_SWSM);
        i225_write(d, I225_SWSM, value | I225_SWSM_SWESMBI);
        if (i225_read(d, I225_SWSM) & I225_SWSM_SWESMBI)
            return 0;
        d->api->sleep_ms(1);
    }
    hw_semaphore_put(d);
    return -16;
}

/** @brief Acquire and record a PHY/NVM resource without disturbing firmware users.
 * @param d Mapped device. @param mask Resource bits in the software half.
 * @return Zero or -EBUSY; ownership persists until a successful release.
 */
static int resource_acquire(struct i225_device *d, uint32_t mask)
{
    for (unsigned i = 0; i < 100; ++i) {
        int rc = hw_semaphore_get(d);
        if (rc)
            return rc;
        uint32_t sync = i225_read(d, I225_SW_FW_SYNC);
        if (!(sync & (mask | (mask << 16)))) {
            i225_write(d, I225_SW_FW_SYNC, sync | mask);
            d->swfw_owned |= mask;
            hw_semaphore_put(d);
            return 0;
        }
        hw_semaphore_put(d);
        d->api->sleep_ms(5);
    }
    return -16;
}

/** @brief Release only resources acquired by this device instance.
 * @param d Mapped device. @param mask Requested software resource bits.
 * @return Zero or -EBUSY; failed ownership remains recorded for STOP retry.
 */
static int resource_release(struct i225_device *d, uint32_t mask)
{
    mask &= d->swfw_owned;
    if (!mask)
        return 0;
    int rc = hw_semaphore_get(d);
    if (rc)
        return rc;
    i225_write(d, I225_SW_FW_SYNC, i225_read(d, I225_SW_FW_SYNC) & ~mask);
    d->swfw_owned &= ~mask;
    hw_semaphore_put(d);
    return 0;
}

/** @brief Complete one Clause 22 transaction and verify the returned address.
 * @param d Device holding PHY ownership. @param reg PHY register in 0..31.
 * @param value Read result or write data. @param write Select a write.
 * @return Zero, -EINVAL, -EIO or -ETIMEDOUT.
 */
static int mdio(struct i225_device *d, unsigned reg, uint16_t *value, bool write)
{
    if (reg > 31 || !value)
        return -22;
    uint32_t command = (reg << 16) | (1u << 21) |
                       (write ? (1u << 26) : (1u << 27)) |
                       (write ? *value : 0);
    i225_write(d, I225_MDIC, command);
    i225_flush(d);
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t result = i225_read(d, I225_MDIC);
        if (result & I225_MDIC_READY) {
            if ((result & I225_MDIC_ERROR) || ((result >> 16) & 31u) != reg)
                return -5;
            if (!write)
                *value = (uint16_t)result;
            return 0;
        }
        d->api->sleep_ms(1);
    }
    return -110;
}

static int mmd_read_aneg(struct i225_device *d, uint16_t *value)
{
    uint16_t data = 7;
    int rc = mdio(d, 13, &data, true);
    if (!rc) {
        data = 32;
        rc = mdio(d, 14, &data, true);
    }
    if (!rc) {
        data = 0x4007;
        rc = mdio(d, 13, &data, true);
    }
    if (!rc)
        rc = mdio(d, 14, value, false);
    return rc;
}

static int mmd_write_aneg(struct i225_device *d, uint16_t value)
{
    int rc;
    uint16_t data = 7;
    rc = mdio(d, 13, &data, true);
    if (!rc) {
        data = 32;
        rc = mdio(d, 14, &data, true);
    }
    if (!rc) {
        data = 0x4007;
        rc = mdio(d, 13, &data, true);
    }
    if (!rc)
        rc = mdio(d, 14, &value, true);
    return rc;
}

/** @brief Pulse the internal PHY reset and wait for hardware completion.
 * @param d Device holding PHY ownership, with management reset blocking clear.
 * @return Zero or -ETIMEDOUT. The reset line is deasserted on every path.
 */
static int reset_phy(struct i225_device *d)
{
    uint32_t control = i225_read(d, I225_CTRL) & ~I225_CTRL_PHY_RST;
    i225_write(d, I225_CTRL, control | I225_CTRL_PHY_RST);
    i225_flush(d);
    d->api->sleep_ms(1); /* Exceeds the PHY's 100 us reset pulse minimum. */
    i225_write(d, I225_CTRL, control);
    i225_flush(d);
    d->api->sleep_ms(1);
    int rc = wait_bits(d, I225_PHPM, I225_PHPM_RST_COMP, I225_PHPM_RST_COMP, 10);
    if (!rc)
        d->api->sleep_ms(1);
    return rc;
}

/** @brief Disable unsupported EEE advertisement and MAC low power signaling.
 * @param d Mapped device; unrelated PHY configuration bits are preserved.
 */
static void disable_eee(struct i225_device *d)
{
    i225_write(d, I225_IPCNFG, i225_read(d, I225_IPCNFG) & ~I225_IPCNFG_EEE_ADV);
    i225_write(d, I225_EEER, i225_read(d, I225_EEER) & ~I225_EEER_LPI);
    (void)i225_read(d, I225_IPCNFG);
    (void)i225_read(d, I225_EEER);
}

/** @brief Initialize copper autonegotiation while respecting management ownership.
 * @param d Mapped, reset device with firmware driver ownership announced.
 * @return Zero, -EBUSY when management owns the PHY, or a PHY/semaphore error.
 */
static int setup_link(struct i225_device *d)
{
    if (i225_read(d, I225_MANC) & I225_MANC_RESET_BLOCK)
        return -16;
    int rc = resource_acquire(d, I225_SWFW_PHY0);
    if (rc)
        return rc;
    uint16_t value = 0, id1 = 0, id2 = 0;
    rc = reset_phy(d);
    if (!rc)
        rc = mdio(d, 22, &value, true); /* Select the standard copper register page. */
    if (!rc)
        rc = mdio(d, 2, &id1, false);
    if (!rc)
        rc = mdio(d, 3, &id2, false);
    if (!rc && ((!id1 && !id2) || (id1 == UINT16_MAX && id2 == UINT16_MAX)))
        rc = -19;
    if (!rc)
        rc = mdio(d, 4, &value, false);
    if (!rc) {
        value = (value & ~0x0fe0u) | 0x0141u;
        rc = mdio(d, 4, &value, true);
    }
    if (!rc)
        rc = mdio(d, 9, &value, false);
    if (!rc) {
        value = (value & ~0x0300u) | 0x0200u;
        rc = mdio(d, 9, &value, true);
    }
    if (!rc)
        rc = mmd_read_aneg(d, &value);
    if (!rc)
        rc = mmd_write_aneg(d, value | 0x0080u); /* 2.5GBASE-T full duplex. */
    if (!rc)
        rc = mdio(d, 0, &value, false);
    if (!rc) {
        disable_eee(d);
        value = (value & ~((1u << 14) | (1u << 11) | (1u << 10))) |
                (1u << 12) | (1u << 9);
        rc = mdio(d, 0, &value, true);
    }
    int release_rc = resource_release(d, I225_SWFW_PHY0);
    if (!rc)
        rc = release_rc;
    if (rc)
        return rc;

    uint32_t control = i225_read(d, I225_CTRL);
    control &= ~((1u << 11) | (1u << 12) | (3u << 8) | 1u |
                 I225_CTRL_RFCE | I225_CTRL_TFCE | I225_CTRL_VME);
    i225_write(d, I225_CTRL, control | I225_CTRL_SLU);
    i225_flush(d);
    return 0;
}

static int read_nvm_word(struct i225_device *d, unsigned word, uint16_t *out)
{
    i225_write(d, I225_EERD, I225_EERD_START | I225_EERD_ADDR(word));
    for (unsigned i = 0; i < 100; ++i) {
        uint32_t value = i225_read(d, I225_EERD);
        if (value & I225_EERD_DONE) {
            *out = I225_EERD_DATA(value);
            return 0;
        }
        d->api->sleep_ms(1);
    }
    return -110;
}

static int read_station_address(struct i225_device *d)
{
    uint32_t low = i225_read(d, I225_RAL0);
    uint32_t high = i225_read(d, I225_RAH0);
    for (unsigned i = 0; i < 4; ++i)
        d->mac[i] = (uint8_t)(low >> (i * 8));
    d->mac[4] = (uint8_t)high;
    d->mac[5] = (uint8_t)(high >> 8);
    if (!(high & I225_RAH_AV) || !valid_mac(d->mac)) {
        int rc = resource_acquire(d, I225_SWFW_EEP);
        if (rc)
            return rc;
        uint16_t checksum = 0;
        for (unsigned word = 0; word < I225_NVM_WORDS; ++word) {
            uint16_t value;
            rc = read_nvm_word(d, word, &value);
            if (rc)
                break;
            checksum = (uint16_t)(checksum + value);
            if (word < 3) {
                d->mac[word * 2] = (uint8_t)value;
                d->mac[word * 2 + 1] = (uint8_t)(value >> 8);
            }
        }
        if (!rc && checksum != I225_NVM_SUM)
            rc = -5;
        int release_rc = resource_release(d, I225_SWFW_EEP);
        if (!rc)
            rc = release_rc;
        if (rc)
            return rc;
    }
    if (!valid_mac(d->mac))
        return -5;
    low = (uint32_t)d->mac[0] | ((uint32_t)d->mac[1] << 8) |
          ((uint32_t)d->mac[2] << 16) | ((uint32_t)d->mac[3] << 24);
    high = d->mac[4] | ((uint32_t)d->mac[5] << 8) | I225_RAH_AV;
    i225_write(d, I225_RAL0, low);
    i225_write(d, I225_RAH0, high);
    return 0;
}

/** @brief Restore D0 before issuing MMIO transactions to a sleeping device.
 * @param d Device with PCI bus mastering disabled.
 * @return Zero, -EINVAL for a malformed capability list, or -EIO if D0 is refused.
 */
static int enter_d0(struct i225_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    if (!(d->api->pci_read16(p->bus, p->slot, p->function, 6) & (1u << 4)))
        return 0;

    uint8_t offset = d->api->pci_read32(p->bus, p->slot, p->function, 0x34) & 0xffu;
    uint64_t visited = 0;
    while (offset) {
        if (offset < 0x40 || offset > 0xfc || (offset & 3))
            return -22;
        uint64_t bit = UINT64_C(1) << (offset / 4);
        if (visited & bit)
            return -22;
        visited |= bit;
        uint16_t capability = d->api->pci_read16(p->bus, p->slot, p->function, offset);
        if ((capability & 0xffu) != 1u) {
            offset = capability >> 8;
            continue;
        }
        if (offset > 0xf8)
            return -22;
        uint16_t power = d->api->pci_read16(p->bus, p->slot, p->function, offset + 4);
        /* PME status is W1C: writing zero preserves a pending wake event. */
        d->api->pci_write16(p->bus, p->slot, p->function, offset + 4,
                            power & ~((1u << 15) | 3u));
        if (power & 3u)
            d->api->sleep_ms(10); /* Includes the D3hot-to-D0 settling interval. */
        if (d->api->pci_read16(p->bus, p->slot, p->function, offset + 4) & 3u)
            return -5;
        return 0;
    }
    return 0;
}

static int map_bar0(struct i225_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint32_t low = d->api->pci_read32(p->bus, p->slot, p->function, 0x10);
    if (!low || low == UINT32_MAX || (low & 1) || (low & 6) == 2 || (low & 6) == 6)
        return -22;
    uint64_t base = low & ~15u;
    if ((low & 6) == 4) {
        if (d->api->pci_read32(p->bus, p->slot, p->function, 0x14) == UINT32_MAX)
            return -22;
        base |= (uint64_t)d->api->pci_read32(p->bus, p->slot, p->function, 0x14) << 32;
    }
    if (!base || (base & 4095u) || base > UINT64_MAX - I225_MMIO_BYTES)
        return -22;
    d->registers = d->api->map_mmio(base, I225_MMIO_BYTES);
    return d->registers ? 0 : -12;
}

int i225_hardware_start(struct i225_device *d)
{
    d->present = true;
    const struct reliefos_driver_pci_device *p = &d->pci;
    d->original_command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->command_saved = true;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        d->original_command & ~7u);
    int rc = enter_d0(d);
    if (rc)
        return rc;
    rc = map_bar0(d);
    if (rc)
        return rc;
    d->api->pci_write16(p->bus, p->slot, p->function, 4,
                        (d->original_command & ~7u) | 2u | (1u << 10));

    i225_write(d, I225_IAM, 0);
    i225_write(d, I225_IMC, UINT32_MAX);
    (void)i225_read(d, I225_ICR);
    i225_write(d, I225_RCTL, 0);
    i225_write(d, I225_TCTL, 0);
    i225_write(d, I225_CTRL, i225_read(d, I225_CTRL) | I225_CTRL_GIO_DIS);
    i225_flush(d);
    rc = wait_bits(d, I225_STATUS, I225_STATUS_MASTER, 0, 100);
    if (rc)
        return rc;
    i225_write(d, I225_CTRL, i225_read(d, I225_CTRL) | I225_CTRL_RST);
    i225_flush(d);
    rc = wait_bits(d, I225_CTRL, I225_CTRL_RST, 0, 100);
    if (rc)
        return rc;
    d->api->sleep_ms(10);
    rc = wait_bits(d, I225_EECD, I225_EECD_AUTO_RD, I225_EECD_AUTO_RD, 100);
    if (rc)
        return rc;
    i225_write(d, I225_IAM, 0);
    i225_write(d, I225_IMC, UINT32_MAX);
    (void)i225_read(d, I225_ICR);
    i225_write(d, I225_CTRL_EXT, i225_read(d, I225_CTRL_EXT) | I225_CTRL_DRV_LOAD);
    i225_flush(d);
    rc = read_station_address(d);
    if (!rc)
        rc = setup_link(d);
    return rc;
}

int i225_activate_busmaster(struct i225_device *d)
{
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint32_t control = i225_read(d, I225_CTRL) & ~I225_CTRL_GIO_DIS;
    i225_write(d, I225_CTRL, control | I225_CTRL_SLU);
    i225_flush(d);
    uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->api->pci_write16(p->bus, p->slot, p->function, 4, command | 6u);
    if (!(d->api->pci_read16(p->bus, p->slot, p->function, 4) & 4u))
        return -5;
    return 0;
}

int i225_hardware_stop(struct i225_device *d)
{
    d->active = false;
    if (!d->registers) {
        if (d->command_saved) {
            const struct reliefos_driver_pci_device *p = &d->pci;
            uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
            d->api->pci_write16(p->bus, p->slot, p->function, 4, command & ~4u);
        }
        return 0;
    }
    i225_write(d, I225_IMC, UINT32_MAX);
    i225_write(d, I225_RCTL, 0);
    i225_write(d, I225_TCTL, 0);
    i225_write(d, I225_RXDCTL0, 0);
    i225_write(d, I225_TXDCTL0, 0);
    i225_write(d, I225_CTRL, i225_read(d, I225_CTRL) | I225_CTRL_GIO_DIS);
    i225_flush(d);
    d->api->sleep_ms(10);
    const struct reliefos_driver_pci_device *p = &d->pci;
    uint16_t command = d->api->pci_read16(p->bus, p->slot, p->function, 4);
    d->api->pci_write16(p->bus, p->slot, p->function, 4, command & ~4u);
    if (d->api->pci_read16(p->bus, p->slot, p->function, 4) & 4u)
        return -5;
    int rc = wait_bits(d, I225_STATUS, I225_STATUS_MASTER, 0, 100);
    if (rc)
        return rc;
    i225_write(d, I225_CTRL, i225_read(d, I225_CTRL) | I225_CTRL_RST);
    i225_flush(d);
    rc = wait_bits(d, I225_CTRL, I225_CTRL_RST, 0, 100);
    if (rc)
        return rc;
    rc = resource_release(d, d->swfw_owned);
    if (rc)
        return rc;
    i225_write(d, I225_CTRL_EXT, i225_read(d, I225_CTRL_EXT) & ~I225_CTRL_DRV_LOAD);
    i225_flush(d);
    return 0;
}
