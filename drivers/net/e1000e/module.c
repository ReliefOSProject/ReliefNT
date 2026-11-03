/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module e1000e_driver_module;

/** @brief Register the hardware descriptor with the current native module owner.
 * @return Zero including absent hardware, or a negative initialization errno.
 */
static int e1000e_register(void)
{ return driver_module_register(&e1000e_driver_module,THIS_MODULE); }

/** @brief Finish module exit after the manager has stopped DMA and drained callbacks. */
static void e1000e_exit(void) { }
module_init(e1000e_register);
module_exit(e1000e_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT Intel 82574/82583 PCIe Gigabit Ethernet driver");
MODULE_ALIAS("pci:v00008086d000010D3sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000150Csv*sd*bc*sc*i*");
