/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>

extern const struct reliefos_driver_module r8169_driver_module;

/** @brief Register the driver descriptor under the current native module owner.
 * @return Zero including absent hardware, or a negative initialization errno.
 */
static int r8169_register(void)
{
    return driver_module_register(&r8169_driver_module, THIS_MODULE);
}

/** @brief Finish module exit after the manager has drained callbacks and DMA. */
static void r8169_exit(void)
{
}

module_init(r8169_register);
module_exit(r8169_exit);
MODULE_LICENSE("Apache-2.0 AND ISC AND BSD-4-Clause AND LicenseRef-Realtek-Firmware");
MODULE_DESCRIPTION("ReliefNT Realtek RTL8111/8168 Gigabit and RTL8125 2.5GbE Ethernet");
MODULE_ALIAS("pci:v000010ECd00008168sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v000010ECd00008125sv*sd*bc*sc*i*");
