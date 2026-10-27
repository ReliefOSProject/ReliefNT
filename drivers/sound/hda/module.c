/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
#include <reliefnt/audio.h>
extern const struct reliefos_driver_module hda_driver_module;

/** @brief Register/probe sound hardware through its required sound-core provider.
 * @return Zero including absent hardware, or an init error triggering native resource rollback.
 * Init task under the loader/boot execution transaction; no permanent self-reference.
 */
static int hda_register(void)
{
    return audio_driver_module_register(&hda_driver_module,THIS_MODULE);
}

/** @brief Finish native module exit after the owner ledger has stopped and released the device.
 * No driver pointer remains published; hardware teardown runs in the fallible quiesce phase.
 */
static void hda_exit(void) { }
module_init(hda_register);
module_exit(hda_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT hda device driver");
MODULE_ALIAS("pci:v*d*sv*sd*bc04sc03i*");
