/* SPDX-License-Identifier: Apache-2.0 */
#include "kms.h"
#include "dma.h"
#include "mksstat.h"
#include "render.h"
#include <reliefnt/console.h>
#include <reliefnt/deferred.h>
#include <reliefnt/module.h>
#include <reliefnt/paging.h>
#include <reliefnt/pci_irq.h>
#include "timeout.h"

#define PCI_VENDOR_VMWARE 0x15ad
#define PCI_DEVICE_VMWARE_SVGA2 0x0405
#define PCI_COMMAND 0x04
#define PCI_COMMAND_IO 0x01
#define PCI_COMMAND_MEMORY 0x02
#define PCI_COMMAND_MASTER 0x04
#define PCI_BAR0 0x10
#define SVGA_PORT_BYTES 8
#define SVGA_BAR_PAGE 4096u

struct svga_device svga;

static inline void port_write(uint16_t port, uint32_t value)
{
    __asm__ volatile("outl %0, %1" :: "a"(value), "Nd"(port) : "memory");
}

static inline uint32_t port_read(uint16_t port)
{
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port) : "memory");
    return value;
}

uint32_t svga_read(uint32_t reg)
{
    port_write(svga.port + SVGA_INDEX_PORT, reg);
    return port_read(svga.port + SVGA_VALUE_PORT);
}

void svga_write(uint32_t reg, uint32_t value)
{
    port_write(svga.port + SVGA_INDEX_PORT, reg);
    port_write(svga.port + SVGA_VALUE_PORT, value);
}

void svga_fifo_kick(struct svga_fifo *fifo, uint32_t reason)
{
    (void)fifo;
    svga_write(SVGA_REG_SYNC, reason);
}

bool svga_fifo_host_busy(struct svga_fifo *fifo)
{
    (void)fifo;
    return svga_read(SVGA_REG_BUSY) != 0;
}

static void set_pci_command(uint16_t command)
{
    pci_config_write16(svga.pci.bus, svga.pci.slot, svga.pci.function,
                       PCI_COMMAND, command);
}

void svga_enable_dma(void)
{
    if (!svga.dma_enabled) {
        set_pci_command(svga.original_command | PCI_COMMAND_IO |
                        PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER);
        svga.dma_enabled = true;
    }
}

/* IRQ-safe. The doorbell and indexed I/O transactions remain task-owned. */
static void interrupt(void *context)
{
    const struct svga_device *device = context;
    uint16_t port = device->port + SVGA_IRQSTATUS_PORT;
    uint32_t status = port_read(port);
    if (status)
        port_write(port, status);
    if (status & device->irq_mask)
        kernel_deferred_schedule(DEFERRED_GRAPHICS);
}

/* Called once the FIFO and submission method are known. The core graphics
 * heartbeat also polls, covering hosts without an MSI capability. */
static void enable_interrupts(void)
{
    if (!svga.irq_capable)
        return;
    uint32_t mask = 0;
    if (svga_submit_has_context())
        mask = SVGA_IRQFLAG_COMMAND_BUFFER | SVGA_IRQFLAG_ANY_FENCE | SVGA_IRQFLAG_ERROR;
    else if (svga.fifo.caps & SVGA_FIFO_CAP_FENCE)
        mask = SVGA_IRQFLAG_ANY_FENCE;
    if (!mask)
        return;
    struct reliefos_driver_pci_device device = {
        .bus = svga.pci.bus, .slot = svga.pci.slot, .function = svga.pci.function,
        .vendor_id = svga.pci.vendor_id, .device_id = svga.pci.device_id,
    };
    svga.irq_mask = mask;
    if (pci_request_irq(&device, interrupt, &svga, &svga.irq) &&
        pci_request_msix_irq(&device, interrupt, &svga, &svga.irq))
        return;
    svga_write(SVGA_REG_IRQMASK, mask);
}

/* Firmware can use the BAR for VGA scanout independently of the FIFO. Drain
 * a configured FIFO before toggling PCI decode to size the memory apertures. */
static int drain_firmware(void)
{
    uint64_t last = time_work_counter_us(), remaining = 100000;
    svga_write(SVGA_REG_SYNC, 1);
    for (unsigned poll = 0; poll < 262144; ++poll) {
        if (!svga_read(SVGA_REG_BUSY))
            return 0;
        if (svga_poll_expired(&last, &remaining))
            break;
        __asm__ volatile("pause");
    }
    return -ETIMEDOUT;
}

static uint64_t bar_length(uint32_t bytes)
{
    return ((uint64_t)bytes + SVGA_BAR_PAGE - 1) & ~(uint64_t)(SVGA_BAR_PAGE - 1);
}

static uint32_t bar_base(unsigned index)
{
    uint32_t bar = pci_config_read32(svga.pci.bus, svga.pci.slot,
                                     svga.pci.function, PCI_BAR0 + index * 4);
    /* SVGA II has separate, 32-bit BAR1 and BAR2. Bit 3 is prefetchable. */
    return (bar & 7) ? 0 : bar & ~15u;
}

static int map_vram(uint32_t base)
{
    uint64_t end = base + bar_length(svga.vram_bytes);
    if (!base || end > UINT64_C(0x100000000))
        return -ENXIO;

    /* The boot framebuffer can occupy only part of BAR1. An ioremap alias
     * over that range is forbidden. Map direct runs and holes separately. */
    for (uint64_t pos = base; pos < end;) {
        uint64_t first, last, limit = end;
        if (paging_kernel_direct_map_next(pos, end, &first, &last))
            limit = first == pos ? last : first;
        if (svga.vram_count == SVGA_VRAM_MAPS)
            return -ENOSPC;
        void *address = pci_map_device_bar(&svga.pci, 1, pos, limit - pos);
        if (!address)
            return -ENXIO;
        svga.vram[svga.vram_count++] = (struct svga_vram_map){
            .address = address, .start = pos - base, .end = limit - base,
        };
        pos = limit;
    }
    return 0;
}

static uint32_t boot_vram_end(uint32_t base)
{
    const struct framebuffer *fb = framebuffer_get();
    uint64_t start = fb->reservation_start;
    uint64_t end = start + fb->reservation_bytes;
    uint64_t limit = (uint64_t)base + svga.vram_bytes;
    if (!fb->reservation_bytes || start >= limit || end <= base)
        return 0;
    return (end < limit ? end : limit) - base;
}

static int quiesce(void *context);
static void release(void *context);

int svga_device_start(struct pci_device *out)
{
    if (svga.claimed)
        return -EBUSY;
    int ret = pci_find_device(PCI_VENDOR_VMWARE, PCI_DEVICE_VMWARE_SVGA2, &svga.pci);
    if (ret)
        return -ENODEV;
    /* Owner cleanup must retain the device claim if DMA cannot be drained.
     * Device-global tables do not take a reference on their own module. */
    ret = module_add_resource(THIS_MODULE, quiesce, release, &svga);
    if (ret)
        return ret;

    /* Probe the port aperture with decode serialized by PCI core. The probe
     * is harmless to VGA scanout and does not consume a device lease. */
    ret = pci_device_io_bar0(&svga.pci, SVGA_PORT_BYTES, &svga.port);
    if (ret)
        return ret;
    svga.original_command = pci_config_read16(svga.pci.bus, svga.pci.slot,
                                              svga.pci.function, PCI_COMMAND);
    set_pci_command((svga.original_command | PCI_COMMAND_IO | PCI_COMMAND_MEMORY) &
                    ~PCI_COMMAND_MASTER);
    const char *stage = "SVGA II version negotiation";
    svga.original_id = svga_read(SVGA_REG_ID);
    svga.claimed = true;
    svga_write(SVGA_REG_ID, SVGA_ID_2);
    if (svga_read(SVGA_REG_ID) != SVGA_ID_2) {
        ret = -ENODEV;
        goto fail;
    }

    stage = "firmware FIFO state";
    svga.original_config = svga_read(SVGA_REG_CONFIG_DONE);
    if (svga.original_config > 1) {
        console_warn("[svga] invalid CONFIG_DONE=%x", svga.original_config);
        ret = -EIO;
        goto fail;
    }
    if (svga.original_config) {
        ret = drain_firmware();
        if (ret)
            goto fail;
    }

    svga.caps = svga_read(SVGA_REG_CAPABILITIES);
    if (svga.caps & SVGA_CAP_CAP2_REGISTER)
        svga.caps2 = svga_read(SVGA_REG_CAP2);
    uint16_t irq_port;
    if ((svga.caps & SVGA_CAP_IRQMASK) &&
        !pci_device_io_bar0(&svga.pci, SVGA_IRQSTATUS_PORT + sizeof(uint32_t), &irq_port) &&
        irq_port == svga.port) {
        svga.irq_capable = true;
        svga.original_irq_mask = svga_read(SVGA_REG_IRQMASK);
        svga_write(SVGA_REG_IRQMASK, 0);
        irq_port += SVGA_IRQSTATUS_PORT;
        port_write(irq_port, port_read(irq_port));
    }
    svga.vram_bytes = svga_read(SVGA_REG_VRAM_SIZE);
    svga.fifo_bytes = svga_read(SVGA_REG_MEM_SIZE);
    svga.max_width = svga_read(SVGA_REG_MAX_WIDTH);
    svga.max_height = svga_read(SVGA_REG_MAX_HEIGHT);
    svga.boot_width = svga_read(SVGA_REG_WIDTH);
    svga.boot_height = svga_read(SVGA_REG_HEIGHT);

    stage = "VRAM, FIFO and display limits";
    /* KMS mode dimensions and legacy FIFO coordinates fit these bounds. */
    if (!svga.vram_bytes || svga.fifo_bytes < SVGA_BAR_PAGE + 8 ||
        svga.max_width < 64 || svga.max_width > SVGA_MAX_SCREEN_SIZE ||
        svga.max_height < 64 || svga.max_height > SVGA_MAX_SCREEN_SIZE) {
        console_warn("[svga] invalid limits: VRAM=%u FIFO=%u display=%ux%u",
                     svga.vram_bytes, svga.fifo_bytes, svga.max_width, svga.max_height);
        ret = -EIO;
        goto fail;
    }
    stage = "PCI aperture mapping";
    uint32_t fifo_base = bar_base(2);
    if (fifo_base)
        svga.fifo_map = pci_map_device_bar(&svga.pci, 2, fifo_base,
                                          bar_length(svga.fifo_bytes));
    if (!svga.fifo_map) {
        console_warn("[svga] BAR2 map failed: base=%x bytes=%u", fifo_base, svga.fifo_bytes);
        ret = -ENXIO;
        goto fail;
    }
    uint32_t vram_base = bar_base(1);
    ret = map_vram(vram_base);
    if (ret) {
        console_warn("[svga] BAR1 map failed: base=%x bytes=%u (%d)",
                     vram_base, svga.vram_bytes, ret);
        goto fail;
    }
    stage = "FIFO layout";
    bool extended = svga.caps & SVGA_CAP_EXTENDED_FIFO;
    uint32_t registers = extended ? svga_read(SVGA_REG_MEM_REGS) : 4;
    if (registers < 4) {
        console_warn("[svga] invalid MEM_REGS=%u", registers);
        ret = -EIO;
        goto fail;
    }
    ret = svga_fifo_init(&svga.fifo, svga.fifo_map, svga.fifo_bytes, registers,
                         extended, svga.original_config != 0);
    if (ret)
        goto fail;
    if (!svga.original_config) {
        svga_write(SVGA_REG_CONFIG_DONE, 1);
        svga.fifo_changed = true;
    }
    stage = "FIFO start";
    ret = svga_fifo_start(&svga.fifo);
    if (ret) {
        volatile uint32_t *mem = svga.fifo_map;
        console_warn("[svga] FIFO registers: config=%u enable=%x regs=%u caps=%x",
                     svga.original_config, svga_read(SVGA_REG_ENABLE), registers,
                     svga.fifo.caps);
        console_warn("[svga] FIFO geometry: min=%x max=%x next=%x stop=%x (expected %x %x %x)",
                     mem[SVGA_FIFO_MIN], mem[SVGA_FIFO_MAX], mem[SVGA_FIFO_NEXT_CMD],
                     mem[SVGA_FIFO_STOP], svga.fifo.min, svga.fifo.max, svga.fifo.next);
        goto fail;
    }
    stage = "command buffer start";
    ret = svga_submit_init();
    if (ret)
        goto fail;
    stage = "DMA object tables";
    ret = svga_dma_init(svga_render_reclaim_mobs);
    if (ret)
        goto fail;
    svga_screen_init(boot_vram_end(vram_base));
    svga_kms_init();
    stage = "KMS workers";
    ret = svga_atomic_init();
    if (!ret)
        ret = svga_vblank_init();
    if (ret)
        goto fail;
    svga_render_init();
    svga_host_init();
    svga.ready = true;
    enable_interrupts();
    static const char *const displays[] = {
        [SVGA_DISPLAY_LEGACY] = "legacy",
        [SVGA_DISPLAY_OBJECT] = "screen-object",
        [SVGA_DISPLAY_TARGET] = "screen-target",
    };
    console_info("[svga] SVGA II ready: caps=%x caps2=%x VRAM=%u FIFO=%u display=%s transport=%s irq=%s",
                 svga.caps, svga.caps2, svga.vram_bytes, svga.fifo_bytes,
                 displays[svga_screen_type()], svga_submit_has_context() ? "cb" : "fifo",
                 svga.irq ? "enabled" : "poll");
    *out = svga.pci;
    return 0;

fail:
    console_warn("[svga] %s failed (%d), caps=%x caps2=%x", stage, ret, svga.caps, svga.caps2);
    svga_device_stop();
    return ret;
}

static int quiesce(void *context)
{
    (void)context;
    if (!svga.claimed || svga.quiesced)
        return 0;
    svga.ready = false;
    if (svga.irq_capable) {
        svga_write(SVGA_REG_IRQMASK, 0);
        if (svga.irq) {
            pci_free_irq(svga.irq);
            svga.irq = 0;
        }
        uint16_t port = svga.port + SVGA_IRQSTATUS_PORT;
        port_write(port, port_read(port));
    }
    svga_kms_vt_release();
    svga_console_fini();
    svga_atomic_fini();
    svga_vblank_fini();
    svga_mksstat_fini();
    svga_host_fini();
    svga_execbuf_fini();
    int ret = svga_dma_fini();
    if (!ret)
        ret = svga_submit_fini();
    if (ret) {
        console_warn("[svga] device still owned after stop failure (%d)", ret);
        return ret;
    }
    svga.quiesced = true;
    return 0;
}

static void release(void *context)
{
    (void)context;
    if (!svga.claimed || !svga.quiesced)
        return;
    svga_kms_restore_firmware();
    if (svga.fifo_changed)
        svga_write(SVGA_REG_CONFIG_DONE, svga.original_config);
    if (svga.irq_capable)
        svga_write(SVGA_REG_IRQMASK, svga.original_irq_mask);
    svga_write(SVGA_REG_ID, svga.original_id);
    while (svga.vram_count) {
        struct svga_vram_map *map = &svga.vram[--svga.vram_count];
        pci_unmap_mmio(map->address, map->end - map->start);
    }
    if (svga.fifo_map)
        pci_unmap_mmio((void *)svga.fifo_map, bar_length(svga.fifo_bytes));
    set_pci_command(svga.original_command);
    svga = (struct svga_device){0};
}

void svga_device_stop(void)
{
    if (!quiesce(&svga))
        release(&svga);
}
