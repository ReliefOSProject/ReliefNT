/* SPDX-License-Identifier: BSD-4-Clause */
/*
 * The RTL8168 initialization and PHY register sequences are adapted from
 * OpenBSD re.c, rtl81x9reg.h and rgephy.c, at commit
 * 7779d12194498048b288afda664dd6271a2ec016.
 *
 * Copyright (c) 1997, 1998-2003
 *     Bill Paul <wpaul@windriver.com>.  All rights reserved.
 * Copyright (c) 1997, 1998
 *     Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 * Copyright (c) 2003
 *     Bill Paul <wpaul@windriver.com>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR THE VOICES IN HIS HEAD
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "internal.h"
#include <linux/errno.h>

/* Offsets are for the PCI Express gigabit MAC, not RTL8125. */
#define R_ID0               0x00u
#define R_ID4               0x04u
#define R_MAR0              0x08u
#define R_MAR4              0x0cu
#define R_TX_DESC_LO        0x20u
#define R_TX_DESC_HI        0x24u
#define R_COMMAND           0x37u
#define R_TX_POLL           0x38u
#define R_IMR               0x3cu
#define R_ISR               0x3eu
#define R_TX_CONFIG         0x40u
#define R_RX_CONFIG         0x44u
#define R_MISSED            0x4cu
#define R_CFG_LOCK          0x50u
#define R_CFG1              0x52u
#define R_CFG3              0x54u
#define R_CFG4              0x55u
#define R_PHYAR             0x60u
#define R_PMCH              0x6fu
#define R_MAX_RX            0xdau
#define R_CPLUS             0xe0u
#define R_RX_DESC_LO        0xe4u
#define R_RX_DESC_HI        0xe8u
#define R_EARLY_TX          0xecu
#define R_MISC              0xf0u

#define CMD_TX              0x04u
#define CMD_RX              0x08u
#define CMD_RESET           0x10u
#define CMD_STOP            0x80u
#define PHY_BUSY            0x80000000u
#define PHY_RESET           0x8000u
#define TX_QUEUE_EMPTY      0x00000800u
#define TX_NORMAL_POLL      0x40u
#define RX_ACCEPT_MASK      0x0000000fu
#define RX_FILTER           0x0000000eu
#define RX_BASE_CONFIG      0x0000ff00u
#define TX_BASE_CONFIG      0x03000700u
#define CPLUS_BASE          0x0089u
#define HWREV_MASK          0x7c800000u
#define SPIN_BIT            0x00400000u
#define POLL_LIMIT          100u

#define Q_PHY_PM            (1u << 0)
#define Q_CMD_STOP          (1u << 1)
#define Q_WAIT_TXQ          (1u << 2)
#define Q_WAIT_TXPOLL       (1u << 3)
#define Q_EARLY_OFF         (1u << 4)
#define Q_EARLY_OFF_V2      (1u << 5)
#define Q_RXDV_GATE         (1u << 6)
#define Q_JUMBO_V2          (1u << 7)

enum rtl8168_revision {
    REV_B1 = 1, REV_B2, REV_B3, REV_C, REV_C2, REV_CP, REV_D,
    REV_DP, REV_E, REV_E_VL, REV_F, REV_G, REV_GU, REV_H, REV_EP, REV_FP
};

/** @brief Save the first initialization or shutdown error.
 * @param d Exclusively owned device. @param error Negative errno.
 * @return The supplied errno; a prior diagnostic is not overwritten.
 */
static int rtl8168_error(struct rtl_device *d, int error)
{
    if (!d->io_error)
        d->io_error = error;
    return error;
}

/** @brief Reset the MAC and wait for the device to acknowledge completion.
 * @param d Mapped device in task context with no spinlock held.
 * @return Zero or -ETIMEDOUT; a failed reset does not establish DMA quiescence.
 */
static int rtl8168_reset(struct rtl_device *d)
{
    rtl_write8(d, R_COMMAND, CMD_RESET);
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        rtl_delay_us(d, 100);
        if (!(rtl_read8(d, R_COMMAND) & CMD_RESET))
            return 0;
    }
    return rtl8168_error(d, -ETIMEDOUT);
}

/** @brief Write a Clause 22 PHY register through the MAC's PHYAR mailbox.
 * @param d Mapped device in task context. @param reg Register index 0 through 31.
 * @param value Register value. @return Zero or -ETIMEDOUT.
 */
static int rtl8168_phy_write(struct rtl_device *d, unsigned reg, uint16_t value)
{
    rtl_write32(d, R_PHYAR, PHY_BUSY | (reg << 16) | value);
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        if (!(rtl_read32(d, R_PHYAR) & PHY_BUSY)) {
            rtl_delay_us(d, 20);
            return 0;
        }
        rtl_delay_us(d, 25);
    }
    return rtl8168_error(d, -ETIMEDOUT);
}

/** @brief Read a Clause 22 PHY register through the bounded PHYAR mailbox.
 * @param d Mapped device in task context. @param reg Register index 0 through 31.
 * @param value Caller-owned output. @return Zero or -ETIMEDOUT.
 */
static int rtl8168_phy_read(struct rtl_device *d, unsigned reg, uint16_t *value)
{
    rtl_write32(d, R_PHYAR, reg << 16);
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        uint32_t result = rtl_read32(d, R_PHYAR);
        if (result & PHY_BUSY) {
            *value = (uint16_t)result;
            rtl_delay_us(d, 20);
            return 0;
        }
        rtl_delay_us(d, 25);
    }
    return rtl8168_error(d, -ETIMEDOUT);
}

/** @brief Apply the OpenBSD DSP workaround to the earliest RTL8169S PHYs.
 * @param d Device with a matching revision-zero or revision-one PHY.
 * @return Zero or a mailbox error; page zero restoration is attempted on error.
 */
static int rtl8168_phy_legacy_dsp(struct rtl_device *d)
{
    static const struct {
        uint8_t reg;
        bool update;
        uint16_t clear;
        uint16_t value;
    } operations[] = {
        {21, false, 0, 0x1000}, {24, false, 0, 0x65c7},
        {4, true, 0x0800, 0}, {4, true, 0xf000, 0},
        {3, false, 0, 0x00a1}, {2, false, 0, 0x0008},
        {1, false, 0, 0x1020}, {0, false, 0, 0x1000},
        {4, true, 0, 0x0800}, {4, true, 0x0800, 0},
        {4, true, 0xf000, 0x7000}, {3, false, 0, 0xff41},
        {2, false, 0, 0xde60}, {1, false, 0, 0x0140},
        {0, false, 0, 0x0077}, {4, true, 0xf000, 0xa000},
        {3, false, 0, 0xdf01}, {2, false, 0, 0xdf20},
        {1, false, 0, 0xff95}, {0, false, 0, 0xfa00},
        {4, true, 0xf000, 0xb000}, {3, false, 0, 0xff41},
        {2, false, 0, 0xde20}, {1, false, 0, 0x0140},
        {0, false, 0, 0x00bb}, {4, true, 0xf000, 0xf000},
        {3, false, 0, 0xdf01}, {2, false, 0, 0xdf20},
        {1, false, 0, 0xff95}, {0, false, 0, 0xbf00},
        {4, true, 0, 0x0800}, {4, true, 0x0800, 0}
    };
    int rc = rtl8168_phy_write(d, 31, 1);
    if (!rc) {
        for (unsigned i = 0; i < sizeof(operations) / sizeof(operations[0]); ++i) {
            uint16_t value = operations[i].value;
            if (operations[i].update) {
                rc = rtl8168_phy_read(d, operations[i].reg, &value);
                if (rc)
                    break;
                value = (value & (uint16_t)~operations[i].clear) |
                        operations[i].value;
            }
            rc = rtl8168_phy_write(d, operations[i].reg, value);
            if (rc)
                break;
        }
    }
    int restore = rtl8168_phy_write(d, 31, 0);
    if (!rc)
        rc = restore;
    if (!rc)
        rtl_delay_us(d, 40);
    return rc;
}

/** @brief Wake and reset the copper PHY, then advertise standard link modes.
 * @param d Identified mapped device; task context, no spinlock held.
 * @return Zero, -EIO, or -ETIMEDOUT. Link negotiation continues asynchronously.
 */
static int rtl8168_setup_phy(struct rtl_device *d)
{
    uint16_t bmcr = 0, id1, id2;
    int rc;

    if (d->quirks & Q_PHY_PM)
        rtl_write8(d, R_PMCH, rtl_read8(d, R_PMCH) | 0x80u);
    rc = rtl8168_phy_write(d, 0x1f, 0);
    if (rc)
        return rc;
    rc = rtl8168_phy_write(d, 0x0e, 0);
    if (rc)
        return rc;
    rc = rtl8168_phy_write(d, 0x00, PHY_RESET);
    if (rc)
        return rc;
    for (unsigned i = 0; i < POLL_LIMIT; ++i) {
        rtl_delay_us(d, 1000);
        rc = rtl8168_phy_read(d, 0x00, &bmcr);
        if (rc)
            return rc;
        if (bmcr == 0xffffu)
            return rtl8168_error(d, -EIO);
        if (!(bmcr & PHY_RESET))
            break;
    }
    if (bmcr & PHY_RESET)
        return rtl8168_error(d, -ETIMEDOUT);

    rc = rtl8168_phy_read(d, 2, &id1);
    if (rc)
        return rc;
    rc = rtl8168_phy_read(d, 3, &id2);
    if (rc)
        return rc;
    if ((!id1 && !id2) || (id1 == 0xffffu && id2 == 0xffffu))
        return rtl8168_error(d, -EIO);
    if (((id1 == 0x001cu && (id2 & 0xfff0u) == 0xc910u) ||
         (id1 == 0x0381u && (id2 & 0xfff0u) == 0x3110u)) &&
        (id2 & 0x000fu) <= 1u) {
        rc = rtl8168_phy_legacy_dsp(d);
        if (rc)
            return rc;
    }

    /* CSMA and 10/100 half/full; advertise both 1000BASE-T duplex modes. */
    rc = rtl8168_phy_write(d, 0x04, 0x01e1u);
    if (rc)
        return rc;
    rc = rtl8168_phy_write(d, 0x09, 0x0300u);
    if (rc)
        return rc;
    return rtl8168_phy_write(d, 0x00, 0x1200u);
}

/** @brief Identify only PCI 10ec:8168 and a known OpenBSD MAC revision family.
 * @param d Mapped PCI function with the original, pre-reset TxConfig snapshot.
 * @return Zero with revision, name and quirks, or -ENODEV with no chip name.
 */
int rtl8168_identify(struct rtl_device *d)
{
    enum rtl8168_revision rev;
    const char *name;
    uint32_t flags = 0;

    d->chip_name = 0;
    d->chip_revision = 0;
    d->quirks = 0;
    if (d->pci.vendor_id != 0x10ecu || d->pci.device_id != 0x8168u)
        return -ENODEV;

    switch (d->xid & HWREV_MASK) {
    case 0x30000000u: rev = REV_B1; name = "RTL8111B/8168B"; break;
    case 0x38000000u:
        rev = (d->xid & SPIN_BIT) ? REV_B3 : REV_B2;
        name = "RTL8111B/8168B"; break;
    case 0x3c000000u:
        rev = (d->xid & SPIN_BIT) ? REV_C2 : REV_C;
        name = "RTL8111C/8168C"; break;
    case 0x3c800000u: rev = REV_CP; name = "RTL8111CP/8168CP"; break;
    case 0x28000000u: rev = REV_D; name = "RTL8111D/8168D"; break;
    case 0x28800000u: rev = REV_DP; name = "RTL8111DP/8168DP"; break;
    case 0x2c000000u: rev = REV_E; name = "RTL8111E/8168E"; break;
    case 0x2c800000u: rev = REV_E_VL; name = "RTL8111E-VL/8168E-VL"; break;
    case 0x48000000u: rev = REV_F; name = "RTL8111F/8168F"; break;
    case 0x4c000000u: rev = REV_G; name = "RTL8111G/8168G"; break;
    case 0x50800000u: rev = REV_GU; name = "RTL8111GU/8168GU"; break;
    case 0x54000000u: rev = REV_H; name = "RTL8111H/8168H"; break;
    case 0x50000000u: rev = REV_EP; name = "RTL8111EP/8168EP"; break;
    case 0x54800000u: rev = REV_FP; name = "RTL8111FP/8168FP"; break;
    default: return -ENODEV;
    }

    switch (rev) {
    case REV_B1: case REV_B2: case REV_B3:
        break;
    case REV_DP:
        flags = Q_JUMBO_V2 | Q_WAIT_TXPOLL;
        break;
    case REV_D: case REV_E:
        flags = Q_JUMBO_V2 | Q_CMD_STOP | Q_PHY_PM;
        break;
    case REV_E_VL: case REV_F:
        flags = Q_JUMBO_V2 | Q_CMD_STOP | Q_WAIT_TXQ | Q_EARLY_OFF;
        break;
    case REV_G: case REV_GU: case REV_H: case REV_EP: case REV_FP:
        flags = Q_JUMBO_V2 | Q_CMD_STOP | Q_WAIT_TXQ |
                Q_EARLY_OFF_V2 | Q_RXDV_GATE;
        break;
    default:
        flags = Q_JUMBO_V2 | Q_CMD_STOP;
        break;
    }
    d->chip_revision = (unsigned)rev;
    d->quirks = flags;
    d->chip_name = name;
    return 0;
}

/** @brief Reset and configure the classic gigabit rings, filters and copper PHY.
 * @param d Identified device with initialized DMA and bus mastering enabled.
 * @return Zero after RX/TX enable, or negative errno with DMA retained for stop.
 */
int rtl8168_start(struct rtl_device *d)
{
    uint32_t mac_lo, mac_hi, rxcfg;
    int rc;

    rtl_write16(d, R_IMR, 0);
    rtl_write16(d, R_ISR, 0xffffu);
    rc = rtl8168_reset(d);
    if (rc)
        return rc;

    /* C+ mode must be the first configuration after reset. */
    rtl_write16(d, R_CPLUS, CPLUS_BASE);
    rtl_write16(d, R_IMR, 0);
    rtl_write16(d, R_ISR, 0xffffu);

    mac_lo = (uint32_t)d->mac[0] | ((uint32_t)d->mac[1] << 8) |
             ((uint32_t)d->mac[2] << 16) | ((uint32_t)d->mac[3] << 24);
    mac_hi = (uint32_t)d->mac[4] | ((uint32_t)d->mac[5] << 8);
    rtl_write8(d, R_CFG_LOCK, 0xc0u);
    rtl_write32(d, R_ID4, mac_hi);
    rtl_write32(d, R_ID0, mac_lo);
    if (d->quirks & Q_JUMBO_V2) {
        rtl_write8(d, R_CFG3, rtl_read8(d, R_CFG3) | 0x04u);
        if (d->chip_revision != REV_DP) {
            uint8_t jumbo_bit = d->chip_revision == REV_E ? 0x01u : 0x02u;
            rtl_write8(d, R_CFG4, rtl_read8(d, R_CFG4) | jumbo_bit);
        }
    }
    rtl_write8(d, R_CFG_LOCK, 0);

    rc = rtl8168_setup_phy(d);
    if (rc)
        return rc;

    rtl_write32(d, R_RX_DESC_HI, (uint32_t)(d->rx_physical >> 32));
    rtl_write32(d, R_RX_DESC_LO, (uint32_t)d->rx_physical);
    rtl_write32(d, R_TX_DESC_HI, (uint32_t)(d->tx_physical >> 32));
    rtl_write32(d, R_TX_DESC_LO, (uint32_t)d->tx_physical);
    if (d->quirks & Q_RXDV_GATE)
        rtl_write32(d, R_MISC, rtl_read32(d, R_MISC) & ~0x00080000u);
    rtl_write32(d, R_TX_CONFIG, TX_BASE_CONFIG);
    rtl_write8(d, R_EARLY_TX, 16);
    rxcfg = RX_BASE_CONFIG | RX_FILTER;
    if (d->quirks & Q_EARLY_OFF)
        rxcfg |= 0x00003800u;
    else if (d->quirks & Q_EARLY_OFF_V2)
        rxcfg |= 0x00000800u;
    rtl_write32(d, R_MAR0, 0xffffffffu);
    rtl_write32(d, R_MAR4, 0xffffffffu);
    rtl_write32(d, R_RX_CONFIG, rxcfg);
    rtl_write32(d, R_MISSED, 0);
    rtl_write16(d, R_MAX_RX, (d->quirks & Q_JUMBO_V2) ? 16383u : RTL_BUFFER_SIZE);
    rtl_write8(d, R_CFG1, rtl_read8(d, R_CFG1) | 0x20u);

    /* Descriptor contents and both halves of their addresses precede DMA. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    rtl_write8(d, R_COMMAND, CMD_TX | CMD_RX);
    return 0;
}

/** @brief Stop RX admission, drain the revision-specific TX path and reset DMA.
 * @param d Identified mapped device, including partially initialized devices.
 * @return Zero only after successful drain and reset; otherwise retain DMA memory.
 */
int rtl8168_stop(struct rtl_device *d)
{
    uint8_t command;
    int rc = 0;

    rtl_write16(d, R_IMR, 0);
    rtl_write16(d, R_ISR, 0xffffu);
    rtl_write32(d, R_RX_CONFIG, rtl_read32(d, R_RX_CONFIG) & ~RX_ACCEPT_MASK);
    command = rtl_read8(d, R_COMMAND);
    if (command & (CMD_TX | CMD_RX)) {
        if (d->quirks & Q_WAIT_TXPOLL) {
            unsigned i;
            for (i = 0; i < POLL_LIMIT; ++i) {
                if (!(rtl_read8(d, R_TX_POLL) & TX_NORMAL_POLL))
                    break;
                rtl_delay_us(d, 20);
            }
            if (i == POLL_LIMIT)
                rc = rtl8168_error(d, -ETIMEDOUT);
            rtl_write8(d, R_COMMAND, 0);
        } else if (d->quirks & Q_CMD_STOP) {
            rtl_write8(d, R_COMMAND, CMD_STOP | CMD_TX | CMD_RX);
            if (d->quirks & Q_WAIT_TXQ) {
                unsigned i;
                for (i = 0; i < POLL_LIMIT; ++i) {
                    if (rtl_read32(d, R_TX_CONFIG) & TX_QUEUE_EMPTY)
                        break;
                    rtl_delay_us(d, 100);
                }
                if (i == POLL_LIMIT)
                    rc = rtl8168_error(d, -ETIMEDOUT);
            }
        } else {
            rtl_write8(d, R_COMMAND, 0);
        }
        rtl_delay_us(d, 1000);
    }
    if (rtl8168_reset(d) && !rc)
        rc = -ETIMEDOUT;
    rtl_write16(d, R_IMR, 0);
    rtl_write16(d, R_ISR, 0xffffu);
    return rc;
}
