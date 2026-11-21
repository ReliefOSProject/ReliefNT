#include <reliefnt/export.h>
#include <reliefnt/lock.h>
#include "internal.h"

static struct framebuffer_damage framebuffer_damage;
static struct kernel_spinlock framebuffer_damage_lock = KERNEL_SPINLOCK_INIT;
static uint64_t framebuffer_damage_generation;

/** @brief Read the permanent CPU framebuffer's explicit publication generation.
 * @return Atomic damage snapshot; no lock, allocation or optional code.
 */
uint64_t framebuffer_generation(void)
{ return __atomic_load_n(&framebuffer_damage_generation,__ATOMIC_ACQUIRE); }

/** @brief Transfer accumulated pixel bounds to the normal scanout consumer. */
bool framebuffer_take_damage(uint64_t *generation, struct framebuffer_damage *damage)
{
    if (*generation == framebuffer_generation()) return false;
    uint64_t flags;
    kernel_spin_lock_irqsave(&framebuffer_damage_lock, &flags);
    bool pending = framebuffer_damage.x1 < framebuffer_damage.x2;
    if (pending) {
        *damage = framebuffer_damage;
        *generation = __atomic_load_n(&framebuffer_damage_generation, __ATOMIC_RELAXED);
        framebuffer_damage = (struct framebuffer_damage){0};
    }
    kernel_spin_unlock_irqrestore(&framebuffer_damage_lock, flags);
    return pending;
}

/** @brief Record a clipped CPU update without holding a hardware lock. */
static void framebuffer_record_damage(struct framebuffer_damage damage)
{
    uint64_t flags;
    kernel_spin_lock_irqsave(&framebuffer_damage_lock, &flags);
    framebuffer_damage_union(&framebuffer_damage, damage);
    uint64_t generation = __atomic_load_n(&framebuffer_damage_generation, __ATOMIC_RELAXED);
    __atomic_store_n(&framebuffer_damage_generation, generation + 1, __ATOMIC_RELEASE);
    kernel_spin_unlock_irqrestore(&framebuffer_damage_lock, flags);
}

void framebuffer_present_region(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    if (!fb.available || !width || !height || x >= fb.width || y >= fb.height)
        return;
    if (width > fb.width - x) width = fb.width - x;
    if (height > fb.height - y) height = fb.height - y;
    framebuffer_record_damage((struct framebuffer_damage){x, y, x + width, y + height});
}

void framebuffer_present(void)
{
    framebuffer_present_region(0, 0, UINT32_MAX, UINT32_MAX);
}

EXPORT_SYMBOL(framebuffer_generation);
EXPORT_SYMBOL(framebuffer_take_damage);
