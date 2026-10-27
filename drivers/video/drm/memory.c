/* SPDX-License-Identifier: Apache-2.0 */
#include "memory.h"
#include <reliefnt/heap.h>
#include <reliefnt/module.h>
union drm_async_header {struct module *owner;unsigned __int128 alignment;};
_Static_assert(sizeof(union drm_async_header)==16,"DRM owner prefix preserves heap alignment");
/** @brief Construct one independently pinned asynchronous graphics allocation.
 * @param size Nonzero payload byte count.
 * @return Owned aligned payload or NULL on unavailable owner, overflow or OOM.
 * Task context under a pre-existing provider call/OFD pin. Acquires a user pin
 * before heap allocation and rolls it back on failure; no lock spans the heap.
 */
void *drm_async_alloc(size_t size)
{
    if(!size || size>SIZE_MAX-sizeof(union drm_async_header))return NULL;
    struct module *owner=THIS_MODULE;
    if(!try_module_get(owner))return NULL;
    union drm_async_header *header=kernel_malloc(sizeof(*header)+size);
    if(!header){module_put(owner);return NULL;}
    header->owner=owner;return header+1;
}
/** @brief Consume one exact payload after asynchronous backend ownership drains.
 * @param value Owned drm_async_alloc payload, nullable, consumed once.
 * Task context with the caller's independent provider/callback pin still held.
 * Captures the owner before heap free and performs no access after module_put;
 * no graphics/registry/module lock spans heap release.
 */
void drm_async_free(void *value)
{
    if(!value)return;
    union drm_async_header *header=(union drm_async_header *)value-1;
    struct module *owner=header->owner;
    kernel_free(header);module_put(owner);
}
