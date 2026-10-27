/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_GRAPHICS_SERVICE_H
#define RELIEFNT_GRAPHICS_SERVICE_H
#include <reliefnt/service.h>
#include <reliefnt/pci.h>
struct task;
/* Internal kernel interface, not a Linux userspace ABI. The permanent broker
 * admits only LIVE owners. Async objects require independent persistent pins;
 * one call keeps both user and execution references until its return. */
struct graphics_service_operations {
    bool (*is_registered)(void);
    bool (*render_node_ready)(void);
    int (*pci_identity)(struct pci_device *out);
    bool (*kms_scanout_active)(void);
    void (*kms_vt_release)(void);
    uint32_t (*process_retirements)(uint32_t budget);
    void (*cancel_task_continuation)(struct task *task);
    bool (*task_uninterruptible)(const struct task *task);
    bool (*task_signal_busy)(const struct task *task);
};
/** @brief Publish one graphics table without taking a permanent self-reference.
 * @param provider Persistent immutable table and owner descriptor.
 * @return Zero or generic registration errno. Init/task context; the core ledger
 * detaches/drains before code release. No lock spans allocation or callbacks.
 */
int graphics_service_register(const struct kernel_service_descriptor *provider);
/** @brief Detach one idle exact graphics table without entering its code.
 * @param provider Exact persistent registration identity.
 * @return Zero or ENOENT/EBUSY/EINVAL. Task context; active calls preserve binding.
 */
int graphics_service_unregister(const struct kernel_service_descriptor *provider);
/** @brief Retain only the exact DRM failure-diagnostic boot option as core state.
 * @param cmdline Borrowed boot command line, nullable; no pointer is retained.
 * Boot task context before provider initialization; exact tokens only, no I/O.
 */
void graphics_service_configure(const char *cmdline);
/** @brief Read the permanent diagnostic preference during late provider init.
 * @return True only for the exact boot token drm.faildiag=1.
 * Task context; atomic copied state, no provider or boot-buffer reference escapes.
 */
bool graphics_service_faildiag_enabled(void);
/** @brief Report whether a graphics provider owns a discovered primary node.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_is_registered(void);
/** @brief Report whether the retained provider can expose its render node.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_render_node_ready(void);
/** @brief Copy the admitted primary identity without retaining provider data.
 * @param out Required writable identity; untouched when no provider exists.
 * @return Backend result or -ENODEV when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
int graphics_service_pci_identity(struct pci_device *out);
/** @brief Snapshot whether the provider currently owns the KMS scanout.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_kms_scanout_active(void);
/** @brief Release KMS ownership through the admitted provider during VT handoff.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
void graphics_service_kms_vt_release(void);
/** @brief Run bounded graphics retirement work under the exact provider pin.
 * @param budget Maximum work requested; provider applies its existing bounds.
 * @return Backend result or 0 when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
uint32_t graphics_service_process_retirements(uint32_t budget);
/** @brief Cancel optional graphics syscall continuation ownership for one task.
 * @param task Execution-owned task, nullable; core never dereferences optional state.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
void graphics_service_cancel_task_continuation(struct task *task);
/** @brief Query optional graphics continuation signal-deferral policy.
 * @param task Execution-owned task, nullable.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_task_uninterruptible(const struct task *task);
/** @brief Query optional graphics CPU synchronization signal policy.
 * @param task Execution-owned task, nullable.
 * @return Backend result or false when no method is available.
 * Execution-owned task context; no registry lock spans the callback or wait.
 * The core drops both pins only after optional code returns; nothing escapes.
 */
bool graphics_service_task_signal_busy(const struct task *task);
#endif
