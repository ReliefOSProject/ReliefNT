/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module mouse_driver_module;

/** @brief Register/probe the retained hardware descriptor through the common owner registry.
 * @return Zero including absent hardware, or an init error triggering native resource rollback.
 * Init task under the loader/boot execution transaction; no permanent self-reference.
 */
static int mouse_register(void)
{
    return driver_module_register(&mouse_driver_module,THIS_MODULE);
}

/** @brief Finish native module exit after the owner ledger has stopped and released the device.
 * No driver pointer remains published; hardware teardown runs in the fallible quiesce phase.
 */
static void mouse_exit(void) { }
module_init(mouse_register);
module_exit(mouse_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT mouse device driver");
MODULE_INFO(reliefnt_autoload,"platform");
