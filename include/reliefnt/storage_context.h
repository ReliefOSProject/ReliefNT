/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_STORAGE_CONTEXT_H
#define RELIEFNT_STORAGE_CONTEXT_H
#include <reliefnt/types.h>
struct storage_io_context {bool asynchronous,write_started;};
/** @brief Isolate synchronous background I/O from a reserved user continuation.
 * @param saved Required caller-owned flag snapshot. Execution writer held;
 * does not transfer/cancel the pending provider command or its owner PID.
 */
void storage_kernel_context_enter(struct storage_io_context *saved);
/** @brief Restore exactly the I/O flags of the still-reserved scheduler caller.
 * @param saved Matching snapshot. Execution writer held, no owner transfer.
 */
void storage_kernel_context_leave(const struct storage_io_context *saved);
#endif
