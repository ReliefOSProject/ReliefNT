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
struct task;
/** A retained DEVICE page and cache-compatible supervisor alias. */
struct device_user_page {
    struct device_mapping *mapping;
    void *alias;
    uint64_t offset;
    bool temporary;
};
/** @brief Retain one admitted DEVICE page and a compatible kernel alias.
 * @param task Live user task under the outer execution writer.
 * @param address Already validated user byte address.
 * @param physical Original aligned physical page from the admitted PTE.
 * @param flags Original leaf flags, including its exact cache policy.
 * @param out Empty owner receiving backing and alias on success.
 * @return Zero, EFAULT for an unowned/changed leaf, EIO for an existing
 * conflicting direct policy, EOVERFLOW for a saturated provider reference,
 * or ENOMEM when an owned high alias cannot be made.
 * A typed VMA retains its mapping/provider/code; the builtin FB0 VMA has a
 * permanent backing. No user VA or VMA pointer survives return.
 */
int device_user_page_get(struct task *task,uint64_t address,uint64_t physical,
                         uint64_t flags,struct device_user_page *out);
/** @brief Retire a device alias before dropping its original backing reference.
 * @param page Owned page, cleared before its final provider callback.
 * Execution-owned task context, without scheduler/graph/provider spinlocks.
 */
void device_user_page_put(struct device_user_page *page);
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
/** @brief Wrap one provider-owned reference in an independent mmap admission.
 * @param mapping Pointer to the provider-owned reference; replaced on success.
 * @return Zero or ENOMEM/EINVAL. Failure leaves the input reference unchanged;
 * success transfers it into a private view with independent per-page claims.
 * Execution-owned task context; allocation occurs outside spin/provider locks.
 */
int device_mapping_admit_view(struct device_mapping **mapping);
/** @brief Add one backing reference for a VMA clone/split.
 * @param mapping Borrowed valid mapping with at least one caller-owned reference.
 * @return Zero or EINVAL/EOVERFLOW; failure changes no reference. Does not
 * call the backend, allocate or acquire registry locks. Execution ownership
 * serializes access; all shares retain the original cache-policy claims, and a
 * hardware disconnect cannot break VMA cloning/splitting.
 */
int device_mapping_get(struct device_mapping *mapping);
/** @brief Consume one VMA backing reference and retire state after the last.
 * @param mapping Owned reference, nullable; consumed once.
 * Execution-owned task context. Cache claims remain held through backend
 * release/destroy, then retire before the independent module pin is dropped;
 * no graph/module lock is held.
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
/** @brief Own the selected physical cache policy before installing a DEVICE leaf.
 * @param mapping Retained original backing. @param offset Aligned backing offset.
 * @param physical Physical page just resolved from this mapping.
 * @param flags Receives exact PWT/PCD/PAT bits for the new user PTE.
 * @return Zero, EFAULT for a changed/invalid physical identity, EIO for an
 * inconsistent policy, or ENOMEM. An admission view records one initial page
 * owner; VMA clones and retained user-copy stages add range owners separately.
 * Task context under execution ownership; no callback runs under the MMIO lock.
 */
int device_mapping_cache_page(struct device_mapping *mapping,uint64_t offset,
                              uint64_t physical,uint64_t *flags);
/** @brief Retain a published view's page-policy owners without allocation.
 * @param mapping Retained admission view. @param offset Aligned first offset.
 * @param length Nonzero aligned byte count. @return Zero or negative errno;
 * failure leaves every count unchanged. Execution ownership is required.
 */
int device_mapping_retain_range(struct device_mapping *mapping,uint64_t offset,uint64_t length);
/** @brief Preflight the same range retain without changing any owner count.
 * @param mapping Retained admission view. @param offset Aligned first offset.
 * @param length Nonzero aligned byte count. @return Zero or negative errno.
 * Execution ownership keeps the validated owners stable across preparation.
 */
int device_mapping_check_range(struct device_mapping *mapping,uint64_t offset,uint64_t length);
/** @brief Mark one view reference that this complete operation will retire.
 * @param mapping Borrowed view reference, nullable. Execution ownership is
 * required. Pair each mark with an end before any marked reference is put;
 * no allocation or provider callback occurs.
 */
void device_mapping_retire_begin(struct device_mapping *mapping);
/** @brief End one whole-operation retirement mark before its reference is put.
 * @param mapping Borrowed previously marked view, nullable. Execution
 * ownership is required; no allocation or provider callback occurs.
 */
void device_mapping_retire_end(struct device_mapping *mapping);
/** @brief Retire page-policy owners after the corresponding aliases' TLB flush.
 * @param mapping Retained admission view. @param offset Aligned first offset.
 * @param length Nonzero aligned byte count. @param final_put True when the
 * caller immediately drops this view reference after the range is retired.
 * No allocation or provider callback occurs; execution ownership is required.
 */
void device_mapping_release_range(struct device_mapping *mapping,uint64_t offset,
                                  uint64_t length,bool final_put);
#endif
