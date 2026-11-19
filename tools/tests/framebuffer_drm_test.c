#define _GNU_SOURCE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static uint32_t gfb[16u * 16u];
static unsigned byte_copies_to_scanout;
static __attribute__((unused)) void *fixture_ram_copy(void *destination, const void *source, size_t bytes)
{
    uintptr_t address = (uintptr_t)destination;
    if (address >= (uintptr_t)gfb && address < (uintptr_t)(gfb + 16u * 16u))
        ++byte_copies_to_scanout;
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < bytes; ++i) out[i] = in[i];
    return destination;
}

#include <reliefnt/port.h>
#include <reliefnt/svga.h>
#define __builtin_memcpy fixture_ram_copy
#include "modules/framebuffer.h"
#undef __builtin_memcpy

struct svga_device svga;

static uint32_t fifo_words[16];
static uint32_t doorbells;
static uint32_t busy_reads;

uint64_t svga_lock(void) { return 0; }
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ assert(lock->state == 1 && !flags); lock->state = 0; }
bool svga_try_lock_irqsave(uint64_t *flags)
{
    if (flags) *flags = 0;
    return true;
}
void svga_unlock(uint64_t flags) { (void)flags; }
void svga_barrier(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
void svga_relax(void) {}
static uint32_t fifo_read(uint32_t reg)
{
    if (reg == SVGA_REG_BUSY) ++busy_reads;
    return 0;
}
static void fifo_write(uint32_t reg, uint32_t value)
{
    (void)value;
    if (reg == SVGA_REG_SYNC) ++doorbells;
}
int svga_update_try_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t words[] = {x, y, width, height};
    struct svga_span span = {words, sizeof(words)};
    return svga_fifo_packet_try_locked(SVGA_CMD_UPDATE, false, &span, 1);
}
int svga_update_locked(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    uint32_t words[] = {x, y, width, height};
    struct svga_span span = {words, sizeof(words)};
    return svga_fifo_packet_locked(SVGA_CMD_UPDATE, false, &span, 1);
}

static void test_boot_scanout_takes_only_pending_cpu_damage(void)
{
    fb = (struct framebuffer){.width = 16, .height = 16, .available = true};
    uint64_t seen = framebuffer_generation();
    struct framebuffer_damage taken;
    framebuffer_present_region(3, 5, 2, 1);
    framebuffer_present_region(12, 2, UINT32_MAX, 1);
    assert(framebuffer_take_damage(&seen, &taken));
    assert(taken.x1 == 3 && taken.y1 == 2 && taken.x2 == 16 && taken.y2 == 6);
    assert(!framebuffer_take_damage(&seen, &taken));
    framebuffer_present_region(7, 1, 2, 0);
    framebuffer_present_region(16, 1, 2, 2);
    assert(!framebuffer_take_damage(&seen, &taken));
    framebuffer_present_region(1, 9, 2, 1);
    assert(framebuffer_take_damage(&seen, &taken));
    assert(taken.x1 == 1 && taken.y1 == 9 && taken.x2 == 3 && taken.y2 == 10);
}
bool svga_restore_traces_locked(void) { return false; }
void x86_64_outl(uint32_t value, uint16_t port)
{
    (void)value; (void)port;
    ++doorbells;
}
uint32_t x86_64_inl(uint16_t port) { (void)port; return 0; }

static void setup_fifo_full(void)
{
    memset(&fb, 0, sizeof(fb));
    fb.pixels = gfb;
    fb.width = 16;
    fb.height = 16;
    fb.pitch = 16u * 4u;
    fb.max_width = 16;
    fb.max_height = 16;
    fb.max_bytes = sizeof(gfb);
    fb.backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    fb.bpp = 32;
    fb.bytes_per_pixel = 4;
    fb.available = true;
    memset(gfb, 0, sizeof(gfb));
    memset(fifo_words, 0, sizeof(fifo_words));
    memset(&vmware_svga, 0, sizeof(vmware_svga));
    memset(&svga, 0, sizeof(svga));
    vmware_svga.present = true;
    vmware_svga.io_port = 0x456;
    svga.fifo = fifo_words;
    svga.bound = svga.fifo_ready = true;
    svga.min = 16;
    svga.fifo_bytes = 64;
    svga.ops.read = fifo_read;
    svga.ops.write = fifo_write;
    fifo_words[SVGA_FIFO_MIN] = 16;
    fifo_words[SVGA_FIFO_MAX] = 64;
    fifo_words[SVGA_FIFO_NEXT_CMD] = 40;
    fifo_words[SVGA_FIFO_STOP] = 60;
    framebuffer_drm_scanout_owned = false;
    framebuffer_svga_clear_damage();
    doorbells = 0;
    byte_copies_to_scanout = 0;
}

static void test_full_fifo_damage_is_retained_and_retried(void)
{
    setup_fifo_full();
    assert(framebuffer_drm_scanout_begin() == 0);
    uint32_t source[16u * 16u];
    for (uint32_t i = 0; i < 16u * 16u; ++i) source[i] = 0xff000000u | i;
    assert(framebuffer_drm_present_region(source, 16u * 4u, 0x34325258u,
                                          1, 2, 3, 4) == 0);
    assert(framebuffer_svga_damage_pending);
    assert(gfb[2u * 16u + 1u] == source[2u * 16u + 1u]);
    assert(gfb[3u * 16u + 3u] == 0);
    /* Generic RAM memcpy uses REP MOVSB in the native kernel. UC video
     * memory needs wide pixel stores, not byte-at-a-time scanout uploads. */
    assert(byte_copies_to_scanout == 0);

    source[10u * 16u + 8u] = 0xffabcdefu;
    assert(framebuffer_drm_present_region(source, 16u * 4u, 0x34325258u,
                                          8, 10, 9, 11) == 0);
    assert(framebuffer_svga_damage_pending);
    assert(gfb[10u * 16u + 8u] == 0xffabcdefu);

    fifo_words[SVGA_FIFO_NEXT_CMD] = 40;
    fifo_words[SVGA_FIFO_STOP] = 40;
    framebuffer_display_tick(false);
    assert(!framebuffer_svga_damage_pending);
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 60);
    assert(fifo_words[10] == SVGA_CMD_UPDATE);
    assert(fifo_words[11] == 1 && fifo_words[12] == 2 &&
           fifo_words[13] == 8 && fifo_words[14] == 9);
    assert(doorbells > 0);

    framebuffer_drm_scanout_release();
    assert(!framebuffer_svga_damage_pending && !framebuffer_drm_scanout_owned);
    framebuffer_display_tick(true);
    /* Wrap through the tail instead of skipping bytes the host will parse. */
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 32);
    assert(fifo_words[15] == SVGA_CMD_UPDATE);
    assert(fifo_words[4] == 0 && fifo_words[5] == 0 &&
           fifo_words[6] == 16 && fifo_words[7] == 16);
}

static void test_explicit_present_has_nonblocking_backpressure(void)
{
    setup_fifo_full();
    busy_reads = 0;
    framebuffer_present_region(2, 3, 4, 5);
    assert(busy_reads <= 1);
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 40);
    framebuffer_present_region(3, 5, 7, 8);
    assert(busy_reads <= 2);
    fifo_words[SVGA_FIFO_STOP] = 40;
    framebuffer_display_tick(false);
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 60);
    assert(fifo_words[10] == SVGA_CMD_UPDATE && fifo_words[11] == 2 &&
           fifo_words[12] == 3 && fifo_words[13] == 8 && fifo_words[14] == 10);
    framebuffer_display_tick(false);
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 60);
}

static void test_panic_forces_deferred_notification(void)
{
    setup_fifo_full();
    svga.fifo_notify_deferred = true;
    fifo_words[SVGA_FIFO_STOP] = 40;
    assert(!svga_update_panic(16, 16));
    assert(doorbells && !svga.fifo_notify_deferred);
    assert(fifo_words[10] == SVGA_CMD_UPDATE);
    /* A half-written RESERVE transaction must not be overwritten. */
    svga.fifo_caps = SVGA_FIFO_CAP_RESERVE;
    fifo_words[SVGA_FIFO_RESERVED] = 24;
    assert(svga_update_panic(16, 16) == SVGA_EBUSY);
    assert(fifo_words[SVGA_FIFO_NEXT_CMD] == 60);
}

static void test_exact_pixel_extents(void)
{
    unsigned char *pages = mmap(NULL, 8192, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(pages != MAP_FAILED && !mprotect(pages + 4096, 4096, PROT_NONE));
    framebuffer_copy_io32(NULL, NULL, 0);
    for (unsigned pixels = 1; pixels <= 17; ++pixels) {
        unsigned char *source = pages + 4096 - pixels * 4;
        for (unsigned i = 0; i < pixels * 4; ++i) source[i] = (unsigned char)(i ^ pixels);
        for (unsigned left = 0; left < 2; ++left) {
            memset(gfb, 0xa5, sizeof(gfb));
            framebuffer_copy_io32(gfb + left, source, pixels);
            assert(!memcmp(gfb + left, source, pixels * 4));
            if (left) assert(gfb[0] == 0xa5a5a5a5u);
            assert(gfb[left + pixels] == 0xa5a5a5a5u);
        }
    }
    unsigned char unaligned[69];
    for (unsigned i = 0; i < sizeof(unaligned); ++i) unaligned[i] = (unsigned char)i;
    framebuffer_copy_io32(gfb + 1, unaligned + 1, 17);
    assert(!memcmp(gfb + 1, unaligned + 1, 68));
    assert(!munmap(pages, 8192));
}

int main(void)
{
    test_boot_scanout_takes_only_pending_cpu_damage();
    test_exact_pixel_extents();
    test_full_fifo_damage_is_retained_and_retried();
    test_explicit_present_has_nonblocking_backpressure();
    test_panic_forces_deferred_notification();
    puts("framebuffer-drm: PASS");
    return 0;
}
