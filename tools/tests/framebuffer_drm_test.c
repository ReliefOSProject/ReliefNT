#include <assert.h>
#include <stdio.h>
#include "modules/framebuffer.h"

void kernel_spin_lock_irqsave(struct kernel_spinlock *lock, uint64_t *flags)
{ assert(!lock->state); lock->state = 1; *flags = 0; }
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock, uint64_t flags)
{ assert(lock->state == 1 && !flags); lock->state = 0; }
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

int main(void)
{
    test_boot_scanout_takes_only_pending_cpu_damage();
    puts("PASS framebuffer boot scanout tracks and clips pending damage");
    return 0;
}
