/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_DEVICE_MAPPING_H
#define RELIEFNT_DEVICE_MAPPING_H
#include <reliefnt/module.h>

/* Persistent module/builtin callbacks, not userspace ABI. The backend enters
 * create with one acquired backing reference. Fork and VMA splitting share
 * that lease through allocation-free core references. Release and destroy
 * run once after the final core reference, before code may be unmapped. Callbacks run
 * in execution-owned task context without a graph/module registry lock. */
struct device_mapping_operations {
    void (*release)(void *context);
    void (*destroy)(void *context);
    uint64_t (*physical)(void *context,uint64_t offset);
};
struct device_mapping_info {
    uint64_t length;
    uint32_t max_prot;
    /* Splits share the backing; moves preserve offsets and never expand it. */
    bool can_split,can_move;
};
struct device_mapping;
/** @brief Adopt a backing reference and independently retain its provider code.
 * @param owner LIVE module or NULL for permanent builtin code.
 * @param operations Persistent callback table with release/physical.
 * @param context Backend-owned backing state, including its initial reference.
 * @param info Immutable validated page extent/protection capabilities.
 * @param out Empty output receiving one owned mapping reference.
 * @return Zero or negative errno. Ownership transfers only on success; failure
 * leaves context/reference with caller and balances temporary module pins.
 * Task/execution ownership required; no locks span allocations or callbacks.
 */
int device_mapping_create(struct module *owner,const struct device_mapping_operations *operations,
                            void *context,const struct device_mapping_info *info,struct device_mapping **out);
/** @brief Add one backing reference for a VMA clone/split.
 * @param mapping Borrowed valid mapping with at least one caller-owned reference.
 * @return Zero or EINVAL/EOVERFLOW; failure changes no reference. Does not
 * call the backend, allocate or acquire registry locks. Execution ownership
 * serializes access; a hardware disconnect cannot break VMA cloning/splitting.
 */
int device_mapping_get(struct device_mapping *mapping);
/** @brief Consume one VMA backing reference and retire state after the last.
 * @param mapping Owned reference, nullable; consumed once.
 * Execution-owned task context. Backend release/destroy and wrapper free finish
 * before dropping the independent module pin; no graph/module lock is held.
 */
void device_mapping_put(struct device_mapping *mapping);
/** @brief Read immutable mapping capabilities while retaining a reference.
 * @param mapping Borrowed retained mapping.
 * @return Borrowed information valid through that reference, no ownership
 * transfer, allocation or locking. Execution ownership required.
 */
const struct device_mapping_info *device_mapping_info(const struct device_mapping *mapping);
/** @brief Resolve one in-bounds page to an actual owned physical address.
 * @param mapping Borrowed retained mapping.
 * @param offset Page-aligned byte offset within its extent.
 * @return Aligned nonzero physical page, zero for invalid bounds/backing.
 * Execution-owned task context; backing and code remain retained across the
 * callback, without registry locks or new references.
 */
uint64_t device_mapping_physical(struct device_mapping *mapping,uint64_t offset);
#endif
