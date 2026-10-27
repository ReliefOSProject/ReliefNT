/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DRIVER_MODULE_H
#define RELIEFNT_DRIVER_MODULE_H
#include <reliefnt/module.h>
#include <reliefos/driver.h>

/** @brief Register/probe one native driver with an owned cleanup obligation.
 * @param descriptor Immutable kernel/module-owned descriptor, retained until cleanup.
 * @param owner COMING native module or NULL for permanent builtin code.
 * @return Zero on registration (also when no matching hardware exists), or a
 * negative errno. Failed native init is unwound by the module resource ledger.
 * Init task context with a depth-one execution transaction; hardware init and
 * teardown run with it suspended and interrupts enabled. No registry spinlock
 * spans I/O or callbacks. A self-owned binding does not pin its own module.
 */
int driver_module_register(const struct reliefos_driver_module *descriptor,struct module *owner);

/** @brief Borrow a reserved registration's code owner under its lifetime gate.
 * @param id Driver slot held stable by manager init, or an enabled service gate.
 * @return Borrowed native owner or NULL for permanent builtin code.
 * IRQ-safe, no allocation or lock. The caller retains manager admission or
 * the service lock with admission open/an active callback count. Cleanup closes
 * and drains that gate before clearing the slot. No reference is transferred.
 */
#ifdef CONFIG_MODULES
struct module *driver_module_owner(uint32_t id);
#else
static inline struct module *driver_module_owner(uint32_t id) { (void)id; return NULL; }
#endif
#endif
