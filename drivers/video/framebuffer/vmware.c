#include "internal.h"



uint32_t vmware_svga_read(uint32_t reg)
{
    x86_64_outl(reg, vmware_svga.io_port);
    return x86_64_inl((uint16_t)(vmware_svga.io_port + 1u));
}

void vmware_svga_write(uint32_t reg, uint32_t value)
{
    x86_64_outl(reg, vmware_svga.io_port);
    x86_64_outl(value, (uint16_t)(vmware_svga.io_port + 1u));
}

void vmware_svga_memory_fence(void)
{
    __asm__ volatile("mfence" ::: "memory");
}

/**
 * @brief Enable VMware GFB write tracing for mmap-backed fbdev clients.
 * @return None.
 *
 * Xorg's fbdev shadow framebuffer writes the scan-out surface through its
 * mmap.  Once the SVGA FIFO is enabled those writes do not imply an UPDATE
 * unless SVGA_REG_TRACES is explicitly enabled.  Keep the capability check
 * here so mode/FIFO reinitialisation restores the same display semantics.
 */
void framebuffer_vmware_enable_traces(void)
{
    uint64_t flags = svga_lock();
    if (!vmware_svga.present || !vmware_svga.fifo_present) {
        svga.traces_enabled = false;
        svga_unlock(flags);
        return;
    }
    (void)svga_restore_traces_locked();
    svga_unlock(flags);
}

void framebuffer_vmware_fifo_init(bool report, bool device_lock_held)
{
    uint32_t mem_start = vmware_svga_read(VMWARE_SVGA_REG_MEM_START);
    uint32_t mem_size = vmware_svga_read(VMWARE_SVGA_REG_MEM_SIZE);

    /* SVGA II may discard the FIFO contents when ENABLE or the display mode
     * changes.  Keep the device in the unconfigured state while replacing
     * the guest-owned ring, then publish CONFIG_DONE only after every header
     * field has been written. */
    vmware_svga_write(VMWARE_SVGA_REG_CONFIG_DONE, 0u);
    svga.traces_enabled = false;
    vmware_svga.fifo = 0;
    vmware_svga.fifo_present = false;
    fb.auxiliary_reservation_start = 0;
    fb.auxiliary_reservation_bytes = 0;
    /* The producer must retain one free byte to distinguish a full ring from
     * an empty one; a command-sized ring can therefore never carry a packet. */
    if (mem_size <= VMWARE_SVGA_FIFO_MIN_BYTES +
                        VMWARE_SVGA_UPDATE_WORDS * sizeof(uint32_t) ||
        mem_size > FRAMEBUFFER_MAX_VRAM_BYTES ||
        (mem_start & (sizeof(uint32_t) - 1u)) != 0u ||
        !framebuffer_range_valid(mem_start, mem_size)) {
        if (report) {
            console_warn("[reliefnt] VMware SVGA FIFO unavailable mem=%p size=%u\n",
                           (void *)(uintptr_t)mem_start, mem_size);
        }
        return;
    }

    vmware_svga.fifo = (volatile uint32_t *)(uintptr_t)mem_start;
    vmware_svga.fifo_min = VMWARE_SVGA_FIFO_MIN_BYTES;
    vmware_svga.fifo_max = mem_size;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_MIN] = vmware_svga.fifo_min;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_MAX] = vmware_svga.fifo_max;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD] = vmware_svga.fifo_min;
    vmware_svga.fifo[VMWARE_SVGA_FIFO_STOP] = vmware_svga.fifo_min;
    vmware_svga_memory_fence();
    vmware_svga_write(VMWARE_SVGA_REG_CONFIG_DONE, 1u);

    /* A broken or stale MMIO mapping must not be treated as a working FIFO.
     * Read back all four control words after CONFIG_DONE so the present path
     * can recover instead of silently dropping every update. */
    if (vmware_svga.fifo[VMWARE_SVGA_FIFO_MIN] != vmware_svga.fifo_min ||
        vmware_svga.fifo[VMWARE_SVGA_FIFO_MAX] != vmware_svga.fifo_max ||
        vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD] != vmware_svga.fifo_min ||
        vmware_svga.fifo[VMWARE_SVGA_FIFO_STOP] != vmware_svga.fifo_min) {
        vmware_svga.fifo = 0;
        if (report) {
            console_error("[reliefnt] VMware SVGA FIFO header validation failed\n");
        }
        return;
    }
    vmware_svga.fifo_full_logged = false;
    vmware_svga.fifo_present = true;
    svga_platform_bind(vmware_svga.io_port, vmware_svga.fifo, mem_size);
    if (device_lock_held) {
        (void)svga_restore_traces_locked();
    } else {
        framebuffer_vmware_enable_traces();
    }
    fb.auxiliary_reservation_start = mem_start;
    fb.auxiliary_reservation_bytes = mem_size;
}

int framebuffer_vmware_fifo_update(uint32_t x, uint32_t y,
                                          uint32_t width, uint32_t height)
{
    uint32_t next;
    uint32_t stop;
    uint32_t bytes = VMWARE_SVGA_UPDATE_WORDS * sizeof(uint32_t);
    uint32_t index;
    uint32_t fifo_min = (svga.bound && svga.fifo_ready) ? svga.min : vmware_svga.fifo_min;

    if (!vmware_svga.fifo_present || !vmware_svga.fifo || !width || !height) {
        return 0;
    }
    next = vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD];
    stop = vmware_svga.fifo[VMWARE_SVGA_FIFO_STOP];
    if (next < fifo_min || next >= vmware_svga.fifo_max ||
        stop < fifo_min || stop >= vmware_svga.fifo_max) {
        vmware_svga.fifo_present = false;
        return 0;
    }
    if (!framebuffer_fifo_reserve(fifo_min, vmware_svga.fifo_max, next, stop,
                                  bytes, &index, &next)) {
        goto full;
    }

    index /= sizeof(uint32_t);
    vmware_svga.fifo[index] = VMWARE_SVGA_CMD_UPDATE;
    vmware_svga.fifo[index + 1u] = x;
    vmware_svga.fifo[index + 2u] = y;
    vmware_svga.fifo[index + 3u] = width;
    vmware_svga.fifo[index + 4u] = height;
    vmware_svga_memory_fence();
    vmware_svga.fifo[VMWARE_SVGA_FIFO_NEXT_CMD] = next;
    vmware_svga_memory_fence();
    return 1;

full:
    if (!vmware_svga.fifo_full_logged) {
        vmware_svga.fifo_full_logged = true;
    }
    /* A legacy host may leave STOP unchanged until it receives a doorbell.
     * The preceding packet normally rings the doorbell, but a burst can fill
     * the ring after that publication and leave the next periodic refresh
     * with no packet to wake the host.  Re-issue the non-blocking doorbell on
     * the full path; a later tick retries once the host advances STOP. */
    framebuffer_vmware_kick();
    return 0;
}

void framebuffer_vmware_kick(void)
{
    if (!vmware_svga.present) {
        return;
    }
    vmware_svga_write(VMWARE_SVGA_REG_SYNC, 1u);
}

void framebuffer_vmware_sync(bool report)
{
    if (!vmware_svga.present) {
        return;
    }
    vmware_svga_write(VMWARE_SVGA_REG_SYNC, 1u);
    for (uint32_t spins = 0; spins < VMWARE_SVGA_SYNC_POLL_LIMIT; ++spins) {
        if (vmware_svga_read(VMWARE_SVGA_REG_BUSY) == 0u) {
            return;
        }
    }
    if (!vmware_svga.sync_timeout_logged) {
        if (report) {
            console_warn("[reliefnt] VMware SVGA sync timed out; continuing asynchronously\n");
        }
        vmware_svga.sync_timeout_logged = true;
    }
}

int framebuffer_vmware_probe(void)
{
    struct pci_device device;
    uint32_t bar;
    uint32_t id;
    uint32_t fb_start;
    uint32_t fb_offset;
    uint32_t fb_max_size;
    uint32_t fb_size;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t bpp;
    uint32_t pseudocolor;
    uint32_t pitch;
    uint32_t enabled;
    int device_mode_valid;
    int boot_mode_valid;
    uint64_t usable_bytes;
    uint32_t io_base;

    if (!fb.available || pci_find_device(VMWARE_VENDOR_ID, VMWARE_SVGA_DEVICE_ID,
                                         &device) < 0) {
        return 0;
    }
    bar = pci_config_read32(device.bus, device.slot, device.function, 0x10);
    io_base = bar & 0xfffcu;
    if ((bar & 1u) == 0 || io_base == 0 || io_base > 0xfffeu) {
        return 0;
    }
    vmware_svga.io_port = (uint16_t)io_base;
    pci_config_write16(device.bus, device.slot, device.function, 0x04,
                       (uint16_t)(pci_config_read16(device.bus, device.slot,
                                                     device.function, 0x04) |
                                  VMWARE_PCI_COMMAND_IO |
                                  VMWARE_PCI_COMMAND_MEMORY));
    vmware_svga_write(VMWARE_SVGA_REG_ID, VMWARE_SVGA_ID_2);
    id = vmware_svga_read(VMWARE_SVGA_REG_ID);
    if (id < VMWARE_SVGA_ID_2) {
        return 0;
    }

    fb_start = vmware_svga_read(VMWARE_SVGA_REG_FB_START);
    fb_offset = vmware_svga_read(VMWARE_SVGA_REG_FB_OFFSET);
    fb_max_size = vmware_svga_read(VMWARE_SVGA_REG_FB_MAX_SIZE);
    fb_size = vmware_svga_read(VMWARE_SVGA_REG_FB_SIZE);
    if (!fb_max_size || fb_max_size > FRAMEBUFFER_MAX_VRAM_BYTES ||
        fb_offset >= fb_max_size ||
        !framebuffer_range_valid(fb_start, fb_max_size)) {
        return 0;
    }
    /* FB_SIZE describes the current scan-out surface.  It grows and shrinks
     * with the mode, whereas FB_MAX_SIZE is the SVGA II VRAM capacity. */
    usable_bytes = (uint64_t)fb_max_size - fb_offset;
    if (usable_bytes < framebuffer_current_bytes()) {
        console_error("[reliefnt] VMware SVGA framebuffer too small fb=%p offset=%u "
                       "size=%u max=%u current=%u\n",
                       (void *)(uintptr_t)fb_start, fb_offset, fb_size, fb_max_size,
                       (uint32_t)framebuffer_current_bytes());
        return 0;
    }

    /* The boot protocol framebuffer is only a hint.  SVGA II exposes the
     * scan-out surface through FB_START/FB_OFFSET, which may differ from the
     * address supplied at boot (notably the EFI GOP surface).  Keep
     * fbdev mmap and native drawing on the device's actual linear surface and
     * publish the active SVGA mode so a same-mode Xorg probe does not retain
     * stale boot geometry or pitch. */
    width = vmware_svga_read(VMWARE_SVGA_REG_WIDTH);
    height = vmware_svga_read(VMWARE_SVGA_REG_HEIGHT);
    depth = vmware_svga_read(VMWARE_SVGA_REG_DEPTH);
    bpp = vmware_svga_read(VMWARE_SVGA_REG_BITS_PER_PIXEL);
    pseudocolor = vmware_svga_read(VMWARE_SVGA_REG_PSEUDOCOLOR);
    pitch = vmware_svga_read(VMWARE_SVGA_REG_BYTES_PER_LINE);
    enabled = vmware_svga_read(VMWARE_SVGA_REG_ENABLE);
    device_mode_valid = width && height && depth == 24u && bpp == 32u &&
        pseudocolor == 0u && (uint64_t)pitch >= (uint64_t)width * 4u &&
        (uint64_t)pitch * height <= usable_bytes &&
        framebuffer_range_valid((uint64_t)fb_start + fb_offset,
                                (uint64_t)pitch * height);
    boot_mode_valid = fb.available && fb.width && fb.height &&
        (uint64_t)fb.pitch >= (uint64_t)fb.width * 4u &&
        (uint64_t)fb.pitch * fb.height <= usable_bytes &&
        framebuffer_range_valid((uint64_t)(uintptr_t)fb.pixels,
                                (uint64_t)fb.pitch * fb.height);
    /* The geometry registers describe the live scan-out only once the device
     * has been enabled.  While REG_ENABLE is clear they still hold the
     * power-on default; activate the device at the boot handover geometry below so
     * Xorg inherits the same surface without the stale default pitch. */
    if ((enabled & 1u) == 0u && boot_mode_valid) {
        /* SVGA stays disabled while the boot handover GOP surface is visible.
         * Xorg later writes the same mmap, so leave the device enabled before
         * it starts; otherwise REG_TRACES is accepted but never drives the
         * host scan-out and the first client that clears the greeter leaves a
         * black screen.  Reprogram the boot handover geometry explicitly so the
         * device surface and the inherited framebuffer stay identical. */
        vmware_svga_write(VMWARE_SVGA_REG_ENABLE, 0u);
        vmware_svga_write(VMWARE_SVGA_REG_WIDTH, fb.width);
        vmware_svga_write(VMWARE_SVGA_REG_HEIGHT, fb.height);
        vmware_svga_write(VMWARE_SVGA_REG_DEPTH, 24u);
        vmware_svga_write(VMWARE_SVGA_REG_BITS_PER_PIXEL, 32u);
        vmware_svga_write(VMWARE_SVGA_REG_PSEUDOCOLOR, 0u);
        vmware_svga_write(VMWARE_SVGA_REG_ENABLE, 1u);
        width = vmware_svga_read(VMWARE_SVGA_REG_WIDTH);
        height = vmware_svga_read(VMWARE_SVGA_REG_HEIGHT);
        depth = vmware_svga_read(VMWARE_SVGA_REG_DEPTH);
        bpp = vmware_svga_read(VMWARE_SVGA_REG_BITS_PER_PIXEL);
        pseudocolor = vmware_svga_read(VMWARE_SVGA_REG_PSEUDOCOLOR);
        pitch = vmware_svga_read(VMWARE_SVGA_REG_BYTES_PER_LINE);
        enabled = vmware_svga_read(VMWARE_SVGA_REG_ENABLE);
        device_mode_valid = (enabled & 1u) != 0u && width && height &&
            depth == 24u && bpp == 32u && pseudocolor == 0u &&
            (uint64_t)pitch >= (uint64_t)width * 4u &&
            (uint64_t)pitch * height <= usable_bytes &&
            framebuffer_range_valid((uint64_t)fb_start + fb_offset,
                                    (uint64_t)pitch * height);
        if (device_mode_valid) {
            console_info("[reliefnt] VMware SVGA scan-out enabled at boot mode "
                           "%ux%u pitch=%u\n", width, height, pitch);
        }
    }
    if (device_mode_valid && (enabled & 1u) != 0u) {
        fb.pixels = (uint32_t *)(uintptr_t)((uint64_t)fb_start + fb_offset);
        fb.width = width;
        fb.height = height;
        fb.pitch = pitch;
        fb.bpp = (uint8_t)bpp;
        fb.bytes_per_pixel = 4u;
        framebuffer_set_default_format();
    } else {
        console_warn("[reliefnt] VMware SVGA active mode unavailable "
                       "mode=%ux%u depth=%u bpp=%u pseudo=%u pitch=%u\n",
                       width, height, depth, bpp, pseudocolor, pitch);
        return 0;
    }

    vmware_svga.max_width = vmware_svga_read(VMWARE_SVGA_REG_MAX_WIDTH);
    vmware_svga.max_height = vmware_svga_read(VMWARE_SVGA_REG_MAX_HEIGHT);
    if (!vmware_svga.max_width || !vmware_svga.max_height) {
        return 0;
    }
    if (vmware_svga.max_width > FRAMEBUFFER_MODE_MAX_WIDTH) {
        vmware_svga.max_width = FRAMEBUFFER_MODE_MAX_WIDTH;
    }
    if (vmware_svga.max_height > FRAMEBUFFER_MODE_MAX_HEIGHT) {
        vmware_svga.max_height = FRAMEBUFFER_MODE_MAX_HEIGHT;
    }
    vmware_svga.present = true;
    int drm_ret = drm_device_register(&device);
    if (drm_ret < 0) {
        console_warn("[reliefnt] VMware DRM primary node unavailable ret=%d\n",
                     drm_ret);
    }
    framebuffer_vmware_fifo_init(true, false);

    fb.max_width = vmware_svga.max_width;
    fb.max_height = vmware_svga.max_height;
    fb.max_bytes = (uint32_t)usable_bytes;
    fb.backend = FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    fb.capabilities = FRAMEBUFFER_CAP_MODE_SET;
    fb.reservation_start = fb_start;
    fb.reservation_bytes = fb_max_size;
    console_info("[reliefnt] VMware SVGA detected io=0x%x fb=%p offset=%u size=%u "
                   "max=%u limits=%ux%u fifo=%p\n",
                   vmware_svga.io_port, (void *)(uintptr_t)fb_start, fb_offset,
                   fb_size, fb_max_size, vmware_svga.max_width,
                   vmware_svga.max_height, (void *)vmware_svga.fifo);
    return 1;
}

int framebuffer_vmware_set_mode(uint32_t width, uint32_t height)
{
    uint32_t actual_width;
    uint32_t actual_height;
    uint32_t depth;
    uint32_t bpp;
    uint32_t pseudocolor;
    uint32_t pitch;
    uint32_t fb_start;
    uint32_t fb_offset;
    uint32_t fb_max_size;
    uint32_t fb_size;
    uint64_t usable_bytes;
    uint64_t svga_flags = 0;
    uint64_t legacy_flags = 0;
    bool restart_3d = svga.available;

    if (!vmware_svga.present) {
        return 0;
    }
    if (restart_3d && svga_mode_begin(&svga_flags) != 0) {
        return 0;
    }
    if (!restart_3d) {
        legacy_flags = svga_lock();
    }
    fb.auxiliary_reservation_start = 0;
    fb.auxiliary_reservation_bytes = 0;
    vmware_svga_write(VMWARE_SVGA_REG_ENABLE, 0u);
    vmware_svga_write(VMWARE_SVGA_REG_WIDTH, width);
    vmware_svga_write(VMWARE_SVGA_REG_HEIGHT, height);
    vmware_svga_write(VMWARE_SVGA_REG_DEPTH, 24u);
    vmware_svga_write(VMWARE_SVGA_REG_BITS_PER_PIXEL, 32u);
    vmware_svga_write(VMWARE_SVGA_REG_PSEUDOCOLOR, 0u);
    vmware_svga_write(VMWARE_SVGA_REG_ENABLE, 1u);
    /* Rebuild the legacy FIFO before 3D is initialized. When 3D was already
     * active, mode_begin left the FIFO disabled and mode_end restores the
     * extended register block after the mode is accepted. */
    if (!restart_3d) {
        framebuffer_vmware_fifo_init(false, true);
    }

    actual_width = vmware_svga_read(VMWARE_SVGA_REG_WIDTH);
    actual_height = vmware_svga_read(VMWARE_SVGA_REG_HEIGHT);
    depth = vmware_svga_read(VMWARE_SVGA_REG_DEPTH);
    bpp = vmware_svga_read(VMWARE_SVGA_REG_BITS_PER_PIXEL);
    pseudocolor = vmware_svga_read(VMWARE_SVGA_REG_PSEUDOCOLOR);
    pitch = vmware_svga_read(VMWARE_SVGA_REG_BYTES_PER_LINE);
    fb_start = vmware_svga_read(VMWARE_SVGA_REG_FB_START);
    fb_offset = vmware_svga_read(VMWARE_SVGA_REG_FB_OFFSET);
    fb_max_size = vmware_svga_read(VMWARE_SVGA_REG_FB_MAX_SIZE);
    fb_size = vmware_svga_read(VMWARE_SVGA_REG_FB_SIZE);
    if (actual_width != width || actual_height != height || depth != 24u ||
        bpp != 32u || pseudocolor != 0u ||
        !fb_max_size || fb_max_size > FRAMEBUFFER_MAX_VRAM_BYTES ||
        fb_offset >= fb_max_size || pitch < width * 4u ||
        !framebuffer_range_valid(fb_start, fb_max_size)) {
        if (restart_3d) svga_mode_end(svga_flags);
        else svga_unlock(legacy_flags);
        console_error("[reliefnt] VMware SVGA rejected mode req=%ux%u got=%ux%u "
                       "depth=%u bpp=%u pseudo=%u pitch=%u fb=%p offset=%u size=%u max=%u\n",
                       width, height, actual_width, actual_height, depth, bpp,
                       pseudocolor, pitch, (void *)(uintptr_t)fb_start, fb_offset,
                       fb_size, fb_max_size);
        return 0;
    }
    usable_bytes = (uint64_t)fb_max_size - fb_offset;
    if ((uint64_t)pitch * height > usable_bytes) {
        if (restart_3d) svga_mode_end(svga_flags);
        else svga_unlock(legacy_flags);
        return 0;
    }
    fb.pixels = (uint32_t *)(uintptr_t)((uint64_t)fb_start + fb_offset);
    fb.width = width;
    fb.height = height;
    fb.pitch = pitch;
    fb.bpp = 32u;
    fb.bytes_per_pixel = 4u;
    fb.max_width = vmware_svga.max_width;
    fb.max_height = vmware_svga.max_height;
    fb.max_bytes = (uint32_t)usable_bytes;
    fb.reservation_start = fb_start;
    fb.reservation_bytes = fb_max_size;
    framebuffer_set_default_format();
    fb.available = true;
    if (restart_3d) {
        svga_mode_end(svga_flags);
        vmware_svga.fifo_min = svga.min;
        vmware_svga.fifo_max = svga.fifo_bytes;
        /* ENABLE=0 and CONFIG_DONE=0 may reset trace-based GFB updates.
         * Restore them after the extended FIFO has been configured so mmap
         * writers retain the same scan-out semantics across a mode switch. */
        framebuffer_vmware_enable_traces();
    } else {
        framebuffer_vmware_sync(false);
        svga_unlock(legacy_flags);
    }
    console_info("[reliefnt] VMware SVGA mode=%ux%u depth=%u bpp=%u pseudo=%u "
                   "pitch=%u fb=%p offset=%u size=%u max=%u\n",
                   fb.width, fb.height, depth, bpp, pseudocolor,
                   fb.pitch, (void *)(uintptr_t)fb_start, fb_offset, fb_size,
                   fb_max_size);
    return 1;
}
