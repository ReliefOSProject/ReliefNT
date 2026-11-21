/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_SVGA_INTERNAL_H
#define RELIEFNT_SVGA_INTERNAL_H

#include "../drm_internal.h"
#include "fifo.h"

#define SVGA_VRAM_MAPS 8

struct svga_vram_map {
    uint8_t *address;
    uint64_t start, end; /* byte offsets into BAR1, page-aligned */
};

/* Task callbacks run with DRM execution ownership. The ISR only acknowledges
 * the dedicated IRQ status port and schedules the graphics service. */
struct svga_device {
    struct pci_device pci;
    struct svga_fifo fifo;
    volatile uint32_t *fifo_map;
    struct svga_vram_map vram[SVGA_VRAM_MAPS];
    unsigned vram_count;
    uint32_t vram_bytes, fifo_bytes;
    uint32_t caps, caps2, max_width, max_height;
    uint32_t boot_width, boot_height;
    uint32_t original_id, original_config;
    uint32_t irq, irq_mask, original_irq_mask;
    uint16_t port, original_command;
    bool claimed, ready, fifo_changed, dma_enabled, irq_capable, quiesced;
};

extern struct svga_device svga;
extern const struct drm_driver svga_driver;

uint32_t svga_read(uint32_t reg);
void svga_write(uint32_t reg, uint32_t value);
void svga_enable_dma(void);
int svga_device_start(struct pci_device *out);
void svga_device_stop(void);

bool svga_kms_active(void);
void svga_kms_vt_release(void);
void svga_kms_file_release(struct drm_file *file);
void svga_kms_remove_fb(struct drm_file *file, uint32_t id);
int svga_kms_ioctl(struct task *task, struct drm_file *file, uint32_t command,
                   void *data, uint32_t *created_fb);
int svga_connector_info(uint32_t index, struct graphics_connector_info *out);

#endif
