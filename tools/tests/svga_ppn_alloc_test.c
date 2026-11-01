/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../../drivers/video/svga/device.c"
#include "../../drivers/video/svga/shader.c"

static uint64_t last_mask;
static uint64_t bounded_result = 0x400000000ULL;
static uint32_t fixture_table[2048];
static uint32_t remapped_ppn;
/** @brief Model ordinary allocation returning a page outside PPN32 reach.
 * @param pages Requested page count. @return Deliberately too-high physical base.
 */
uint64_t mm_alloc_pages(uint32_t pages) { assert(pages == 3); return 0x100000000000ULL; }
/** @brief Model a bounded allocation returning a selected synthetic test run.
 * @param pages Requested page count. @param mask Inclusive maximum final byte.
 * @return The configured physical base, including deliberately invalid cases.
 */
uint64_t mm_alloc_pages_below(uint32_t pages, uint64_t mask)
{ assert(pages == 1 || pages == 3); last_mask = mask; return bounded_result; }
/** @brief Ignore fixture page release.
 * @param physical Fixture physical base. @param pages Owned page count.
 */
void mm_free_pages(uint64_t physical, uint32_t pages) { (void)physical; (void)pages; }
/** @brief Supply writable page-table storage for a synthetic physical address.
 * @param physical Fixture physical address. @return Borrowed fixture table.
 */
void *paging_kernel_direct_map(uint64_t physical) { (void)physical; return fixture_table; }
/** @brief Ignore a fixture port write. @param value Value written. @param port I/O port. */
void x86_64_outl(uint32_t value, uint16_t port) { (void)value; (void)port; }
/** @brief Ignore a fixture port read. @param port I/O port. @return Zero. */
uint32_t x86_64_inl(uint16_t port) { (void)port; return 0; }
/** @brief Reset fixture fence state; no hardware is attached. */
void svga_fence_quiescence_reset(void) { }
/** @brief Capture an actual GMR2 remap packet. @param id Command identifier.
 * @param three_d 3D selector. @param spans Packet segments. @param count Segment count.
 * @return Zero to simulate host acceptance.
 */
int svga_fifo_packet_locked(uint32_t id, bool three_d, const struct svga_span *spans, uint32_t count)
{
    (void)three_d;
    if (id == SVGA_CMD_REMAP_GMR2) {
        assert(count == 2 && spans[1].bytes == sizeof(uint32_t));
        remapped_ppn = *(const uint32_t *)spans[1].data;
    }
    return 0;
}
/** @brief Complete fixture host synchronization. @return Zero. */
int svga_sync_locked(void) { return 0; }
/** @brief Verify native binding, GB, GMR and shader PPN32 endpoint handling.
 * @return Zero after accepted exact-end and rejected one-past assertions.
 */
int main(void)
{
    svga_platform_bind(0, NULL, 0);
    assert(svga.ops.alloc(3) == 0x400000000ULL);
    assert(last_mask == ((1ULL << 44) - 1));
    svga.page_limit = 32;
    svga.mob_max_bytes = 65536;
    struct svga_mob mob = {0};
    bounded_result = (1ULL << 44) - 4096;
    assert(svga.ops.alloc(1) == bounded_result);
    assert(!svga_gb_backing_alloc_locked(&mob, 4096));
    svga_gb_backing_free_locked(&mob);
    bounded_result = 1ULL << 44;
    assert(svga_gb_backing_alloc_locked(&mob, 4096) != 0);
    bounded_result = 0x400000000ULL;
    assert(!svga_gb_backing_borrow_locked(&mob, (1ULL << 44) - 3 * 4096, 3, 3 * 4096));
    assert(fixture_table[2] == UINT32_MAX);
    svga_gb_backing_free_locked(&mob);
    assert(svga_gb_backing_borrow_locked(&mob, (1ULL << 44) - 2 * 4096, 3, 3 * 4096) != 0);
    bounded_result = (1ULL << 44) - 4096;
    assert(!shader_backing_alloc(&svga.ops, 4096, &mob));
    svga_shader_backing_free(&svga.ops, &mob);
    bounded_result = 1ULL << 44;
    assert(shader_backing_alloc(&svga.ops, 4096, &mob) != 0);
    svga.available = svga.memory_ready = true;
    svga.gmr_limit = 1; svga.gmr_page_limit = 32; svga.caps = SVGA_CAP_GMR2;
    svga_handle gmr = 0;
    bounded_result = (1ULL << 44) - 4096;
    assert(!svga_gmr_create_locked(4096, &gmr));
    assert(gmr && remapped_ppn == UINT32_MAX);
    assert(!svga_gmr_destroy_locked(0));
    bounded_result = 1ULL << 44;
    assert(svga_gmr_create_locked(4096, &gmr) != 0 && !gmr);
    puts("PASS SVGA native allocations honor the complete PPN32 physical mask");
    return 0;
}
