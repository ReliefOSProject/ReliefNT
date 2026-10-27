/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_MODULE_METADATA_ADAPTER_H
#define RELIEFNT_MODULE_METADATA_ADAPTER_H
#include <reliefnt/module.h>
#include <uapi/linux/module.h>
#define MODULE_ARCH_INIT {0}
/** @brief Native module initialization entry emitted by module_init.
 * @return Zero or negative errno; called once in the loading task with no module-table lock.
 */
extern int init_module(void);
/** @brief Native cleanup entry emitted by module_exit, called after callback drain. */
extern void cleanup_module(void);
#endif
