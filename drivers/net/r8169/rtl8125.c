/* $OpenBSD: if_rge.c,v 1.49 2026/11/05 07:54:52 kevlo Exp $ */
/*
 * Copyright (c) 2019, 2020, 2023-2026
 *      Kevin Lo <kevlo@openbsd.org>
 *
 * Copyright (c) 2019, 2020, 2025, 2026
 *      Kevin Lo <kevlo@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "internal.h"
#include "rtl8125_regs.h"
#include "firmware_8125.h"
#include <uapi/linux/errno.h>

/* Private revision values are not shared with RTL8168. */
enum { MAC_R25 = 1, MAC_R25B, MAC_R25D_1, MAC_R25D_2 };
#define nitems(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define RGE_TYPE_R25D(d) ((d)->chip_revision == MAC_R25D_1 || (d)->chip_revision == MAC_R25D_2)
#define RGE_READ_1(d,r) ((d)->io_error ? 0 : rtl_read8((d),(r)))
#define RGE_READ_2(d,r) ((d)->io_error ? 0 : rtl_read16((d),(r)))
#define RGE_READ_4(d,r) ((d)->io_error ? 0 : rtl_read32((d),(r)))
#define RGE_WRITE_1(d,r,v) do { if (!(d)->io_error) rtl_write8((d),(r),(v)); } while (0)
#define RGE_WRITE_2(d,r,v) do { if (!(d)->io_error) rtl_write16((d),(r),(v)); } while (0)
#define RGE_WRITE_4(d,r,v) do { if (!(d)->io_error) rtl_write32((d),(r),(v)); } while (0)
#define RGE_SETBIT_1(d,r,m) RGE_WRITE_1((d),(r),RGE_READ_1((d),(r)) | (m))
#define RGE_CLRBIT_1(d,r,m) RGE_WRITE_1((d),(r),RGE_READ_1((d),(r)) & ~(m))
#define RGE_CLRBIT_2(d,r,m) RGE_WRITE_2((d),(r),RGE_READ_2((d),(r)) & ~(m))
#define RGE_CLRBIT_4(d,r,m) RGE_WRITE_4((d),(r),RGE_READ_4((d),(r)) & ~(m))
#define RGE_MAC_SETBIT(d,r,m) rge_mac_modify((d),(r),0,(m))
#define RGE_MAC_CLRBIT(d,r,m) rge_mac_modify((d),(r),(m),0)
#define RGE_PHY_SETBIT(d,r,m) rge_phy_modify((d),(r),0,(m))
#define RGE_PHY_CLRBIT(d,r,m) rge_phy_modify((d),(r),(m),0)
#define RGE_EPHY_SETBIT(d,r,m) rge_ephy_modify((d),(r),0,(m))
#define RGE_EPHY_CLRBIT(d,r,m) rge_ephy_modify((d),(r),(m),0)

/** @brief Poll a 32-bit handshake register without holding a lock.
 * @param d Mapped controller. @param reg Handshake register offset.
 * @param mask Bits to test. @param want Expected masked bits.
 * @param ms Maximum number of millisecond sleeps.
 * @return Register value, or zero with sticky -ETIMEDOUT/-ENODEV.
 */
static uint32_t rge_wait32(struct rtl_device *d, unsigned reg, uint32_t mask,
                           uint32_t want, unsigned ms)
{
    for (unsigned i = 0; !d->io_error; ++i) {
        uint32_t v = rtl_read32(d, reg);
        if (v == 0xffffffffu) { d->io_error = -ENODEV; break; }
        if ((v & mask) == want) return v;
        if (i == ms) { d->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(d, 1000);
    }
    return 0;
}

/** @brief Access the direct MAC OCP window; no busy handshake is defined.
 * @param sc Initialized controller. @param reg Even MAC OCP address.
 * @param val Value written; no effect after a prior I/O failure.
 */
static void rge_write_mac_ocp(struct rtl_device *sc, uint16_t reg, uint16_t val)
{
    RGE_WRITE_4(sc, RGE_MACOCP, 0x80000000u | ((uint32_t)(reg >> 1) << 16) | val);
}
/** @brief Read one word through the direct MAC OCP window.
 * @param sc Initialized controller. @param reg Even MAC OCP address.
 * @return Register value, or zero after a prior I/O failure.
 */
static uint16_t rge_read_mac_ocp(struct rtl_device *sc, uint16_t reg)
{
    RGE_WRITE_4(sc, RGE_MACOCP, (uint32_t)(reg >> 1) << 16);
    return (uint16_t)RGE_READ_4(sc, RGE_MACOCP);
}
/** @brief Change selected MAC OCP bits through the direct window.
 * @param sc Initialized controller. @param reg Even MAC OCP address.
 * @param clear Bits to clear. @param set Bits to set afterwards.
 */
static void rge_mac_modify(struct rtl_device *sc, uint16_t reg, uint16_t clear, uint16_t set)
{
    uint16_t v = rge_read_mac_ocp(sc, reg);
    rge_write_mac_ocp(sc, reg, (v & ~clear) | set);
}
/** @brief Write PHY OCP and wait for hardware to clear its busy bit.
 * @param sc Initialized controller. @param reg Even PHY OCP address.
 * @param val Register value; a timeout is recorded in io_error.
 */
static void rge_write_phy_ocp(struct rtl_device *sc, uint16_t reg, uint16_t val)
{
    if (sc->io_error) return;
    RGE_WRITE_4(sc, RGE_PHYOCP, 0x80000000u | ((uint32_t)(reg >> 1) << 16) | val);
    rtl_delay_us(sc, 1);
    (void)rge_wait32(sc, RGE_PHYOCP, 0x80000000u, 0, 20);
}
/** @brief Read PHY OCP and wait for hardware to set its ready bit.
 * @param sc Initialized controller. @param reg Even PHY OCP address.
 * @return Read value, or zero with sticky -ETIMEDOUT/-ENODEV.
 */
static uint16_t rge_read_phy_ocp(struct rtl_device *sc, uint16_t reg)
{
    if (sc->io_error) return 0;
    RGE_WRITE_4(sc, RGE_PHYOCP, (uint32_t)(reg >> 1) << 16);
    rtl_delay_us(sc, 1);
    return (uint16_t)rge_wait32(sc, RGE_PHYOCP, 0x80000000u, 0x80000000u, 20);
}
/** @brief Change selected PHY OCP bits using bounded handshakes.
 * @param sc Initialized controller. @param reg Even PHY OCP address.
 * @param clear Bits to clear. @param set Bits to set afterwards.
 */
static void rge_phy_modify(struct rtl_device *sc, uint16_t reg, uint16_t clear, uint16_t set)
{
    uint16_t v = rge_read_phy_ocp(sc, reg);
    rge_write_phy_ocp(sc, reg, (v & ~clear) | set);
}
/** @brief Write one EPHY register and wait for its busy handshake.
 * @param sc Initialized controller. @param reg EPHY address. @param val Value to write.
 */
static void rge_write_ephy(struct rtl_device *sc, uint16_t reg, uint16_t val)
{
    if (sc->io_error) return;
    RGE_WRITE_4(sc, RGE_EPHYAR, 0x80000000u | ((uint32_t)(reg & 0x7f) << 16) | val);
    rtl_delay_us(sc, 1);
    (void)rge_wait32(sc, RGE_EPHYAR, 0x80000000u, 0, 20);
    if (!sc->io_error) rtl_delay_us(sc, 20);
}
/** @brief Read one EPHY register using the bounded ready handshake.
 * @param sc Initialized controller. @param reg EPHY address.
 * @return Read value, or zero with sticky timeout or removal error.
 */
static uint16_t rge_read_ephy(struct rtl_device *sc, uint16_t reg)
{
    if (sc->io_error) return 0;
    RGE_WRITE_4(sc, RGE_EPHYAR, (uint32_t)(reg & 0x7f) << 16);
    rtl_delay_us(sc, 1);
    uint16_t v = (uint16_t)rge_wait32(sc, RGE_EPHYAR, 0x80000000u, 0x80000000u, 20);
    if (!sc->io_error) rtl_delay_us(sc, 20);
    return v;
}
/** @brief Change selected EPHY bits using bounded handshakes.
 * @param sc Initialized controller. @param reg EPHY address.
 * @param clear Bits to clear. @param set Bits to set afterwards.
 */
static void rge_ephy_modify(struct rtl_device *sc, uint16_t reg, uint16_t clear, uint16_t set)
{
    uint16_t v = rge_read_ephy(sc, reg);
    rge_write_ephy(sc, reg, (v & ~clear) | set);
}
/** @brief Read an extended PCI configuration dword via the CSI window.
 * @param sc Initialized controller. @param reg Aligned PCI configuration offset.
 * @return Value, or zero with sticky timeout or removal error.
 */
static uint32_t rge_read_csi(struct rtl_device *sc, uint16_t reg)
{
    if (sc->io_error) return 0;
    RGE_WRITE_4(sc, RGE_CSIAR, (reg & 0xfff) | 0xf000);
    rtl_delay_us(sc, 1);
    (void)rge_wait32(sc, RGE_CSIAR, 0x80000000u, 0x80000000u, 20);
    if (!sc->io_error) rtl_delay_us(sc, 20);
    return RGE_READ_4(sc, RGE_CSIDR);
}
/** @brief Write an extended PCI configuration dword via CSI.
 * @param sc Initialized controller. @param reg Aligned PCI configuration offset.
 * @param val Value written; sticky error prevents subsequent programming.
 */
static void rge_write_csi(struct rtl_device *sc, uint16_t reg, uint32_t val)
{
    if (sc->io_error) return;
    RGE_WRITE_4(sc, RGE_CSIDR, val);
    RGE_WRITE_4(sc, RGE_CSIAR, 0x80000000u | 0xf000 | (reg & 0xfff));
    rtl_delay_us(sc, 1);
    (void)rge_wait32(sc, RGE_CSIAR, 0x80000000u, 0, 20);
    if (!sc->io_error) rtl_delay_us(sc, 20);
}
/** @brief Read a standard MII register via its PHY OCP address.
 * @param sc Initialized controller. @param reg Clause 22 register number.
 * @return MII value, or zero with sticky error.
 */
static uint16_t rge_mii_read(struct rtl_device *sc, unsigned reg)
{
    return rge_read_phy_ocp(sc, (uint16_t)(0xa400 + reg * 2));
}
/** @brief Write a standard MII register via its PHY OCP address.
 * @param sc Initialized controller. @param reg Clause 22 register number.
 * @param val Register value.
 */
static void rge_mii_write(struct rtl_device *sc, unsigned reg, uint16_t val)
{
    rge_write_phy_ocp(sc, (uint16_t)(0xa400 + reg * 2), val);
}
/** @brief Enter or leave the PHY patch mode and verify its acknowledgement.
 * @param sc Initialized controller. @param set Nonzero to enter patch mode.
 */
static void rge_patch_phy_mcu(struct rtl_device *sc, int set)
{
    if (set) RGE_PHY_SETBIT(sc, 0xb820, 0x0010);
    else RGE_PHY_CLRBIT(sc, 0xb820, 0x0010);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint16_t v = rge_read_phy_ocp(sc, 0xb800);
        if (sc->io_error) return;
        if (!!(v & 0x0040) == !!set) return;
        if (i == 100) { sc->io_error = -ETIMEDOUT; return; }
        rtl_delay_us(sc, 1000);
    }
}

/** @brief Load the revision-matched PHY program and verify its version marker.
 * @param sc Initialized controller. @param rcodever Expected Realtek program version.
 */
static void rge_phy_config_mcu(struct rtl_device *sc, uint16_t rcodever)
{
    const struct rtl8125_fw_pair *p;
    unsigned count;
    switch (sc->chip_revision) {
    case MAC_R25: p = mac_r25_mcu; count = nitems(mac_r25_mcu); break;
    case MAC_R25B: p = mac_r25b_mcu; count = nitems(mac_r25b_mcu); break;
    case MAC_R25D_1: p = mac_r25d_1_mcu; count = nitems(mac_r25d_1_mcu); break;
    case MAC_R25D_2: p = mac_r25d_2_mcu; count = nitems(mac_r25d_2_mcu); break;
    default: sc->io_error = -ENODEV; return;
    }
    rge_write_phy_ocp(sc, 0xa436, 0x801e);
    uint16_t old = rge_read_phy_ocp(sc, 0xa438);
    if (sc->io_error || old == rcodever) return;
    rge_patch_phy_mcu(sc, 1);
    if (sc->chip_revision == MAC_R25) {
        rge_write_phy_ocp(sc, 0xa436, 0x8024);
        rge_write_phy_ocp(sc, 0xa438, 0x8601);
        rge_write_phy_ocp(sc, 0xa436, 0xb82e);
        rge_write_phy_ocp(sc, 0xa438, 0x0001);
        RGE_PHY_SETBIT(sc, 0xb820, 0x0080);
    }
    for (unsigned i = 0; i < count && !sc->io_error; ++i) {
        if ((sc->chip_revision == MAC_R25D_1 && (i == 2403 || i == 2528)) ||
            (sc->chip_revision == MAC_R25D_2 && i == 1269)) {
            rge_patch_phy_mcu(sc, 0);
            rge_patch_phy_mcu(sc, 1);
        }
        rge_write_phy_ocp(sc, p[i].reg, p[i].val);
    }
    if (sc->chip_revision == MAC_R25) {
        RGE_PHY_CLRBIT(sc, 0xb820, 0x0080);
        rge_write_phy_ocp(sc, 0xa436, 0);
        rge_write_phy_ocp(sc, 0xa438, 0);
        RGE_PHY_CLRBIT(sc, 0xb82e, 0x0001);
        rge_write_phy_ocp(sc, 0xa436, 0x8024);
        rge_write_phy_ocp(sc, 0xa438, 0);
    }
    rge_patch_phy_mcu(sc, 0);
    rge_write_phy_ocp(sc, 0xa436, 0x801e);
    rge_write_phy_ocp(sc, 0xa438, rcodever);
    rge_write_phy_ocp(sc, 0xa436, 0x801e);
    uint16_t now = rge_read_phy_ocp(sc, 0xa438);
    if (!sc->io_error && now != rcodever) sc->io_error = -EIO;
}

/** @brief Apply the revision-specific Realtek PHY tuning sequence.
 * @param sc Mapped controller; io_error suppresses programming after any failure.
 */
static void
rge_phy_config_mac_r25(struct rtl_device *sc)
{
	uint16_t val;
	int i;
	static const uint16_t mac_cfg3_a438_value[] =
	    { 0x0043, 0x00a7, 0x00d6, 0x00ec, 0x00f6, 0x00fb, 0x00fd, 0x00ff,
	      0x00bb, 0x0058, 0x0029, 0x0013, 0x0009, 0x0004, 0x0002 };

	static const uint16_t mac_cfg3_b88e_value[] =
	    { 0xc091, 0x6e12, 0xc092, 0x1214, 0xc094, 0x1516, 0xc096, 0x171b,
	      0xc098, 0x1b1c, 0xc09a, 0x1f1f, 0xc09c, 0x2021, 0xc09e, 0x2224,
	      0xc0a0, 0x2424, 0xc0a2, 0x2424, 0xc0a4, 0x2424, 0xc018, 0x0af2,
	      0xc01a, 0x0d4a, 0xc01c, 0x0f26, 0xc01e, 0x118d, 0xc020, 0x14f3,
	      0xc022, 0x175a, 0xc024, 0x19c0, 0xc026, 0x1c26, 0xc089, 0x6050,
	      0xc08a, 0x5f6e, 0xc08c, 0x6e6e, 0xc08e, 0x6e6e, 0xc090, 0x6e12 };

	rge_phy_config_mcu(sc, RGE_MAC_R25_RCODE_VER);

	RGE_PHY_SETBIT(sc, 0xad4e, 0x0010);
	val = rge_read_phy_ocp(sc, 0xad16) & ~0x03ff;
	rge_write_phy_ocp(sc, 0xad16, val | 0x03ff);
	val = rge_read_phy_ocp(sc, 0xad32) & ~0x003f;
	rge_write_phy_ocp(sc, 0xad32, val | 0x0006);
	RGE_PHY_CLRBIT(sc, 0xac08, 0x1000);
	RGE_PHY_CLRBIT(sc, 0xac08, 0x0100);
	val = rge_read_phy_ocp(sc, 0xacc0) & ~0x0003;
	rge_write_phy_ocp(sc, 0xacc0, val | 0x0002);
	val = rge_read_phy_ocp(sc, 0xad40) & ~0x00e0;
	rge_write_phy_ocp(sc, 0xad40, val | 0x0040);
	val = rge_read_phy_ocp(sc, 0xad40) & ~0x0007;
	rge_write_phy_ocp(sc, 0xad40, val | 0x0004);
	RGE_PHY_CLRBIT(sc, 0xac14, 0x0080);
	RGE_PHY_CLRBIT(sc, 0xac80, 0x0300);
	val = rge_read_phy_ocp(sc, 0xac5e) & ~0x0007;
	rge_write_phy_ocp(sc, 0xac5e, val | 0x0002);
	rge_write_phy_ocp(sc, 0xad4c, 0x00a8);
	rge_write_phy_ocp(sc, 0xac5c, 0x01ff);
	val = rge_read_phy_ocp(sc, 0xac8a) & ~0x00f0;
	rge_write_phy_ocp(sc, 0xac8a, val | 0x0030);
	rge_write_phy_ocp(sc, 0xb87c, 0x8157);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	rge_write_phy_ocp(sc, 0xb87c, 0x8159);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	rge_write_phy_ocp(sc, 0xb87c, 0x80a2);
	rge_write_phy_ocp(sc, 0xb87e, 0x0153);
	rge_write_phy_ocp(sc, 0xb87c, 0x809c);
	rge_write_phy_ocp(sc, 0xb87e, 0x0153);

	rge_write_phy_ocp(sc, 0xa436, 0x81b3);
	for (i = 0; i < nitems(mac_cfg3_a438_value); i++)
		rge_write_phy_ocp(sc, 0xa438, mac_cfg3_a438_value[i]);
	for (i = 0; i < 26; i++)
		rge_write_phy_ocp(sc, 0xa438, 0);
	rge_write_phy_ocp(sc, 0xa436, 0x8257);
	rge_write_phy_ocp(sc, 0xa438, 0x020f);
	rge_write_phy_ocp(sc, 0xa436, 0x80ea);
	rge_write_phy_ocp(sc, 0xa438, 0x7843);

	rge_patch_phy_mcu(sc, 1);
	RGE_PHY_CLRBIT(sc, 0xb896, 0x0001);
	RGE_PHY_CLRBIT(sc, 0xb892, 0xff00);
	for (i = 0; i < nitems(mac_cfg3_b88e_value); i += 2) {
		rge_write_phy_ocp(sc, 0xb88e, mac_cfg3_b88e_value[i]);
		rge_write_phy_ocp(sc, 0xb890, mac_cfg3_b88e_value[i + 1]);
	}
	RGE_PHY_SETBIT(sc, 0xb896, 0x0001);
	rge_patch_phy_mcu(sc, 0);

	RGE_PHY_SETBIT(sc, 0xd068, 0x2000);
	rge_write_phy_ocp(sc, 0xa436, 0x81a2);
	RGE_PHY_SETBIT(sc, 0xa438, 0x0100);
	val = rge_read_phy_ocp(sc, 0xb54c) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb54c, val | 0xdb00);
	RGE_PHY_CLRBIT(sc, 0xa454, 0x0001);
	RGE_PHY_SETBIT(sc, 0xa5d4, 0x0020);
	RGE_PHY_CLRBIT(sc, 0xad4e, 0x0010);
	RGE_PHY_CLRBIT(sc, 0xa86a, 0x0001);
	RGE_PHY_SETBIT(sc, 0xa442, 0x0800);
	RGE_PHY_SETBIT(sc, 0xa424, 0x0008);
}

/** @brief Apply the revision-specific Realtek PHY tuning sequence.
 * @param sc Mapped controller; io_error suppresses programming after any failure.
 */
static void
rge_phy_config_mac_r25b(struct rtl_device *sc)
{
	uint16_t val;
	int i;

	rge_phy_config_mcu(sc, RGE_MAC_R25B_RCODE_VER);

	RGE_PHY_SETBIT(sc, 0xa442, 0x0800);
	val = rge_read_phy_ocp(sc, 0xac46) & ~0x00f0;
	rge_write_phy_ocp(sc, 0xac46, val | 0x0090);
	val = rge_read_phy_ocp(sc, 0xad30) & ~0x0003;
	rge_write_phy_ocp(sc, 0xad30, val | 0x0001);
	rge_write_phy_ocp(sc, 0xb87c, 0x80f5);
	rge_write_phy_ocp(sc, 0xb87e, 0x760e);
	rge_write_phy_ocp(sc, 0xb87c, 0x8107);
	rge_write_phy_ocp(sc, 0xb87e, 0x360e);
	rge_write_phy_ocp(sc, 0xb87c, 0x8551);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0800);
	val = rge_read_phy_ocp(sc, 0xbf00) & ~0xe000;
	rge_write_phy_ocp(sc, 0xbf00, val | 0xa000);
	val = rge_read_phy_ocp(sc, 0xbf46) & ~0x0f00;
	rge_write_phy_ocp(sc, 0xbf46, val | 0x0300);
	for (i = 0; i < 10; i++) {
		rge_write_phy_ocp(sc, 0xa436, 0x8044 + i * 6);
		rge_write_phy_ocp(sc, 0xa438, 0x2417);
	}
	RGE_PHY_SETBIT(sc, 0xa4ca, 0x0040);
	val = rge_read_phy_ocp(sc, 0xbf84) & ~0xe000;
	rge_write_phy_ocp(sc, 0xbf84, val | 0xa000);
	rge_write_phy_ocp(sc, 0xa436, 0x8170);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0x2700;
	rge_write_phy_ocp(sc, 0xa438, val | 0xd800);
	RGE_PHY_SETBIT(sc, 0xa424, 0x0008);
}

/** @brief Apply the revision-specific Realtek PHY tuning sequence.
 * @param sc Mapped controller; io_error suppresses programming after any failure.
 */
static void
rge_phy_config_mac_r25d_1(struct rtl_device *sc)
{
	uint16_t val;
	int i;

	rge_phy_config_mcu(sc, RGE_MAC_R25D_1_RCODE_VER);

	RGE_PHY_SETBIT(sc, 0xa442, 0x0800);

	rge_patch_phy_mcu(sc, 1);
	RGE_PHY_SETBIT(sc, 0xbf96, 0x8000);
	val = rge_read_phy_ocp(sc, 0xbf94) & ~0x0007;
	rge_write_phy_ocp(sc, 0xbf94, val | 0x0005);
	val = rge_read_phy_ocp(sc, 0xbf8e) & ~0x3c00;
	rge_write_phy_ocp(sc, 0xbf8e, val | 0x2800);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	RGE_PHY_SETBIT(sc, 0xbcd8, 0xc000);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	val = rge_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	rge_write_phy_ocp(sc, 0xbc80, val | 0x0004);
	RGE_PHY_SETBIT(sc, 0xbc82, 0xe000);
	RGE_PHY_SETBIT(sc, 0xbc82, 0x1c00);
	val = rge_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	rge_write_phy_ocp(sc, 0xbc80, val | 0x0005);
	val = rge_read_phy_ocp(sc, 0xbc82) & ~0x00e0;
	rge_write_phy_ocp(sc, 0xbc82, val | 0x0040);
	RGE_PHY_SETBIT(sc, 0xbc82, 0x001c);
	RGE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x8000);
	RGE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	RGE_PHY_CLRBIT(sc, 0xbd70, 0x0100);
	RGE_PHY_SETBIT(sc, 0xa466, 0x0002);
	rge_write_phy_ocp(sc, 0xa436, 0x836a);
	RGE_PHY_CLRBIT(sc, 0xa438, 0xff00);
	rge_patch_phy_mcu(sc, 0);

	rge_write_phy_ocp(sc, 0xb87c, 0x832c);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	val = rge_read_phy_ocp(sc, 0xb106) & ~0x0700;
	rge_write_phy_ocp(sc, 0xb106, val | 0x0100);
	val = rge_read_phy_ocp(sc, 0xb206) & ~0x0700;
	rge_write_phy_ocp(sc, 0xb206, val | 0x0200);
	val = rge_read_phy_ocp(sc, 0xb306) & ~0x0700;
	rge_write_phy_ocp(sc, 0xb306, val | 0x0300);
	rge_write_phy_ocp(sc, 0xb87c, 0x80cb);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0300);
	rge_write_phy_ocp(sc, 0xbcf4, 0x0000);
	rge_write_phy_ocp(sc, 0xbcf6, 0x0000);
	rge_write_phy_ocp(sc, 0xbc12, 0x0000);
	rge_write_phy_ocp(sc, 0xb87c, 0x844d);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0200);

	rge_write_phy_ocp(sc, 0xb87c, 0x8feb);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0100);
	rge_write_phy_ocp(sc, 0xb87c, 0x8fe9);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0600);

	val = rge_read_phy_ocp(sc, 0xac7e) & ~0x01fc;
	rge_write_phy_ocp(sc, 0xac7e, val | 0x00B4);
	rge_write_phy_ocp(sc, 0xb87c, 0x8105);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x7a00);
	rge_write_phy_ocp(sc, 0xb87c, 0x8117);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x3a00);
	rge_write_phy_ocp(sc, 0xb87c, 0x8103);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x7400);
	rge_write_phy_ocp(sc, 0xb87c, 0x8115);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x3400);
	RGE_PHY_CLRBIT(sc, 0xad40, 0x0030);
	val = rge_read_phy_ocp(sc, 0xad66) & ~0x000f;
	rge_write_phy_ocp(sc, 0xad66, val | 0x0007);
	val = rge_read_phy_ocp(sc, 0xad68) & ~0xf000;
	rge_write_phy_ocp(sc, 0xad68, val | 0x8000);
	val = rge_read_phy_ocp(sc, 0xad68) & ~0x0f00;
	rge_write_phy_ocp(sc, 0xad68, val | 0x0500);
	val = rge_read_phy_ocp(sc, 0xad68) & ~0x000f;
	rge_write_phy_ocp(sc, 0xad68, val | 0x0002);
	val = rge_read_phy_ocp(sc, 0xad6a) & ~0xf000;
	rge_write_phy_ocp(sc, 0xad6a, val | 0x7000);
	rge_write_phy_ocp(sc, 0xac50, 0x01e8);
	rge_write_phy_ocp(sc, 0xa436, 0x81fa);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x5400);
	val = rge_read_phy_ocp(sc, 0xa864) & ~0x00f0;
	rge_write_phy_ocp(sc, 0xa864, val | 0x00c0);
	val = rge_read_phy_ocp(sc, 0xa42c) & ~0x00ff;
	rge_write_phy_ocp(sc, 0xa42c, val | 0x0002);
	rge_write_phy_ocp(sc, 0xa436, 0x80e1);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x0f00);
	rge_write_phy_ocp(sc, 0xa436, 0x80de);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xf000;
	rge_write_phy_ocp(sc, 0xa438, val | 0x0700);
	RGE_PHY_SETBIT(sc, 0xa846, 0x0080);
	rge_write_phy_ocp(sc, 0xa436, 0x80ba);
	rge_write_phy_ocp(sc, 0xa438, 0x8a04);
	rge_write_phy_ocp(sc, 0xa436, 0x80bd);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0xca00);
	rge_write_phy_ocp(sc, 0xa436, 0x80b7);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0xb300);
	rge_write_phy_ocp(sc, 0xa436, 0x80ce);
	rge_write_phy_ocp(sc, 0xa438, 0x8a04);
	rge_write_phy_ocp(sc, 0xa436, 0x80d1);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0xca00);
	rge_write_phy_ocp(sc, 0xa436, 0x80cb);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0xbb00);
	rge_write_phy_ocp(sc, 0xa436, 0x80a6);
	rge_write_phy_ocp(sc, 0xa438, 0x4909);
	rge_write_phy_ocp(sc, 0xa436, 0x80a8);
	rge_write_phy_ocp(sc, 0xa438, 0x05b8);
	rge_write_phy_ocp(sc, 0xa436, 0x8200);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x5800);
	rge_write_phy_ocp(sc, 0xa436, 0x8ff1);
	rge_write_phy_ocp(sc, 0xa438, 0x7078);
	rge_write_phy_ocp(sc, 0xa436, 0x8ff3);
	rge_write_phy_ocp(sc, 0xa438, 0x5d78);
	rge_write_phy_ocp(sc, 0xa436, 0x8ff5);
	rge_write_phy_ocp(sc, 0xa438, 0x7862);
	rge_write_phy_ocp(sc, 0xa436, 0x8ff7);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x1400);

	rge_write_phy_ocp(sc, 0xa436, 0x814c);
	rge_write_phy_ocp(sc, 0xa438, 0x8455);
	rge_write_phy_ocp(sc, 0xa436, 0x814e);
	rge_write_phy_ocp(sc, 0xa438, 0x84a6);
	rge_write_phy_ocp(sc, 0xa436, 0x8163);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x0600);
	rge_write_phy_ocp(sc, 0xa436, 0x816a);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x0500);
	rge_write_phy_ocp(sc, 0xa436, 0x8171);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x1f00);

	val = rge_read_phy_ocp(sc, 0xbc3a) & ~0x000f;
	rge_write_phy_ocp(sc, 0xbc3a, val | 0x0006);
	for (i = 0; i < 10; i++) {
		rge_write_phy_ocp(sc, 0xa436, 0x8064 + i * 3);
		RGE_PHY_CLRBIT(sc, 0xa438, 0x0700);
	}
	val = rge_read_phy_ocp(sc, 0xbfa0) & ~0xff70;
	rge_write_phy_ocp(sc, 0xbfa0, val | 0x5500);
	rge_write_phy_ocp(sc, 0xbfa2, 0x9d00);
	rge_write_phy_ocp(sc, 0xa436, 0x8165);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0x0700;
	rge_write_phy_ocp(sc, 0xa438, val | 0x0200);

	rge_write_phy_ocp(sc, 0xa436, 0x8019);
	RGE_PHY_SETBIT(sc, 0xa438, 0x0100);
	rge_write_phy_ocp(sc, 0xa436, 0x8fe3);
	rge_write_phy_ocp(sc, 0xa438, 0x0005);
	rge_write_phy_ocp(sc, 0xa438, 0x0000);
	rge_write_phy_ocp(sc, 0xa438, 0x00ed);
	rge_write_phy_ocp(sc, 0xa438, 0x0502);
	rge_write_phy_ocp(sc, 0xa438, 0x0b00);
	rge_write_phy_ocp(sc, 0xa438, 0xd401);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x2900);

	rge_write_phy_ocp(sc, 0xa436, 0x8018);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x1700);

	rge_write_phy_ocp(sc, 0xa436, 0x815b);
	val = rge_read_phy_ocp(sc, 0xa438) & ~0xff00;
	rge_write_phy_ocp(sc, 0xa438, val | 0x1700);

	RGE_PHY_CLRBIT(sc, 0xa4e0, 0x8000);
	RGE_PHY_CLRBIT(sc, 0xa5d4, 0x0020);
	RGE_PHY_CLRBIT(sc, 0xa654, 0x0800);
	RGE_PHY_SETBIT(sc, 0xa430, 0x1001);
	RGE_PHY_SETBIT(sc, 0xa442, 0x0080);
}

/** @brief Apply the revision-specific Realtek PHY tuning sequence.
 * @param sc Mapped controller; io_error suppresses programming after any failure.
 */
static void
rge_phy_config_mac_r25d_2(struct rtl_device *sc)
{
	uint16_t val;

	rge_phy_config_mcu(sc, RGE_MAC_R25D_2_RCODE_VER);

	RGE_PHY_SETBIT(sc, 0xa442, 0x0800);

	rge_patch_phy_mcu(sc, 1);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	RGE_PHY_SETBIT(sc, 0xbcd8, 0xc000);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x4000);
	val = rge_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	rge_write_phy_ocp(sc, 0xbc80, val | 0x0004);
	RGE_PHY_SETBIT(sc, 0xbc82, 0xe000);
	RGE_PHY_SETBIT(sc, 0xbc82, 0x1c00);
	val = rge_read_phy_ocp(sc, 0xbc80) & ~0x001f;
	rge_write_phy_ocp(sc, 0xbc80, val | 0x0005);
	val = rge_read_phy_ocp(sc, 0xbc82) & ~0x00e0;
	rge_write_phy_ocp(sc, 0xbc82, val | 0x0040);
	RGE_PHY_SETBIT(sc, 0xbc82, 0x001c);
	RGE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	val = rge_read_phy_ocp(sc, 0xbcd8) & ~0xc000;
	rge_write_phy_ocp(sc, 0xbcd8, val | 0x8000);
	RGE_PHY_CLRBIT(sc, 0xbcd8, 0xc000);
	rge_patch_phy_mcu(sc, 0);

	val = rge_read_phy_ocp(sc, 0xac7e) & ~0x01fc;
	rge_write_phy_ocp(sc, 0xac7e, val | 0x00b4);
	rge_write_phy_ocp(sc, 0xb87c, 0x8105);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x7a00);
	rge_write_phy_ocp(sc, 0xb87c, 0x8117);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x3a00);
	rge_write_phy_ocp(sc, 0xb87c, 0x8103);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x7400);
	rge_write_phy_ocp(sc, 0xb87c, 0x8115);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x3400);
	rge_write_phy_ocp(sc, 0xb87c, 0x8feb);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0500);
	rge_write_phy_ocp(sc, 0xb87c, 0x8fea);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0x0700);
	rge_write_phy_ocp(sc, 0xb87c, 0x80d6);
	val = rge_read_phy_ocp(sc, 0xb87e) & ~0xff00;
	rge_write_phy_ocp(sc, 0xb87e, val | 0xef00);
	RGE_PHY_CLRBIT(sc, 0xa5d4, 0x0020);
	RGE_PHY_CLRBIT(sc, 0xa654, 0x0800);
	RGE_PHY_CLRBIT(sc, 0xa448, 0x0400);
	RGE_PHY_CLRBIT(sc, 0xa586, 0x0400);
	RGE_PHY_SETBIT(sc, 0xa430, 0x1001);
	RGE_PHY_SETBIT(sc, 0xa442, 0x0080);
}

/* EPHY tuning is from the ISC-licensed if_rgereg.h reference. */
static const struct rtl8125_fw_pair mac_r25_ephy[] = {
    {0x0004,0xd000},{0x000a,0x8653},{0x0023,0xab66},{0x0020,0x9455},
    {0x0021,0x99ff},{0x0029,0xfe04},{0x0044,0xd000},{0x004a,0x8653},
    {0x0063,0xab66},{0x0060,0x9455},{0x0061,0x99ff},{0x0069,0xfe04}
};
static const struct rtl8125_fw_pair mac_r25b_ephy[] = {
    {0x000b,0xa908},{0x001e,0x20eb},{0x0022,0x0023},{0x0002,0x60c2},
    {0x0029,0xff00},{0x004b,0xa908},{0x005e,0x20eb},{0x0062,0x0023},
    {0x0042,0x60c2},{0x0069,0xff00}
};

/** @brief Tune the revision-specific PCIe analog PHY.
 * @param sc Mapped controller; each access propagates a bounded handshake error.
 */
static void rge_ephy_config(struct rtl_device *sc)
{
    if (sc->chip_revision == MAC_R25) {
        for (int i = 0; i < nitems(mac_r25_ephy) && !sc->io_error; ++i)
            rge_write_ephy(sc, mac_r25_ephy[i].reg, mac_r25_ephy[i].val);
        uint16_t v = rge_read_ephy(sc, 0x002a) & ~0x7000;
        rge_write_ephy(sc, 0x002a, v | 0x3000);
        RGE_EPHY_CLRBIT(sc, 0x0019, 0x0040);
        RGE_EPHY_SETBIT(sc, 0x001b, 0x0e00);
        RGE_EPHY_CLRBIT(sc, 0x001b, 0x7000);
        rge_write_ephy(sc, 0x0002, 0x6042);
        rge_write_ephy(sc, 0x0006, 0x0014);
        v = rge_read_ephy(sc, 0x006a) & ~0x7000;
        rge_write_ephy(sc, 0x006a, v | 0x3000);
        RGE_EPHY_CLRBIT(sc, 0x0059, 0x0040);
        RGE_EPHY_SETBIT(sc, 0x005b, 0x0e00);
        RGE_EPHY_CLRBIT(sc, 0x005b, 0x7000);
        rge_write_ephy(sc, 0x0042, 0x6042);
        rge_write_ephy(sc, 0x0046, 0x0014);
    } else if (sc->chip_revision == MAC_R25B) {
        for (int i = 0; i < nitems(mac_r25b_ephy) && !sc->io_error; ++i)
            rge_write_ephy(sc, mac_r25b_ephy[i].reg, mac_r25b_ephy[i].val);
    }
}

/** @brief Change the MAC MCU RAM page while initialization owns the OCP window.
 * @param sc Mapped controller. @param page Page number from zero through two.
 */
static void rge_switch_mcu_ram_page(struct rtl_device *sc, unsigned page)
{
    rge_mac_modify(sc, 0xe446, 0x0003, (uint16_t)page);
}
/** @brief Load the MAC microcode appropriate to this silicon revision.
 * @param sc Identified controller with MAC MCU disabled.
 */
static void rge_mac_config_mcu(struct rtl_device *sc)
{
    const struct rtl8125_fw_pair *p = 0;
    unsigned count = 0;
    if (sc->chip_revision == MAC_R25) {
        p = rtl8125_mac_bps; count = nitems(rtl8125_mac_bps);
    } else if (sc->chip_revision == MAC_R25B) {
        p = rtl8125b_mac_bps; count = nitems(rtl8125b_mac_bps);
    } else if (sc->chip_revision == MAC_R25D_2) {
        p = rtl8125d_2_mac_bps; count = nitems(rtl8125d_2_mac_bps);
    }
    if (sc->chip_revision == MAC_R25B) {
        rge_switch_mcu_ram_page(sc, 0);
        for (unsigned i = 0; i < count && !sc->io_error; ++i)
            rge_write_mac_ocp(sc, p[i].reg, p[i].val);
        return;
    }
    for (unsigned page = 0; page < 3 && !sc->io_error; ++page) {
        rge_switch_mcu_ram_page(sc, page);
        if (sc->chip_revision == MAC_R25D_1) {
            static const uint16_t init[] = {0xe002,0xe006,0x4166,0x9cf6,
                                            0xc002,0xb800,0x14a4,0xc102,0xb900};
            for (unsigned i = 0; i < sizeof(init)/sizeof(init[0]); ++i)
                rge_write_mac_ocp(sc, 0xf800 + i * 2, page == 0 ? init[i] : 0);
            for (unsigned reg = 0xf812; reg <= 0xf9f6; reg += 2)
                rge_write_mac_ocp(sc, reg, 0);
        } else {
            for (unsigned i = 0; i < count && !sc->io_error; ++i) {
                if (sc->chip_revision == MAC_R25 && page == 2 && p[i].reg >= 0xf9f8)
                    continue;
                rge_write_mac_ocp(sc, p[i].reg, page == 0 ? p[i].val : 0);
            }
            if (sc->chip_revision == MAC_R25D_2)
                for (unsigned reg = 0xf884; reg <= 0xf9f6; reg += 2)
                    rge_write_mac_ocp(sc, reg, 0);
        }
        if (sc->chip_revision == MAC_R25) {
            if (page == 2) {
                rge_write_mac_ocp(sc, 0xf9f8, 0x6486);
                rge_write_mac_ocp(sc, 0xf9fa, 0x0b15);
                rge_write_mac_ocp(sc, 0xf9fc, 0x090e);
                rge_write_mac_ocp(sc, 0xf9fe, 0x1139);
            }
        } else {
            rge_write_mac_ocp(sc, 0xf9f8, page == 2 ? 0x6938 : 0);
            rge_write_mac_ocp(sc, 0xf9fa, page == 2 ? (sc->chip_revision == MAC_R25D_1 ? 0x0a18 : 0x0a19) : 0);
            rge_write_mac_ocp(sc, 0xf9fc, page == 2 ? (sc->chip_revision == MAC_R25D_1 ? 0x0217 : 0x030e) : 0);
            rge_write_mac_ocp(sc, 0xf9fe, page == 2 ? (sc->chip_revision == MAC_R25D_1 ? 0x0d2a : 0x0b2f) : 0);
        }
    }
    rge_write_mac_ocp(sc, 0xfc26, 0x8000);
    if (sc->chip_revision == MAC_R25) {
        rge_write_mac_ocp(sc, 0xfc2a, 0x0540);
        rge_write_mac_ocp(sc, 0xfc2e, 0x0a06);
        rge_write_mac_ocp(sc, 0xfc30, 0x0eb8);
        rge_write_mac_ocp(sc, 0xfc32, 0x3a5c);
        rge_write_mac_ocp(sc, 0xfc34, 0x10a8);
        rge_write_mac_ocp(sc, 0xfc40, 0x0d54);
        rge_write_mac_ocp(sc, 0xfc42, 0x0e24);
        rge_write_mac_ocp(sc, 0xfc48, 0x307a);
    } else {
        rge_write_mac_ocp(sc, 0xfc28, sc->chip_revision == MAC_R25D_1 ? 0x14a2 : 0x2382);
        rge_write_mac_ocp(sc, 0xfc48, 0x0001);
    }
}
/** @brief Disable controller clock request and PME state with configuration unlocked.
 * @param sc Mapped controller; the original configuration lock state is preserved.
 */
static void rge_disable_aspm_clkreq(struct rtl_device *sc)
{
    bool unlock = (RGE_READ_1(sc, RGE_EECMD) & RGE_EECMD_WRITECFG) != RGE_EECMD_WRITECFG;
    if (unlock) RGE_SETBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
    RGE_CLRBIT_1(sc, RGE_CFG2, RGE_CFG2_CLKREQ_EN);
    RGE_CLRBIT_1(sc, RGE_CFG5, RGE_CFG5_PME_STS);
    if (unlock) RGE_CLRBIT_1(sc, RGE_EECMD, RGE_EECMD_WRITECFG);
}
/** @brief Request RX/TX shutdown, confirm both FIFO conditions, and reset the MAC.
 * @param sc Mapped controller, without a lock held or competing callbacks.
 * @return Zero on completed reset, or sticky negative timeout/removal errno.
 */
static int rge_reset(struct rtl_device *sc)
{
    RGE_WRITE_4(sc, RGE_IMR, 0);
    RGE_WRITE_4(sc, RGE_ISR, RGE_READ_4(sc, RGE_ISR));
    RGE_WRITE_4(sc, RGE_TIMERINT0, 0);
    RGE_WRITE_4(sc, RGE_TIMERINT1, 0);
    RGE_WRITE_4(sc, RGE_TIMERINT2, 0);
    RGE_WRITE_4(sc, RGE_TIMERINT3, 0);
    RGE_CLRBIT_4(sc, RGE_RXCFG, 0x3f);
    RGE_SETBIT_1(sc, RGE_PPSW, 0x08);
    RGE_SETBIT_1(sc, RGE_CMD, RGE_CMD_STOPREQ);
    if (!sc->io_error) rtl_delay_us(sc, 200);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint8_t v = RGE_READ_1(sc, RGE_MCUCMD);
        if (v == 0xff) { sc->io_error = -ENODEV; break; }
        if ((v & 0x30) == 0x30) break;
        if (i == 150) { sc->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(sc, 1000);
    }
    if (sc->chip_revision != MAC_R25) {
        for (unsigned i = 0; !sc->io_error; ++i) {
            uint16_t v = RGE_READ_2(sc, RGE_IM);
            if (v == 0xffff) { sc->io_error = -ENODEV; break; }
            if ((v & 0x0103) == 0x0103) break;
            if (i == 150) { sc->io_error = -ETIMEDOUT; break; }
            rtl_delay_us(sc, 1000);
        }
    }
    /* Keep both engines disabled after FIFO drain while issuing reset. */
    RGE_WRITE_1(sc, RGE_CMD, 0);
    RGE_WRITE_1(sc, RGE_CMD, RGE_CMD_RESET);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint8_t v = RGE_READ_1(sc, RGE_CMD);
        if (v == 0xff) { sc->io_error = -ENODEV; break; }
        if (!(v & (RGE_CMD_RESET | RGE_CMD_RXENB | RGE_CMD_TXENB))) break;
        if (i == 100) { sc->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(sc, 1000);
    }
    return sc->io_error;
}

/** @brief Exit out-of-band mode before changing MAC and PHY configuration.
 * @param sc Mapped and identified controller with exclusive initialization access.
 * @return Zero, or the first negative hardware error.
 */
static int rge_exit_oob(struct rtl_device *sc)
{
    rge_write_mac_ocp(sc, 0xc0bc, 0x00ff);
    if (rge_reset(sc)) return sc->io_error;
    RGE_CLRBIT_1(sc, RGE_MCUCMD, RGE_MCUCMD_IS_OOB);
    RGE_MAC_CLRBIT(sc, 0xe8de, 0x4000);
    for (unsigned stage = 0; stage < 2 && !sc->io_error; ++stage) {
        if (stage) {
            rge_write_mac_ocp(sc, 0xc0aa, 0x07d0);
            rge_write_mac_ocp(sc, 0xc0a6, 0x01b5);
            rge_write_mac_ocp(sc, 0xc01e, 0x5555);
        }
        if (!sc->io_error) rtl_delay_us(sc, 100);
        for (unsigned i = 0; !sc->io_error; ++i) {
            uint16_t v = RGE_READ_2(sc, RGE_TWICMD);
            if (v == 0xffff) { sc->io_error = -ENODEV; break; }
            if (v & 0x0200) break;
            if (i == 10) { sc->io_error = -ETIMEDOUT; break; }
            rtl_delay_us(sc, 1000);
        }
    }
    if (rge_read_mac_ocp(sc, 0xd42c) & 0x0100) {
        for (unsigned i = 0; !sc->io_error; ++i) {
            uint16_t v = rge_read_phy_ocp(sc, 0xa420);
            if (sc->io_error) break;
            if ((v & 7) == 2) break;
            if (i == 1000) { sc->io_error = -ETIMEDOUT; break; }
            rtl_delay_us(sc, 1000);
        }
        RGE_MAC_CLRBIT(sc, 0xd42c, 0x0100);
        if (sc->chip_revision != MAC_R25) RGE_PHY_CLRBIT(sc, 0xa466, 0x0001);
        RGE_PHY_CLRBIT(sc, 0xa468, 0x000a);
    }
    return sc->io_error;
}
/** @brief Power the PHY and load the matching controller microcode.
 * @param sc Identified controller, in exclusive task context.
 * @return Zero or a negative error from a power, CSI, patch, or reset handshake.
 */
static int rge_chipinit(struct rtl_device *sc)
{
    if (rge_exit_oob(sc)) return sc->io_error;
    RGE_SETBIT_1(sc, RGE_PMCH, 0xc0);
    rge_mii_write(sc, 0, 0x1000);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint16_t v = rge_read_phy_ocp(sc, 0xa420);
        if (sc->io_error) break;
        if ((v & 7) == 3) break;
        if (i == 1000) { sc->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(sc, 1000);
    }
    rge_disable_aspm_clkreq(sc);
    RGE_CLRBIT_1(sc, 0xf1, 0x80);
    RGE_MAC_CLRBIT(sc, 0xd40a, 0x0010);
    rge_disable_aspm_clkreq(sc);
    rge_write_mac_ocp(sc, 0xfc48, 0);
    for (unsigned reg = 0xfc28; reg < 0xfc48 && !sc->io_error; reg += 2)
        rge_write_mac_ocp(sc, reg, 0);
    if (!sc->io_error) rtl_delay_us(sc, 3000);
    rge_write_mac_ocp(sc, 0xfc26, 0);
    rge_mac_config_mcu(sc);
    if (sc->chip_revision == MAC_R25 && rge_read_phy_ocp(sc, 0xc416) != 0x0500) {
        rge_patch_phy_mcu(sc, 1);
        rge_write_phy_ocp(sc, 0xc416, 0);
        rge_write_phy_ocp(sc, 0xc416, 0x0500);
        rge_patch_phy_mcu(sc, 0);
    }
    uint32_t status = rge_read_csi(sc, 0x108);
    rge_write_csi(sc, 0x108, status | 0x00100000);
    if (!sc->io_error) (void)rge_reset(sc);
    return sc->io_error;
}
/** @brief Reset, patch, and tune the Ethernet PHY and disable EEE.
 * @param sc Identified powered controller with matching firmware data.
 * @return Zero or the first negative programming or timeout error.
 */
static int rge_phy_config(struct rtl_device *sc)
{
    rge_ephy_config(sc);
    rge_mii_write(sc, 4, rge_mii_read(sc, 4) & ~0x01e0);
    rge_mii_write(sc, 9, rge_mii_read(sc, 9) & ~0x0300);
    RGE_PHY_CLRBIT(sc, 0xa5d4, RGE_ADV_2500TFDX);
    rge_mii_write(sc, 0, 0x9200);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint16_t v = rge_mii_read(sc, 0);
        if (sc->io_error) break;
        if (!(v & 0x8000)) break;
        if (i == 2500) { sc->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(sc, 1000);
    }
    if (sc->io_error) return sc->io_error;
    switch (sc->chip_revision) {
    case MAC_R25: rge_phy_config_mac_r25(sc); break;
    case MAC_R25B: rge_phy_config_mac_r25b(sc); break;
    case MAC_R25D_1: rge_phy_config_mac_r25d_1(sc); break;
    case MAC_R25D_2: rge_phy_config_mac_r25d_2(sc); break;
    default: sc->io_error = -ENODEV; return sc->io_error;
    }
    RGE_PHY_CLRBIT(sc, 0xa5b4, 0x8000);
    RGE_MAC_CLRBIT(sc, 0xe040, 0x0003);
    if (sc->chip_revision == MAC_R25) {
        RGE_MAC_CLRBIT(sc, 0xeb62, 0x0006);
        RGE_PHY_CLRBIT(sc, 0xa432, 0x0010);
    } else RGE_PHY_SETBIT(sc, 0xa432, 0x0010);
    RGE_PHY_CLRBIT(sc, 0xa5d0, 0x0006);
    RGE_PHY_CLRBIT(sc, 0xa6d4, 0x0001);
    RGE_PHY_CLRBIT(sc, 0xa6d8, 0x0010);
    RGE_PHY_CLRBIT(sc, 0xa428, 0x0080);
    RGE_PHY_CLRBIT(sc, 0xa4a2, 0x0200);
    RGE_MAC_CLRBIT(sc, 0xe052, 0x0001);
    RGE_PHY_CLRBIT(sc, 0xa442, 0x3000);
    RGE_PHY_CLRBIT(sc, 0xa430, 0x8000);
    return sc->io_error;
}
/** @brief Advertise ordinary 10/100/1000/2500BASE-T and restart negotiation.
 * @param sc Patched controller with its PHY accessible.
 * @return Zero or the first negative PHY error or reset timeout.
 */
static int rge_autoneg(struct rtl_device *sc)
{
    RGE_PHY_CLRBIT(sc, 0xa428, 0x0200);
    RGE_PHY_CLRBIT(sc, 0xa5ea, 0x0001);
    uint16_t v = rge_read_phy_ocp(sc, 0xa5d4) | RGE_ADV_2500TFDX;
    rge_mii_write(sc, 4, 0x0de1); /* selector, 10/100 half/full and pause */
    rge_mii_write(sc, 9, 0x0300); /* 1000BASE-T half/full */
    rge_write_phy_ocp(sc, 0xa5d4, v);
    rge_mii_write(sc, 0, 0x9200);
    for (unsigned i = 0; !sc->io_error; ++i) {
        uint16_t control = rge_mii_read(sc, 0);
        if (sc->io_error) break;
        if (!(control & 0x8000)) break;
        if (i == 2500) { sc->io_error = -ETIMEDOUT; break; }
        rtl_delay_us(sc, 1000);
    }
    return sc->io_error;
}

/** @brief Identify a supported RTL8125 MAC using TxConfig's hardware revision.
 * @param d Mapped device with xid set before reset.
 * @return Zero with chip metadata, or -ENODEV for unsupported silicon.
 */
int rtl8125_identify(struct rtl_device *d)
{
    switch (d->xid & 0x7cf00000u) {
    case 0x60900000u: d->chip_revision = MAC_R25; d->chip_name = "RTL8125A"; break;
    case 0x64100000u: d->chip_revision = MAC_R25B; d->chip_name = "RTL8125B"; break;
    case 0x68800000u: d->chip_revision = MAC_R25D_1; d->chip_name = "RTL8125D-1"; break;
    case 0x68900000u: d->chip_revision = MAC_R25D_2; d->chip_name = "RTL8125D-2"; break;
    default: return -ENODEV;
    }
    return 0;
}
/** @brief Load the matching firmware and configure one standard RX/TX queue.
 * @param d Identified mapped device with rings and PCI bus mastering enabled.
 * @return Zero with RX/TX running, or negative errno retaining all resources.
 */
int rtl8125_start(struct rtl_device *d)
{
    if (d->io_error) return d->io_error;
    if (!d->registers || d->mmio_bytes < 0x604a || !d->rx || !d->tx ||
        (d->rx_physical & 0xff) || (d->tx_physical & 0xff)) return -EINVAL;
    if (rge_chipinit(d) || rge_phy_config(d)) return d->io_error;

    RGE_SETBIT_1(d, RGE_EECMD, RGE_EECMD_WRITECFG);
    RGE_WRITE_4(d, RGE_MAC0, (uint32_t)d->mac[0] | (uint32_t)d->mac[1] << 8 |
        (uint32_t)d->mac[2] << 16 | (uint32_t)d->mac[3] << 24);
    RGE_WRITE_4(d, RGE_MAC4, (uint32_t)d->mac[4] | (uint32_t)d->mac[5] << 8);
    RGE_CLRBIT_1(d, 0xf1, 0x80);
    rge_disable_aspm_clkreq(d);
    RGE_WRITE_2(d, RGE_EEE_TXIDLE_TIMER, 16408);
    RGE_WRITE_4(d, RGE_RXDESC_ADDR_LO, (uint32_t)d->rx_physical);
    RGE_WRITE_4(d, RGE_RXDESC_ADDR_HI, (uint32_t)(d->rx_physical >> 32));
    RGE_WRITE_4(d, RGE_TXDESC_ADDR_LO, (uint32_t)d->tx_physical);
    RGE_WRITE_4(d, RGE_TXDESC_ADDR_HI, (uint32_t)(d->tx_physical >> 32));

    /* Bit 24 selects the extended 32-byte RX descriptor, so leave it clear. */
    uint32_t rxconf = d->chip_revision == MAC_R25 ? 0x40000700u :
                      d->chip_revision == MAC_R25B ? 0x40000c00u : 0x40200c00u;
    RGE_WRITE_4(d, RGE_RXCFG, rxconf);
    RGE_WRITE_4(d, RGE_TXCFG, 0x03000700);
    uint32_t cfg = rge_read_csi(d, 0x70c);
    rge_write_csi(d, 0x70c, (cfg & ~0x3f000000u) | 0x27000000u);
    if (!RGE_TYPE_R25D(d)) RGE_WRITE_2(d, 0x0382, 0x221b);
    RGE_WRITE_4(d, RGE_RSS_CTRL, 0);
    RGE_CLRBIT_2(d, RGE_RXQUEUE_CTRL, 0x001c);
    RGE_CLRBIT_1(d, RGE_CFG1, RGE_CFG1_SPEED_DOWN);
    rge_write_mac_ocp(d, 0xc140, 0xffff);
    rge_write_mac_ocp(d, 0xc142, 0xffff);
    /* Bit 0 selects the extended TX layout; shared rings have 16-byte entries. */
    RGE_MAC_CLRBIT(d, 0xeb58, 0x0001);
    rge_mac_modify(d, 0xe614, 0x0700, d->chip_revision == MAC_R25B ? 0x0200 : 0x0300);
    rge_mac_modify(d, 0xe63e, 0x0c30, 0x0020);
    RGE_MAC_CLRBIT(d, 0xc0b4, 0x0001);
    RGE_MAC_SETBIT(d, 0xc0b4, 0x0001);
    RGE_MAC_SETBIT(d, 0xc0b4, 0x000c);
    rge_mac_modify(d, 0xeb6a, 0x00ff, 0x0033);
    rge_mac_modify(d, 0xeb50, 0x03e0, 0x0040);
    RGE_MAC_CLRBIT(d, 0xe056, 0x00f0);
    RGE_WRITE_1(d, RGE_TDFNR, 0x10);
    RGE_MAC_CLRBIT(d, 0xe040, 0x1000);
    rge_mac_modify(d, 0xea1c, 0x0003, 0x0001);
    rge_write_mac_ocp(d, 0xe0c0, RGE_TYPE_R25D(d) ? 0x4403 : 0x4000);
    RGE_MAC_SETBIT(d, 0xe052, 0x0060);
    RGE_MAC_CLRBIT(d, 0xe052, 0x0088);
    rge_mac_modify(d, 0xd430, 0x0fff, 0x045f);
    RGE_SETBIT_1(d, RGE_DLLPR, 0xc0);
    RGE_SETBIT_1(d, RGE_PPSW, 0x40);
    if (d->chip_revision == MAC_R25) RGE_SETBIT_1(d, RGE_MCUCMD, 0x01);
    if (!RGE_TYPE_R25D(d)) RGE_MAC_CLRBIT(d, 0xe080, 0x0002);
    RGE_MAC_CLRBIT(d, 0xea1c, 0x0004);
    RGE_MAC_SETBIT(d, 0xeb54, 0x0001);
    if (!d->io_error) rtl_delay_us(d, 1);
    RGE_MAC_CLRBIT(d, 0xeb54, 0x0001);
    RGE_CLRBIT_2(d, 0x1880, 0x0030);
    if (d->chip_revision != MAC_R25) RGE_CLRBIT_1(d, RGE_INT_CFG0, 0x01);
    RGE_WRITE_4(d, RGE_TIMERINT0, 0);
    RGE_WRITE_4(d, RGE_TIMERINT1, 0);
    RGE_WRITE_4(d, RGE_TIMERINT2, 0);
    RGE_WRITE_4(d, RGE_TIMERINT3, 0);
    for (unsigned i = 0; i < (d->chip_revision == MAC_R25B ? 32u : 64u) && !d->io_error; ++i)
        RGE_WRITE_4(d, RGE_INTMITI(i), 0);
    RGE_MAC_SETBIT(d, 0xc0ac, 0x1f80);
    rge_write_mac_ocp(d, 0xe098, 0xc302);
    RGE_MAC_CLRBIT(d, 0xe032, 0x0003);
    cfg = rge_read_csi(d, 0x98);
    rge_write_csi(d, 0x98, cfg & ~0x0000ff00u);
    if (!d->io_error) rtl_delay_us(d, 20);
    rge_mac_modify(d, 0xe092, 0x00ff, RGE_TYPE_R25D(d) ? 0x0008 : 0);

    /* Software handles checksums and VLAN headers; RSS and VLAN stripping stay off. */
    RGE_CLRBIT_4(d, RGE_RXCFG, RGE_RXCFG_VLANSTRIP);
    RGE_CLRBIT_2(d, RGE_CPLUSCMD, 0x0060);
    RGE_WRITE_2(d, RGE_RXMAXSIZE, RTL_MAX_FRAME + 4);
    RGE_CLRBIT_1(d, RGE_PPSW, 0x08);
    RGE_WRITE_4(d, RGE_MAR0, 0xffffffffu);
    RGE_WRITE_4(d, RGE_MAR4, 0xffffffffu);
    RGE_WRITE_4(d, RGE_RXCFG, rxconf | RGE_RXCFG_INDIV | RGE_RXCFG_MULTI | RGE_RXCFG_BROAD);
    rge_disable_aspm_clkreq(d);
    RGE_CLRBIT_1(d, RGE_EECMD, RGE_EECMD_WRITECFG);
    if (!d->io_error) rtl_delay_us(d, 10);
    if (rge_autoneg(d)) return d->io_error;
    RGE_WRITE_4(d, RGE_IMR, 0);
    RGE_WRITE_4(d, RGE_ISR, RGE_READ_4(d, RGE_ISR));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    RGE_WRITE_1(d, RGE_CMD, RGE_CMD_TXENB | RGE_CMD_RXENB);
    return d->io_error;
}
/** @brief Disable and reset RTL8125 DMA before the owner releases its resources.
 * @param d Identified mapped device, including partial initialization.
 * @return Zero on confirmed quiescence, or negative errno requiring retention.
 */
int rtl8125_stop(struct rtl_device *d)
{
    if (!d->registers) return 0;
    int first_error = d->io_error;
    d->io_error = 0; /* Stop must still be attempted after an initialization error. */
    int rc = rge_reset(d);
    if (!rc) RGE_MAC_CLRBIT(d, 0xc0ac, 0x1f80);
    if (!rc) rc = d->io_error;
    if (first_error) d->io_error = first_error;
    return rc;
}
