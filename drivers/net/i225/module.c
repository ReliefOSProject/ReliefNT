/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module i225_driver_module;

static int i225_register(void)
{ return driver_module_register(&i225_driver_module, THIS_MODULE); }

static void i225_exit(void) { }
module_init(i225_register);
module_exit(i225_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT Intel I225-V 2.5GbE network driver");
MODULE_ALIAS("pci:v00008086d000015F3sv*sd*bc*sc*i*");
