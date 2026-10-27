/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
#include <reliefnt/audio.h>
extern const struct reliefos_driver_module es1371_driver_module;

/** @brief Register/probe sound hardware through its required sound-core provider.
 * @return Zero including absent hardware, or an init error triggering native resource rollback.
 * Init task under the loader/boot execution transaction; no permanent self-reference.
 */
static int es1371_register(void)
{
    return audio_driver_module_register(&es1371_driver_module,THIS_MODULE);
}

/** @brief Finish native module exit after the owner ledger has stopped and released the device.
 * No driver pointer remains published; hardware teardown runs in the fallible quiesce phase.
 */
static void es1371_exit(void) { }
module_init(es1371_register);
module_exit(es1371_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT es1371 device driver");
MODULE_ALIAS("pci:v00001274d00001371sv*sd*bc*sc*i*");
