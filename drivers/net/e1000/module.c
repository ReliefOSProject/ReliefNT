/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module e1000_driver_module;

/** @brief Register/probe the retained hardware descriptor through the common owner registry.
 * @return Zero including absent hardware, or an init error triggering native resource rollback.
 * Init task under the loader/boot execution transaction; no permanent self-reference.
 */
static int e1000_register(void)
{
    return driver_module_register(&e1000_driver_module,THIS_MODULE);
}

/** @brief Finish native module exit after the owner ledger has stopped and released the device.
 * No driver pointer remains published; hardware teardown runs in the fallible quiesce phase.
 */
static void e1000_exit(void) { }
module_init(e1000_register);
module_exit(e1000_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT e1000 device driver");
MODULE_ALIAS("pci:v00008086d0000100Esv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000100Fsv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d00001004sv*sd*bc*sc*i*");
MODULE_ALIAS("pci:v00008086d0000100Dsv*sd*bc*sc*i*");
