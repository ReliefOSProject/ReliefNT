#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/vga_text.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/pci.h>

static struct framebuffer fb={.available=true,.backend=FRAMEBUFFER_BACKEND_BOCHS_VBE,.width=1024,.height=768};
static uint16_t memory[2002],vbe[16]={0xb0c5,1024,768,32,0x41,0,1024,768,0,0};
static uint8_t font[8192],seq[256],crtc[256],gr[256],ac[32];
static unsigned si,ci,gi,ai,vi,svi,writes,begin_calls,end_calls;
static uint32_t svga_regs[16]={[1]=1};
static bool flip, busy, fail_mode;
static uint16_t log_cells[2000];
const struct framebuffer *framebuffer_get(void) { return &fb; }
void *mm_phys_to_virt(uint64_t phys) { return phys==0xa0000 ? (void*)font : phys==0xb8000 ? (void*)(memory+1) : NULL; }
bool paging_mmio_uncached(uint64_t p,uint64_t n) { assert(p==0xa0000 && n==0x20000);return true; }
int pci_find_device(uint16_t v,uint16_t d,struct pci_device *out)
{ (void)v;(void)d; *out=(struct pci_device){.class_code=3}; return 0; }
uint32_t pci_config_read32(uint8_t b,uint8_t s,uint8_t f,uint8_t o)
{ (void)b;(void)s;(void)f; assert(o==0x10); return 0x1001; }
uint64_t svga_lock(void) { return 0; }
void svga_unlock(uint64_t f) { (void)f; }
int svga_mode_begin(uint64_t *f) { *f=0; ++begin_calls; return busy?-16:0; }
void svga_mode_end(uint64_t f) { (void)f;++end_calls; }
int framebuffer_vmware_set_mode(uint32_t w,uint32_t h)
{
    assert(w==fb.width && h==fb.height);if(fail_mode)return 0;svga_regs[1]=1;
    /* The real setter logs before KD_GRAPHICS commits. Logging must not
     * reclaim VGA or touch its aperture halfway through restoring graphics. */
    unsigned calls=begin_calls;
    uint16_t before=memory[1];
    vga_text_hw_present(log_cells,0,0,true,0,false);
    assert(begin_calls==calls && svga_regs[1]==1 && memory[1]==before);
    return 1;
}
int framebuffer_vbe_set_mode(uint32_t w,uint32_t h)
{ assert(w==fb.width && h==fb.height);if(fail_mode)return 0;vbe[1]=w;vbe[2]=h;vbe[3]=32;vbe[4]=0x41;return 1; }
void x86_64_outb(uint8_t v,uint16_t p)
{
    ++writes;
    switch(p) {
    case 0x3c4: si=v;break; case 0x3c5: seq[si]=v;break;
    case 0x3d4: ci=v;break; case 0x3d5: crtc[ci]=v;break;
    case 0x3ce: gi=v;break; case 0x3cf:
        if(gi==6 && !v && seq[4]==6)
            assert(!gr[0] && !gr[1] && !gr[3] && gr[8]==0xff);
        gr[gi]=v;break;
    case 0x3c0: if(!flip)ai=v&31;else ac[ai]=v;flip=!flip;break;
    default:break;
    }
}
uint8_t x86_64_inb(uint16_t p)
{
    switch(p) {
    case 0x3cc:return 0x67; case 0x3c5:return seq[si];case 0x3d5:return crtc[ci];
    case 0x3cf:return gr[gi];case 0x3c1:return ac[ai];case 0x3da:flip=false;return 0;
    default:return 0;
    }
}
void x86_64_outw(uint16_t v,uint16_t p) { ++writes;if(p==0x1ce)vi=v;else {assert(p==0x1cf && vi<16);vbe[vi]=v;} }
uint16_t x86_64_inw(uint16_t p) { assert(p==0x1cf && vi<16);return vbe[vi]; }
void x86_64_outl(uint32_t v,uint16_t p)
{ assert(p==0x1000 || p==0x1001);++writes;if(p==0x1000)svi=v;else {assert(svi<16);svga_regs[svi]=v;} }
uint32_t x86_64_inl(uint16_t p) {assert(p==0x1001 && svi<16);return svga_regs[svi];}

int main(int argc,char **argv)
{
    assert(argc==2);
    if(!strcmp(argv[1],"unsupported")) {
        fb.backend=FRAMEBUFFER_BACKEND_BOOT;
        assert(vga_text_hw_init(false)==-19 && !writes);return 0;
    }
    if(!strcmp(argv[1],"svga-busy")) {
        fb.backend=FRAMEBUFFER_BACKEND_VMWARE_SVGA;busy=true;
        assert(vga_text_hw_init(true)==-16 && begin_calls==1 && !end_calls);
        assert(vbe[4]==0x41 && svga_regs[1]==1 && !seq[2]);return 0;
    }
    if(!strcmp(argv[1],"svga")) fb.backend=FRAMEBUFFER_BACKEND_VMWARE_SVGA;
    /* Firmware is not required to leave the font-write raster state sane. */
    gr[0]=15;gr[1]=15;gr[3]=0x18;gr[8]=0;
    assert(vga_text_hw_init(true)==0);
    /* SVGA can be layered over an EFI-programmed Bochs VBE scanout. Both
     * extended enables must be off before legacy text registers take effect. */
    assert(vbe[4]==0);
    assert(seq[2]==3 && gr[6]==0x0e && crtc[9]==0x4f && !(ac[16]&8));
    uint16_t cells[2000];for(unsigned i=0;i<2000;++i)cells[i]=0x1e41;
    memory[0]=memory[2001]=0xa55a;
    vga_text_hw_present(cells,79,24,true,1,false);
    assert(memory[1]==0x1e41 && memory[2000]==0x1e41 && memory[0]==0xa55a && memory[2001]==0xa55a);
    assert(crtc[14]==7 && crtc[15]==207 && crtc[10]==0);
    vga_text_hw_present(cells,0,0,false,0,true);
    assert(memory[1]==0xe141 && crtc[10]==0x20);
    assert(vga_text_hw_leave()==0);
    unsigned previous=writes;
    vga_text_hw_present(cells,999,999,true,0,false);
    assert(writes==previous);
    if(fb.backend==FRAMEBUFFER_BACKEND_BOCHS_VBE) assert(vbe[1]==1024 && vbe[2]==768 && vbe[3]==32 && (vbe[4]&0x41)==0x41);
    else assert(begin_calls==1 && end_calls==1);
    assert(vga_text_hw_enter()==0);
    {
        /* Reproduce the real startup order: fbctl sets a new pixel mode while
         * the VT still owns text, then Xorg requests KD_GRAPHICS. */
        fb.width=1280;fb.height=800;
        vbe[1]=1280;vbe[2]=800;vbe[3]=32;vbe[4]=0x41;
        vga_text_hw_present(cells,0,0,true,0,false);
        assert(vbe[4]==0);
        assert(vga_text_hw_leave()==0);
        if(fb.backend==FRAMEBUFFER_BACKEND_BOCHS_VBE)
            assert(vbe[1]==1280 && vbe[2]==800 && vbe[3]==32);
    }
    assert(vga_text_hw_enter()==0);
    fail_mode=true;
    assert(vga_text_hw_leave()==-5 && vbe[4]==0);
    fail_mode=false;
    assert(vga_text_hw_leave()==0);
    /* Emergency mode must also turn off both extended scanouts. */
    vbe[4]=0x41;
    vga_text_hw_panic();
    assert(vbe[4]==0 && seq[2]==3);
    if(fb.backend==FRAMEBUFFER_BACKEND_VMWARE_SVGA) assert(!svga_regs[1]);
    puts("PASS VGA hardware: text register/font setup, 16-color attributes, aperture bounds, cursor and graphic-mode restore");
}
