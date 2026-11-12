/* Firmware data declarations; data copyright and distribution notice in firmware_8125.c. */
#ifndef RELIEFNT_RTL8125_FIRMWARE_H
#define RELIEFNT_RTL8125_FIRMWARE_H
#include <stdint.h>
struct rtl8125_fw_pair { uint16_t reg; uint16_t val; };
extern const struct rtl8125_fw_pair rtl8125_mac_bps[256];
extern const struct rtl8125_fw_pair rtl8125b_mac_bps[138];
extern const struct rtl8125_fw_pair rtl8125d_2_mac_bps[66];
extern const struct rtl8125_fw_pair mac_r25_mcu[550];
extern const struct rtl8125_fw_pair mac_r25b_mcu[1447];
extern const struct rtl8125_fw_pair mac_r25d_1_mcu[3241];
extern const struct rtl8125_fw_pair mac_r25d_2_mcu[1818];
#endif
