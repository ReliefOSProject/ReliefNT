/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DRM_MEMORY_H
#define RELIEFNT_DRM_MEMORY_H
#include <reliefnt/types.h>
/** @brief Allocate long-lived graphics state with its original module user pin.
 * @param size Nonzero payload size, excluding the private ownership prefix.
 * @return Owned, 16-byte-aligned payload or NULL on overflow, unavailable owner
 * or allocation failure. Task context with an already admitted OFD/service
 * call pin; no graphics, registry or module lock may span this allocation.
 * Transfer the object to a queue only after construction. This persistent
 * reference outlives the submitting FD and prevents unloading pending work.
 */
void *drm_async_alloc(size_t size);
/** @brief Free an exact asynchronous allocation and its original owner reference.
 * @param value Owned payload from drm_async_alloc, or NULL; consumed once.
 * Task context after all GPU/queue/backend ownership has ended. Caller must
 * still hold its admitted OFD/service or permanent backend callback pin through
 * return; dropping the object's reference alone cannot protect its caller PC.
 * No graphics lock may span free; no object access follows the final put.
 */
void drm_async_free(void *value);
#endif
