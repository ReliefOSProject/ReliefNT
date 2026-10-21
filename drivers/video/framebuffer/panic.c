#include "internal.h"



/**
 * @brief Reclaim the legacy framebuffer after stopping other panic CPUs.
 * Bypasses the device lock, which the interrupted CPU may already own.
 */
void framebuffer_panic_prepare(void)
{
    __atomic_store_n(&framebuffer_drm_scanout_owned, false, __ATOMIC_RELEASE);
    framebuffer_drm_damage_pending = false;
    if (fb.backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA && vmware_svga.present) {
        vmware_svga_write(VMWARE_SVGA_REG_TRACES, 1u);
        framebuffer_vmware_kick();
    }
}

/**
 * @brief Publish panic pixels without logging, locks or unbounded FIFO waits.
 * The panic owner is the only remaining producer. A wedged GPU is best effort.
 */
void framebuffer_panic_present(void)
{
    if (!fb.available || fb.backend != FRAMEBUFFER_BACKEND_VMWARE_SVGA ||
        !vmware_svga.present) return;
    vmware_svga_memory_fence();
    for (uint32_t attempt = 0; attempt < 4096u; ++attempt) {
        if (framebuffer_vmware_fifo_update(0, 0, fb.width, fb.height)) {
            framebuffer_vmware_kick();
            return;
        }
        if (!vmware_svga.fifo_present) return;
        (void)vmware_svga_read(VMWARE_SVGA_REG_BUSY);
        __asm__ volatile("pause");
    }
}
