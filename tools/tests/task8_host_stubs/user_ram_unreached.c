/* SPDX-License-Identifier: Apache-2.0 */
/** @file @brief Trap user-page staging from kernel-buffer-only host fixtures. */
#include <reliefnt/paging.h>
#include <reliefnt/mm.h>
#include <stdlib.h>
/** @brief Trap an unexpected user-page query. */
uint64_t address_space_user_page_flags(const struct address_space *as,uint64_t address)
{ (void)as;(void)address;abort(); }
/** @brief Trap an unexpected page alias. */
void *mm_phys_to_virt(uint64_t physical) { (void)physical;abort(); }
/** @brief Trap an unexpected page reference query. */
uint32_t mm_page_refcount(uint64_t physical) { (void)physical;abort(); }
/** @brief Trap an unexpected RAM retain. */
void mm_retain_page(uint64_t physical) { (void)physical;abort(); }
/** @brief Trap an unexpected RAM release. */
void mm_free_page(uint64_t physical) { (void)physical;abort(); }
/** @brief Trap an unexpected cache retain. */
int page_cache_retain(uint64_t physical) { (void)physical;abort(); }
/** @brief Trap an unexpected cache release. */
void page_cache_release(uint64_t physical) { (void)physical;abort(); }
