/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap DEVICE user buffers in fixtures which exercise only RAM. */
#include <reliefnt/device_mapping.h>
#include <reliefnt/paging.h>
#include <stdlib.h>
__attribute__((weak)) int device_user_page_get(struct task *task,uint64_t address,
    uint64_t physical,uint64_t flags,struct device_user_page *out)
{ (void)task;(void)address;(void)physical;(void)flags;(void)out;abort(); }
__attribute__((weak)) void device_user_page_put(struct device_user_page *page)
{ (void)page;abort(); }
/** @brief Trap temporary alias release in fixtures without device mappings.
 * @param address Alias. @param bytes Mapping size. @return Never returns.
 */
__attribute__((weak)) bool paging_iounmap(void *address,uint64_t bytes)
{ (void)address;(void)bytes;abort(); }
/** @brief Trap physical-policy selection outside the fixture's modeled path.
 * @param claim Unused claim. @param physical Unused page. @param flags Unused
 * output. @return Never returns.
 */
__attribute__((weak)) bool paging_cache_claim_select(struct paging_cache_claim *claim,
    uint64_t physical,uint64_t *flags)
{ (void)claim;(void)physical;(void)flags;abort(); }
/** @brief Trap physical-policy coalescing in a fixture with no policy owners.
 * @param first Unused claim. @param second Unused claim. @return Never returns.
 */
__attribute__((weak)) bool paging_cache_claim_merge(struct paging_cache_claim *first,
    struct paging_cache_claim *second)
{ (void)first;(void)second;abort(); }
/** @brief Trap physical-policy release in a fixture with no policy owners.
 * @param claim Unused claim. This function never returns.
 */
__attribute__((weak)) void paging_cache_claim_release(struct paging_cache_claim *claim)
{ (void)claim;abort(); }
/** @brief Trap DEVICE mmap policy admission in a RAM-only fixture.
 * @param mapping Unused backing. @param offset Unused offset. @param physical
 * Unused page. @param flags Unused output. @return Never returns.
 */
__attribute__((weak)) int device_mapping_cache_page(struct device_mapping *mapping,
    uint64_t offset,uint64_t physical,uint64_t *flags)
{ (void)mapping;(void)offset;(void)physical;(void)flags;abort(); }
