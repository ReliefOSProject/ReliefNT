/* SPDX-License-Identifier: Apache-2.0 */
#include <reliefnt/driver_module.h>
extern const struct reliefos_driver_module vmxnet3_driver_module;
/** @brief Register the descriptor with the native module owner. @return Zero or negative errno. */
static int vmxnet3_register(void)
{
    return driver_module_register(&vmxnet3_driver_module, THIS_MODULE);
}
/** @brief Finish exit after manager-managed DMA stop and callback draining. */
static void vmxnet3_exit(void)
{
}
module_init(vmxnet3_register);
module_exit(vmxnet3_exit);
MODULE_LICENSE("Apache-2.0");
MODULE_DESCRIPTION("ReliefNT VMware VMXNET3 Ethernet driver");
MODULE_ALIAS("pci:v000015ADd000007B0sv*sd*bc*sc*i*");
