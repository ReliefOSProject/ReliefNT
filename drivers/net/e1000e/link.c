/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Claim the 82574 MDIO/NVM semaphore without stealing firmware ownership.
 * @param d Live initialized device; task init only, no callback or spinlock held.
 * @return Zero or -EBUSY. 82583 semaphore bits are reserved; completed autoload
 * provides the hardware configuration boundary for that device.
 */
static int acquire_phy(struct e1k_device *d)
{
    if (d->pci.device_id==0x150c) return 0;
    for (unsigned i=0;i<1000;++i) {
        uint32_t value=e1k_read(d,E1K_EXTCNF);
        if (!(value&((1u<<7)|(1u<<6)|E1K_SW_OWN))) {
            e1k_write(d,E1K_EXTCNF,value|E1K_SW_OWN);
            if (e1k_read(d,E1K_EXTCNF)&E1K_SW_OWN) {
                d->software_owns_phy=true;
                return 0;
            }
        }
        d->api->sleep_ms(1);
    }
    return -16;
}

/** @brief Release only the MDIO/NVM semaphore actually acquired by this driver.
 * @param d Device with live MMIO, task init only; does not change firmware bits.
 */
static void release_phy(struct e1k_device *d)
{
    if (d->software_owns_phy) {
        e1k_write(d,E1K_EXTCNF,e1k_read(d,E1K_EXTCNF)&~E1K_SW_OWN);
        e1k_flush(d);
        d->software_owns_phy=false;
    }
}

/** @brief Perform a Clause 22 MDIO operation through MDIC at PHY address one.
 * @param d Device with serialized initialization and PHY ownership.
 * @param reg Clause 22 register index. @param value Read output or write input.
 * @param write True for write, false for read.
 * @return Zero, -EIO for MDIC error, or -ETIMEDOUT; sleeps only in init context.
 */
static int mdio(struct e1k_device *d, unsigned reg, uint16_t *value, bool write)
{
    e1k_write(d,E1K_MDIC,(reg<<16)|(1u<<21)|(write?(1u<<26):(1u<<27))|
                        (write?*value:0));
    for (unsigned i=0;i<100;++i) {
        uint32_t result=e1k_read(d,E1K_MDIC);
        if (result&(1u<<28)) {
            if (result&(1u<<30)) return -5;
            if (!write) *value=(uint16_t)result;
            return 0;
        }
        d->api->sleep_ms(1);
    }
    return -110;
}

/** @brief Validate a six-byte unicast station address without generating one.
 * @param mac Exactly six bytes read from the device's RAR or NVM.
 * @return True for nonzero unicast, false for empty, multicast or broadcast.
 */
static bool valid_mac(const uint8_t mac[6])
{
    uint8_t any=0;
    for (unsigned i=0;i<6;++i) any|=mac[i];
    return any && !(mac[0]&1);
}

/** @brief Load the hardware MAC using 82574/82583 EERD layout when RAR is invalid.
 * @param d Reset device with acquired MDIO/NVM ownership.
 * @return Zero or -ETIMEDOUT/-EIO. Never writes NVM or fabricates a MAC address.
 */
static int station_address(struct e1k_device *d)
{
    uint32_t lo=e1k_read(d,E1K_RAL), hi=e1k_read(d,E1K_RAH);
    for (unsigned i=0;i<4;++i) d->mac[i]=(uint8_t)(lo>>(i*8));
    d->mac[4]=(uint8_t)hi; d->mac[5]=(uint8_t)(hi>>8);
    if (!(hi&(1u<<31)) || !valid_mac(d->mac)) {
        for (unsigned word=0;word<3;++word) {
            e1k_write(d,E1K_EERD,1u|(word<<2));
            uint32_t value=0;
            for (unsigned wait=0;wait<100;++wait) {
                value=e1k_read(d,E1K_EERD);
                if (value&2) break;
                d->api->sleep_ms(1);
            }
            if (!(value&2)) return -110;
            d->mac[word*2]=(uint8_t)(value>>16);
            d->mac[word*2+1]=(uint8_t)(value>>24);
        }
    }
    if (!valid_mac(d->mac)) return -5;
    lo=(uint32_t)d->mac[0]|((uint32_t)d->mac[1]<<8)|
       ((uint32_t)d->mac[2]<<16)|((uint32_t)d->mac[3]<<24);
    hi=d->mac[4]|((uint32_t)d->mac[5]<<8)|(1u<<31);
    e1k_write(d,E1K_RAL,lo); e1k_write(d,E1K_RAH,hi);
    return 0;
}

/** @brief Load MAC and start full-duplex 10/100/1000 copper auto-negotiation.
 * @param d Reset device with completed autoload; task init, no spinlock held.
 * @return Zero without waiting for a cable, or negative NVM/MDIO/PHY errno.
 */
int e1k_setup_link(struct e1k_device *d)
{
    int rc=acquire_phy(d);
    if (rc) return rc;
    rc=station_address(d);
    uint16_t value=0, id=0;
    if (!rc) rc=mdio(d,22,&value,true); /* Select standard copper page zero. */
    if (!rc) rc=mdio(d,2,&id,false);
    if (!rc && (!id || id==0xffff)) rc=-19;
    if (!rc) rc=mdio(d,4,&value,false);
    if (!rc) {
        value=(value&~0x0fe0u)|0x0141u; /* 10/100 full; no unresolved pause mode. */
        rc=mdio(d,4,&value,true);
    }
    if (!rc) rc=mdio(d,9,&value,false);
    if (!rc) { value=(value&~0x0300u)|0x0200u; rc=mdio(d,9,&value,true); }
    if (!rc) rc=mdio(d,0,&value,false);
    if (!rc) {
        value=(value&~((1u<<15)|(1u<<14)|(1u<<11)|(1u<<10)))|
              (1u<<12)|(1u<<9);
        rc=mdio(d,0,&value,true);
    }
    release_phy(d);
    if (rc) return rc;
    uint32_t ctrl=e1k_read(d,E1K_CTRL);
    ctrl&=~((1u<<31)|(1u<<30)|(1u<<28)|(1u<<27)|(1u<<12)|(1u<<11)|
             (1u<<7)|(1u<<5)|E1K_CTRL_GIO_DISABLE);
    e1k_write(d,E1K_CTRL,ctrl|E1K_CTRL_SLU);
    e1k_flush(d);
    return 0;
}
