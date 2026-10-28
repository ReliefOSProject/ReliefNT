/* Opt-in hardware VGA text scanout. Existing framebuffer mode/render code is
 * unchanged; graphics restoration delegates to its existing backend setters. */
#include <reliefnt/vga_text.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/mm.h>
#include <reliefnt/paging.h>
#include <reliefnt/pci.h>
#include <reliefnt/port.h>
#include <reliefos/psf_font.h>

/* Core-owned mode transaction APIs, shared with the framebuffer mode setter.
 * Successful begin holds the device lock; end reconfigures FIFO and unlocks. */
int svga_mode_begin(uint64_t *flags);
void svga_mode_end(uint64_t flags);
uint64_t svga_lock(void);
void svga_unlock(uint64_t flags);
int framebuffer_vmware_set_mode(uint32_t width, uint32_t height);
int framebuffer_vbe_set_mode(uint32_t width, uint32_t height);

static volatile uint16_t *text_memory;
static volatile uint8_t *font_memory;
static bool ready, text_active, svga_suspended, have_dispi, transitioning;
static uint32_t backend;
static uint16_t svga_port;
static uint8_t saved_misc, saved_seq[5], saved_crtc[25], saved_gr[9], saved_ac[21], saved_dac[768];

/** @brief Read one indexed byte register while the caller owns device I/O. */
static uint8_t reg_read(uint16_t port, uint8_t index)
{ x86_64_outb(index,port); return x86_64_inb(port+1); }
/** @brief Write one indexed byte register while the caller owns device I/O. */
static void reg_write(uint16_t port, uint8_t index, uint8_t value)
{ x86_64_outb(index,port); x86_64_outb(value,port+1); }
/** @brief Reset the attribute flip-flop before selecting its index. */
static void ac_write(uint8_t index, uint8_t value)
{ (void)x86_64_inb(0x3da); x86_64_outb(index,0x3c0); x86_64_outb(value,0x3c0); }
/** @brief Restore video-enable after attribute-register accesses. */
static void unblank(void)
{ (void)x86_64_inb(0x3da); x86_64_outb(0x20,0x3c0); }
/** @brief Read a Bochs VBE register without touching VGA bank state. */
static uint16_t dispi_read(unsigned index)
{ x86_64_outw((uint16_t)index,0x1ce); return x86_64_inw(0x1cf); }
/** @brief Write a Bochs VBE register in the serialized transition path. */
static void dispi_write(unsigned index, uint16_t value)
{ x86_64_outw((uint16_t)index,0x1ce); x86_64_outw(value,0x1cf); }
/** @brief Access the bound SVGA device's indexed mode register. */
static void svga_write(unsigned index, unsigned value)
{ x86_64_outl(index,svga_port); x86_64_outl(value,svga_port+1); }
/** @brief Read the current extended scanout enable state for a foreign mode set. */
static unsigned svga_read(unsigned index)
{ x86_64_outl(index,svga_port); return x86_64_inl(svga_port+1); }

/** @brief Capture VGA registers/DAC before a destructive legacy mode switch. */
static void save_vga(void)
{
    saved_misc = x86_64_inb(0x3cc);
    for (unsigned i=0;i<5;++i) saved_seq[i]=reg_read(0x3c4,i);
    for (unsigned i=0;i<25;++i) saved_crtc[i]=reg_read(0x3d4,i);
    for (unsigned i=0;i<9;++i) saved_gr[i]=reg_read(0x3ce,i);
    for (unsigned i=0;i<21;++i) {
        (void)x86_64_inb(0x3da); x86_64_outb(i,0x3c0); saved_ac[i]=x86_64_inb(0x3c1);
    }
    x86_64_outb(0,0x3c7);
    for (unsigned i=0;i<768;++i) saved_dac[i]=x86_64_inb(0x3c9);
    unblank();
}

/** @brief Program VGA mode 3 timing, font planes and a fixed 16-color palette. */
static void program_text(void)
{
    static const uint8_t seq[5]={3,0,3,0,2};
    static const uint8_t crtc[25]={
        0x5f,0x4f,0x50,0x82,0x55,0x81,0xbf,0x1f,0,0x4f,0x0d,0x0e,0,0,0,0x50,
        0x9c,0x0e,0x8f,0x28,0x1f,0x96,0xb9,0xa3,0xff};
    static const uint8_t gr[9]={0,0,0,0,0,0x10,0x0e,0,0xff};
    x86_64_outb(0x67,0x3c2);
    for (unsigned i=0;i<5;++i) reg_write(0x3c4,i,seq[i]);
    reg_write(0x3d4,0x11,reg_read(0x3d4,0x11)&0x7f);
    for (unsigned i=0;i<25;++i) reg_write(0x3d4,i,crtc[i]);
    /* Font stores use the VGA raster pipeline too. EFI need not leave its
     * write mode, set/reset, rotation or bit mask in a useful text state. */
    for (unsigned i=0;i<9;++i) reg_write(0x3ce,i,gr[i]);
    /* Plane 2 is the glyph RAM: 32 bytes per glyph, 16 visible scanlines. */
    reg_write(0x3c4,2,4); reg_write(0x3c4,4,6);
    reg_write(0x3ce,4,2); reg_write(0x3ce,5,0); reg_write(0x3ce,6,0);
    for (unsigned glyph=0;glyph<256;++glyph)
        for (unsigned line=0;line<32;++line)
            font_memory[glyph*32+line]=line<16 ? reliefos_lat15_vga16_psf[4+glyph*16+line] : 0;
    for (unsigned i=0;i<5;++i) reg_write(0x3c4,i,seq[i]);
    for (unsigned i=0;i<9;++i) reg_write(0x3ce,i,gr[i]);
    for (unsigned i=0;i<16;++i) ac_write(i,i);
    ac_write(16,4); ac_write(17,0); ac_write(18,15); ac_write(19,8); ac_write(20,0);
    /* Six-bit DAC intensity: bit 3 brightens each component, no blink bit. */
    x86_64_outb(0xff,0x3c6); x86_64_outb(0,0x3c8);
    for (unsigned i=0;i<16;++i) {
        unsigned bright=(i&8)?21:0;
        unsigned r=((i&4)?42:0)+bright, g=((i&2)?42:0)+bright, b=((i&1)?42:0)+bright;
        if (i==6) g=21;
        x86_64_outb(r,0x3c9); x86_64_outb(g,0x3c9); x86_64_outb(b,0x3c9);
    }
    unblank();
}

/** @brief Restore legacy registers, leaving extended graphics enable until last. */
static void restore_vga(void)
{
    x86_64_outb(saved_misc,0x3c2);
    for(unsigned i=0;i<5;++i) reg_write(0x3c4,i,saved_seq[i]);
    reg_write(0x3d4,0x11,reg_read(0x3d4,0x11)&0x7f);
    for(unsigned i=0;i<25;++i) reg_write(0x3d4,i,saved_crtc[i]);
    for(unsigned i=0;i<9;++i) reg_write(0x3ce,i,saved_gr[i]);
    for(unsigned i=0;i<21;++i) ac_write(i,saved_ac[i]);
    x86_64_outb(0,0x3c8);
    for(unsigned i=0;i<768;++i) x86_64_outb(saved_dac[i],0x3c9);
    unblank();
}

/** @brief Enter text mode only after the active graphics provider is quiescent. */
int vga_text_hw_enter(void)
{
    if (!ready) return -19;
    if (text_active) return 0;
    if (backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
        uint64_t flags;
        int ret=svga_mode_begin(&flags);
        if (ret) return ret;
        svga_suspended=true;
        save_vga(); svga_write(1,0); svga_unlock(flags);
    } else save_vga();
    /* EFI may leave the underlying Bochs scanout enabled even when the
     * selected provider is SVGA. Either extended enable overrides VGA mode 3. */
    if (have_dispi) dispi_write(4,0);
    program_text(); text_active=true;
    return 0;
}

/** @brief Return to the saved pixel mode before the VT exposes GUI scanout. */
int vga_text_hw_leave(void)
{
    if (!ready) return -19;
    if (!text_active) return 0;
    const struct framebuffer *fb=framebuffer_get();
    /* Existing mode setters can emit console logs before ownership commits.
     * Do not mistake their in-flight enable for a foreign framebuffer probe. */
    transitioning=true;
    restore_vga();
    if (backend==FRAMEBUFFER_BACKEND_BOCHS_VBE) {
        /* A privileged fbctl/Xorg probe can select a new mode before
         * KD_GRAPHICS. Restore the authoritative descriptor, not stale boot
         * bpp/pitch. Shared VT code restores saved GUI pixels after this call. */
        if (!framebuffer_vbe_set_mode(fb->width,fb->height)) {
            dispi_write(4,0); program_text(); transitioning=false; return -5;
        }
    } else if (backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
        /* Reuse the existing checked FIFO/mode transaction, including trace
         * restoration. No framebuffer TTY renderer is enabled by this call. */
        if (!framebuffer_vmware_set_mode(fb->width,fb->height)) {
            svga_write(1,0);
            if (have_dispi) dispi_write(4,0);
            program_text(); transitioning=false; return -5;
        }
        if (svga_suspended) svga_mode_end(svga_lock());
        svga_suspended=false;
    } else { program_text(); transitioning=false; return -19; }
    text_active=false;
    transitioning=false;
    return 0;
}

/** @brief Bind only known VGA-compatible display devices or a legacy text handoff. */
int vga_text_hw_init(bool legacy_boot)
{
    const struct framebuffer *fb=framebuffer_get();
    struct pci_device dev;
    backend=fb->backend;
    if (fb->available) {
        uint16_t vendor=backend==FRAMEBUFFER_BACKEND_BOCHS_VBE?0x1234:0x15ad;
        uint16_t device=backend==FRAMEBUFFER_BACKEND_BOCHS_VBE?0x1111:0x0405;
        if ((backend!=FRAMEBUFFER_BACKEND_BOCHS_VBE && backend!=FRAMEBUFFER_BACKEND_VMWARE_SVGA) ||
            pci_find_device(vendor,device,&dev)<0 || dev.class_code!=3 || dev.subclass!=0) return -19;
        if (backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA) {
            uint32_t bar=pci_config_read32(dev.bus,dev.slot,dev.function,0x10);
            if (!(bar&1) || !(bar&0xfffc) || (bar&0xfffc)>0xfffe) return -19;
            svga_port=(uint16_t)(bar&0xfffc);
        }
    } else if (!legacy_boot) return -19;
    uint16_t dispi_id=dispi_read(0);
    have_dispi=dispi_id>=0xb0c0 && dispi_id<=0xb0c5;
    if (backend==FRAMEBUFFER_BACKEND_BOCHS_VBE && !have_dispi) return -19;
    if (!paging_mmio_uncached(0xa0000,0x20000)) return -5;
    font_memory=mm_phys_to_virt(0xa0000); text_memory=mm_phys_to_virt(0xb8000);
    if (!font_memory || !text_memory || x86_64_inb(0x3cc)==0xff) return -19;
    ready=true;
    return vga_text_hw_enter();
}

/** @brief Publish cells without allowing text writes into a GUI-owned aperture. */
void vga_text_hw_present(const uint16_t *cells, unsigned x, unsigned y,
                         bool visible, unsigned style, bool reverse)
{
    if (!ready || !text_active || transitioning || !cells) return;
    /* Mode-setting is independent of text rendering. Reclaim text scanout
     * after a pre-KD_GRAPHICS fbdev mode set without changing fbdev policy. */
    if ((have_dispi && (dispi_read(4)&1)) ||
        (backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA && (svga_read(1)&1))) {
        text_active=false;
        if (vga_text_hw_enter()) return;
    }
    for(unsigned i=0;i<2000;++i) {
        uint16_t value=cells[i];
        if (reverse) value=(value&255)|((value&0x0f00)<<4)|((value&0xf000)>>4);
        text_memory[i]=value;
    }
    bool block=style==1 || style==2;
    reg_write(0x3d4,10,visible? (block?0:14) : 0x20);
    reg_write(0x3d4,11,15);
    unsigned position=y<25 && x<80 ? y*80+x : 0;
    reg_write(0x3d4,14,(uint8_t)(position>>8)); reg_write(0x3d4,15,(uint8_t)position);
}

/** @brief Force the emergency text scanout with no device-lock dependency. */
void vga_text_hw_panic(void)
{
    if (!ready) return;
    if (have_dispi) dispi_write(4,0);
    if (backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA) svga_write(1,0);
    program_text(); transitioning=false; text_active=true;
}
