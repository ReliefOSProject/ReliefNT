/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../drivers/video/drm/drm_internal.h"
#include "../../drivers/video/svga/device.h"

struct svga_device svga;
static unsigned generic_calls, bounded_calls, frees;
/** @brief Supply inaccessible generic high RAM. @param pages Page count. @return Fake physical base. */
uint64_t mm_alloc_pages(uint32_t pages) { assert(pages == 1); ++generic_calls; return (1ULL << 44) + 4096; }
/** @brief Supply reachable RAM under the requested device mask.
 * @param pages Page count. @param mask Inclusive device mask. @return Fake physical base.
 */
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{ assert(pages == 1 && mask == (1ULL << 44) - 1); ++bounded_calls; return (1ULL << 44) - 4096; }
/** @brief Release a fake page. @param physical Physical base. @param pages Page count. */
void mm_free_pages(uint64_t physical, uint32_t pages) { assert(physical && pages == 1); ++frees; }
/** @brief Report inert free memory. @return Fixture KiB. */
uint64_t mm_free_memory_kib(void) { return 0; }
/** @brief Allocate metadata. @param bytes Requested length. @return Owned host memory. */
void *kernel_malloc(size_t bytes) { return calloc(1, bytes); }
/** @brief Release metadata. @param pointer Owned allocation or NULL. */
void kernel_free(void *pointer) { free(pointer); }
/** @brief Lock fixture state. @param lock Lock. @param flags Saved state output. */
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
/** @brief Unlock fixture state. @param lock Lock. @param flags Saved state. */
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ (void)flags; assert(lock->state); lock->state = 0; }
/** @brief Disable failure diagnostics. @return False. */
bool drm_faildiag_is_enabled(void) { return false; }
/** @brief Ignore disabled diagnostics. @param c Context. @param e Error. @param s Stage.
 * @param f Fields. @param n Field count. */
void drm_faildiag_record(struct drm_faildiag_context *c, int e, const char *s,
                         const struct drm_faildiag_field *f, uint32_t n)
{ (void)c; (void)e; (void)s; (void)f; (void)n; }
/** @brief Acquire the fixture device lock. @param flags Saved state output. @return True. */
bool svga_try_lock_irqsave(uint64_t *flags) { *flags = 0; return true; }
/** @brief Release the fixture device lock. @param flags Saved state. */
void svga_unlock(uint64_t flags) { (void)flags; }
/** @brief Permit a simulated mode lease. @return Zero. */
int svga_mode_lease_acquire_locked(void) { return 0; }
/** @brief Construct the one fixture handle. @param kind Resource kind. @param slot Slot. @return Handle. */
svga_handle svga_new_handle(uint32_t kind, uint32_t slot) { (void)kind; assert(!slot); return 1; }
/** @brief Find the fixture GMR. @param handle Published handle. @return Index or -1. */
int svga_find_gmr(svga_handle handle) { return handle == 1 && svga.gmrs[0].handle ? 0 : -1; }
/** @brief Publish one fixture batch. @param words Borrowed words. @param bytes Length.
 * @param fence Writable completion fence. @return Zero. */
int svga_fifo_batch_try_locked(const uint32_t *words, uint32_t bytes, svga_handle *fence)
{ assert(words && bytes); *fence = 1; return 0; }
/** @brief Signal a fixture fence. @param fence Fence handle. @return True. */
bool svga_fence_reached_locked(svga_handle fence) { assert(fence == 1); return true; }
/** @brief Complete a fixture memory fence. */
void svga_barrier(void) { }
/** @brief Retain the BO owner until the simulated device retires. @param owner Borrowed BO. */
static void owner_put(void *owner) { assert(owner); }

/** @brief Exercise real BO allocation and borrowed GMR attachment across the PPN limit.
 * @return Zero after both device-specific and generic placement assertions.
 */
int main(void)
{
    struct drm_device vmware = {.pci = {.vendor_id = 0x15ad, .device_id = 0x0405}};
    struct drm_device other = {.pci = {.vendor_id = 0x1234, .device_id = 0x1111}};
    struct drm_file file = {.device = &vmware};
    uint32_t handle; uint64_t offset;
    assert(!drm_bo_create(&file, NULL, 4096, &handle, &offset));
    struct drm_bo *bo = drm_bo_lookup_handle_get(&file, handle);
    uint64_t physical = drm_bo_page_physical(bo, 0);
    assert(physical == (1ULL << 44) - 4096 && bounded_calls == 1 && !generic_calls);
    svga.available = svga.fifo_ready = svga.gb_active = true;
    svga.caps = SVGA_CAP_GMR2; svga.gmr_limit = 1; svga.gmr_page_limit = 8;
    svga.page_limit = 8; svga.mob_max_bytes = 4096;
    svga_handle gmr = 0;
    assert(svga_gmr_borrowed_attach(physical, 4096, bo, owner_put, NULL, &gmr) == SVGA_EBUSY);
    assert(gmr && !svga_gmr_borrowed_attach(physical, 4096, bo, owner_put, NULL, &gmr));
    svga_gb_backing_free_locked(&svga.external_mobs[0]);
    svga.gmrs[0] = (struct svga_gmr){0};
    drm_bo_put(bo); assert(!drm_bo_handle_delete(&file, handle));
    file.device = &other;
    assert(!drm_bo_create(&file, NULL, 4096, &handle, &offset));
    bo = drm_bo_lookup_handle_get(&file, handle);
    assert(drm_bo_page_physical(bo, 0) > (1ULL << 44) && generic_calls == 1);
    gmr = 0;
    assert(svga_gmr_borrowed_attach(drm_bo_page_physical(bo, 0), 4096, bo, owner_put, NULL, &gmr) != 0 && !gmr);
    drm_bo_put(bo); assert(!drm_bo_handle_delete(&file, handle));
    assert(frees == 2);
    puts("PASS vmwgfx bounded BO reaches real SVGA borrowed attachment; other DRM stays unrestricted");
    return 0;
}
