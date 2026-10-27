/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module uhci_driver_module;

/** @brief Probe/register the same UHCI implementation for built-in and module use.
 * @return Zero including absent hardware, or native initialization errno.
 * Task init under the loader/boot transaction; no permanent self-reference.
 */
static int uhci_register(void)
{
    return driver_module_register(&uhci_driver_module,THIS_MODULE);
}
/** @brief Complete exit after the owner ledger has drained polling and stopped DMA.
 * Hardware teardown belongs to the fallible quiesce phase before this callback.
 */
static void uhci_exit(void) { }
module_init(uhci_register);
module_exit(uhci_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT UHCI USB boot HID host driver");
MODULE_ALIAS("pci:v*d*sv*sd*bc0Csc03i00*");
