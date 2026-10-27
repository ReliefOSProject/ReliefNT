/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SERVICE_H
#define RELIEFNT_SERVICE_H
#include <reliefnt/module.h>

struct kernel_service_binding;
/* Slots are permanent zero-initialized core storage, never provider memory. */
struct kernel_service_slot {struct kernel_service_binding *binding;};
/* Immutable descriptor/table remain valid until unregister/ledger release.
 * Publication owns no external self-reference. A queued asynchronous job must
 * obtain its own persistent module user pin; a call below lasts only until
 * its matching release, including any temporary execution suspension. */
struct kernel_service_descriptor {const void *operations;struct module *owner;};
struct kernel_service_call {const void *operations;struct kernel_service_binding *binding;};

/** @brief Publish one optional provider without taking an external self-pin.
 * @param slot Permanent zero-initialized core slot.
 * @param descriptor Persistent immutable table and COMING/LIVE owner or builtin.
 * @return Zero or negative EINVAL/EEXIST/ENOMEM/EBUSY. Init/task context;
 * allocation and cleanup-ledger insertion run outside the service lock.
 * Failed publication leaves no admitted provider. COMING/GOING callbacks are
 * refused; the module ledger owns detach/drain and final binding release.
 */
int kernel_service_register(struct kernel_service_slot *slot,const struct kernel_service_descriptor *descriptor);
/** @brief Unpublish one idle exact provider, preserving any ledger reference.
 * @param slot Permanent owning slot. @param descriptor Exact persistent identity.
 * @return Zero or EINVAL/ENOENT/EBUSY. Task context; active admitted calls refuse
 * detach. No callback, I/O or heap allocation occurs under the service lock.
 */
int kernel_service_unregister(struct kernel_service_slot *slot,const struct kernel_service_descriptor *descriptor);
/** @brief Snapshot a LIVE table with user/execution pins before entering its code.
 * @param slot Permanent core slot, nullable for an unavailable service.
 * @param call Empty caller-owned output, populated only on successful admission.
 * @return True with one owned call, false without changing an existing owned
 * call or publishing pointers to missing/COMING/GOING providers. IRQ/task safe;
 * short registry/module lock transaction, no allocation or provider callback.
 * The user pin makes a busy unload return EWOULDBLOCK while leaving LIVE
 * callbacks usable; the execution pin drains before that user pin is dropped.
 */
bool kernel_service_acquire(struct kernel_service_slot *slot,struct kernel_service_call *call);
/** @brief Release a completed callback before provider code can be reclaimed.
 * @param call Owned admitted call or an empty/NULL record, cleared first.
 * IRQ/task safe; consumes one reference and its user/execution pins. No provider code,
 * allocation or blocking operation; final core binding free is outside locks.
 */
void kernel_service_release(struct kernel_service_call *call);
#endif
