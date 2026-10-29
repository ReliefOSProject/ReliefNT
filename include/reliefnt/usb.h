/*
 * ReliefNT USB host publication and bounded input polling interface.
 */
#ifndef RELIEFNT_USB_H
#define RELIEFNT_USB_H

#include <reliefnt/module.h>

struct usb_host;

/** @brief Publish one host controller's bounded input poll callback.
 * @param poll IRQ-safe callback; never blocks, allocates, or changes registration.
 * @param context Borrowed controller state retained through unregister return.
 * @param owner COMING/LIVE native code owner, or NULL for permanent code.
 * @param out Required result receiving an exclusively owned registration handle.
 * @return Zero or negative errno, leaving out NULL on failure. Task context;
 * the registration has no external self-reference. COMING/GOING is not polled.
 */
int usb_host_register(void (*poll)(void *), void *context, struct module *owner,
                      struct usb_host **out);

/** @brief Publish IRQ polling plus a deferred, waitable maintenance callback.
 * @param poll Bounded IRQ poller. @param service Task-context maintenance.
 * @param context Borrowed stable host state. @param owner Native code owner.
 * @param out Required exclusive handle. @return Zero or negative errno.
 * Poll/service never overlap for one handle; neither may unregister itself.
 * Service runs with the manager gate held and execution ownership suspended.
 */
int usb_host_register_service(void (*poll)(void *), void (*service)(void *),
                               void *context, struct module *owner, struct usb_host **out);
/** @brief Coalesce task work for an owned registration from IRQ or task context.
 * @param host Borrowed live handle, held through this call. No wait/allocation.
 */
void usb_host_request_service(struct usb_host *host);
/** @brief Observe queued USB maintenance without acquiring provider code. */
bool usb_service_pending(void);
/** @brief Run a fair bounded batch of pinned maintenance callbacks.
 * Task context under manager admission, with execution ownership suspended.
 * No registry lock spans callbacks; closing/COMING/GOING entries are skipped.
 */
void usb_service_run(void);
/** @brief Drain deferred USB work at a depth-one syscall or scheduler boundary. */
void usb_process_work(void);

/** @brief Close callback admission and synchronously drain one owned host handle.
 * @param host Exclusive registration handle returned by usb_host_register.
 * @return Zero after the handle is freed, -ENOENT for an unknown handle, or
 * -EBUSY if a concurrent unregister already owns it. NULL is an idempotent no-op.
 * Task teardown without IRQ/execution locks needed by the callback. The caller
 * stops hardware separately before freeing DMA or controller context.
 */
int usb_host_unregister(struct usb_host *host);

/**
 * @brief Pump the USB host controller and deliver any pending HID key/mouse events.
 */
void usb_poll(void);

#endif
