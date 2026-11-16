/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>

extern const struct reliefos_driver_module rtl8139_driver_module;

/** @brief Register the RTL8139 descriptor under its native module owner.
 * @return Zero including absent hardware, or a negative registration errno.
 */
static int rtl8139_register(void)
{
    return driver_module_register(&rtl8139_driver_module, THIS_MODULE);
}

/** @brief Finish exit after the owner manager has drained callbacks and DMA. */
static void rtl8139_exit(void)
{
}

module_init(rtl8139_register);
module_exit(rtl8139_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT Realtek RTL8139 classic PCI Ethernet driver");
MODULE_ALIAS("pci:v000010ECd00008139sv*sd*bc02sc00i*");
