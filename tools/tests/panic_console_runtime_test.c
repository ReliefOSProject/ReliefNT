/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/framebuffer.h"
#include "modules/console.h"
#include "../../lib/bugcheck.c"

#define WIDTH 640u
#define HEIGHT 640u
#define GUARD 32u
static unsigned checks;
static bool panic_phase;
static unsigned allocations, locking, io_writes, vga_bytes;
static char serial_output[32768];
static size_t serial_used;
struct svga_device svga;
#define CHECK(c) do {++checks; if (!(c)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);exit(1);}} while(0)

uint64_t svga_lock(void) { CHECK(!panic_phase); ++locking; return 0; }
bool svga_try_lock_irqsave(uint64_t *flags) { if(flags)*flags=0; return false; }
void svga_unlock(uint64_t flags) {(void)flags;}
int svga_update_locked(uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{(void)x;(void)y;(void)w;(void)h;CHECK(!panic_phase);return 0;}
int svga_update_try_locked(uint32_t x,uint32_t y,uint32_t w,uint32_t h)
{(void)x;(void)y;(void)w;(void)h;return 0;}
bool svga_restore_traces_locked(void) {CHECK(!panic_phase);return false;}
void x86_64_outl(uint32_t value,uint16_t port) {(void)value;(void)port;++io_writes;}
uint32_t x86_64_inl(uint16_t port) {(void)port;return 0;}
void x86_64_outw(uint16_t value,uint16_t port) {(void)value;(void)port;}
uint16_t x86_64_inw(uint16_t port) {(void)port;return 0;}
void *kernel_malloc(size_t bytes) {CHECK(!panic_phase);++allocations;return malloc(bytes);}
void kernel_free(void *pointer) {CHECK(!panic_phase);free(pointer);}
void serial_write(const char *text) {(void)text;CHECK(!panic_phase);}
void serial_write_emergency(const char *text,size_t length)
{CHECK(length<sizeof(serial_output)-serial_used);memcpy(serial_output+serial_used,text,length);serial_used+=length;serial_output[serial_used]=0;}
void vga_putc(char byte) {(void)byte;}
void vga_panic_begin(void) {}
void vga_panic_putc(char byte,uint8_t foreground) {(void)byte;CHECK(foreground<16);++vga_bytes;}
uint64_t time_uptime_us(void) {return 0;}
uint64_t kernel_irq_save(void) {return 0;}
void kernel_irq_restore(uint64_t flags) {(void)flags;}
uint32_t smp_current_cpu(void) {return 0;}
uint32_t smp_cpu_count(void) {return 2;}
void smp_membarrier_poll(void) {}

/** @brief Check pitch/guards, actual glyphs, VT ownership and fatal diagnostics. */
static void scenario(uint8_t bytes,bool graphical)
{
    uint32_t pitch=WIDTH*bytes+13;
    size_t length=(size_t)pitch*HEIGHT;
    uint8_t *memory=malloc(length+2*GUARD);
    CHECK(memory!=NULL);memset(memory,0xa5,length+2*GUARD);
    fb=(struct framebuffer){.pixels=(uint32_t *)(void *)(memory+GUARD),
        .width=WIDTH,.height=HEIGHT,.pitch=pitch,.bpp=bytes*8,.bytes_per_pixel=bytes,
        .type=MULTIBOOT2_FRAMEBUFFER_TYPE_RGB,.red_field_position=16,.red_mask_size=8,
        .green_field_position=8,.green_mask_size=8,.blue_field_position=0,.blue_mask_size=8,.available=true};
    panic_phase=false;serial_used=0;serial_output[0]=0;
    console_boot_logo_active=false;console_active_vt=0;console_vt_graphical=false;
    memset(vt_text,0,sizeof(vt_text));log_len=log_start=0;
    fb_console_render_target=NULL;fb_console_enabled=false;
    console_graphic_saved=NULL;console_graphic_saved_bytes=0;
    console_enable_framebuffer();console_vt_activate(2,false);
    console_vt_write(2,"TTY2 preserved prompt\n",22);
    uint8_t prefix[WIDTH*4];memcpy(prefix,memory+GUARD,WIDTH*bytes);
    size_t tty2_before=vt_text[1].length;
    if(graphical) {
        console_vt_activate(1,true);
        framebuffer_rect(0,0,WIDTH,HEIGHT,0x00336699);
        framebuffer_drm_scanout_owned=true;
    }
    unsigned old_alloc=allocations,old_locks=locking;
    panic_phase=true;
    struct console_panic_context context=console_panic_begin();
    CHECK(context.vt==(graphical?1u:2u));
    CHECK(context.graphical==graphical);
    CHECK(console_active_vt==(graphical?1u:2u));
    CHECK(!console_vt_graphical);
    struct bugcheck_info info={.reason="Unhandled Page Fault",.detail="panic renderer fixture",
        .has_exception=true,.have_registers=true,.have_runtime=true,
        .cpu=1,.apic=3,.pid=42,.ticks=1234,.uptime_ms=12340,.cr2=0xdeadbeef,.cr3=0x12345000,
        .display=context,.frame={.vector=14,.error=2,.rip=0x8022334,.cs=8,.rflags=0x202,
        .rsp=0x8800000,.ss=16,.rax=0x1111,.rbx=0x2222,.rcx=0x3333,.rbp=0x8899000}};
    memcpy(info.task_name,"fixture-task",13);
    render_bugcheck_text(&info);
    console_panic_flush();
    CHECK(allocations==old_alloc);CHECK(locking==old_locks);
    CHECK(strstr(serial_output,"KERNEL PANIC")!=NULL);
    CHECK(strstr(serial_output,"Unhandled Page Fault")!=NULL);
    CHECK(strstr(serial_output,"0x0000000008022334")!=NULL);
    CHECK(strstr(serial_output,"0x00000000DEADBEEF")!=NULL);
    CHECK(strstr(serial_output,"0x0000000000001111")!=NULL);
    CHECK(strstr(serial_output,"P=0 W=1 U=0")!=NULL);
    CHECK(strstr(serial_output,"fixture-task")!=NULL);
    CHECK(!framebuffer_drm_scanout_owned);
    const char *history=vt_text[graphical?0:1].bytes;
    CHECK(strstr(history,"KERNEL PANIC")!=NULL);
    if(graphical) CHECK(vt_text[1].length==tty2_before);
    else CHECK(memcmp(prefix,memory+GUARD,WIDTH*bytes)==0);
    if(graphical) {
        unsigned red=0;
        for(unsigned y=0;y<16;++y)for(unsigned x=0;x<WIDTH;++x) {
            uint8_t *p=memory+GUARD+(size_t)y*pitch+x*bytes;
            if(p[2]>200 && p[1]<150 && p[0]<150)++red;
        }
        CHECK(red>100); /* headline starts on the first screen row */
        for(unsigned y=HEIGHT-16;y<HEIGHT;++y)for(unsigned x=0;x<WIDTH*bytes;++x)
            CHECK(memory[GUARD+(size_t)y*pitch+x]==(x%bytes==0?0x99:x%bytes==1?0x66:x%bytes==2?0x33:0)); /* retain GUI below text */
    }
    for(unsigned y=0;y<HEIGHT;++y)for(unsigned x=WIDTH*bytes;x<pitch;++x)
        CHECK(memory[GUARD+(size_t)y*pitch+x]==0xa5);
    for(unsigned i=0;i<GUARD;++i) {CHECK(memory[i]==0xa5);CHECK(memory[GUARD+length+i]==0xa5);}
    panic_phase=false;free(memory);
    fb=(struct framebuffer){0};
}

/** @brief Exercise real emergency FIFO success, backpressure, and corrupt indices. */
static void fifo_scenario(void)
{
    uint32_t ring[256]={0};
    fb=(struct framebuffer){.available=true,.width=800,.height=600,
                           .backend=FRAMEBUFFER_BACKEND_VMWARE_SVGA};
    vmware_svga=(struct vmware_svga_state){.present=true,.fifo_present=true,
        .io_port=0x1000,.fifo=ring,.fifo_min=64,.fifo_max=sizeof(ring)};
    ring[VMWARE_SVGA_FIFO_NEXT_CMD]=ring[VMWARE_SVGA_FIFO_STOP]=64;
    panic_phase=true;
    framebuffer_panic_prepare();framebuffer_panic_present();
    CHECK(ring[VMWARE_SVGA_FIFO_NEXT_CMD]==84);
    CHECK(ring[16]==VMWARE_SVGA_CMD_UPDATE && ring[19]==800 && ring[20]==600);
    ring[VMWARE_SVGA_FIFO_NEXT_CMD]=64;ring[VMWARE_SVGA_FIFO_STOP]=68;
    unsigned before=io_writes;
    framebuffer_panic_present();
    CHECK(io_writes>before && io_writes-before<30000); /* finite stuck-FIFO path */
    CHECK(ring[VMWARE_SVGA_FIFO_NEXT_CMD]==64);
    ring[VMWARE_SVGA_FIFO_NEXT_CMD]=sizeof(ring)+4;
    framebuffer_panic_present();
    CHECK(!vmware_svga.fifo_present);
    panic_phase=false;
    vmware_svga=(struct vmware_svga_state){0};fb=(struct framebuffer){0};
}

/** @brief A GOP mapping rejected at boot must stay disabled during a panic. */
static void disabled_framebuffer_scenario(void)
{
    fb=(struct framebuffer){.available=true,.pixels=(uint32_t *)(uintptr_t)0x1000000000ULL,
        .width=800,.height=600,.pitch=3200,.bytes_per_pixel=4,
        .backend=FRAMEBUFFER_BACKEND_VMWARE_SVGA};
    vmware_svga=(struct vmware_svga_state){.present=true,.fifo_present=true,
        .fifo=(volatile uint32_t *)(uintptr_t)0x1000000000ULL};
    fb_console_enabled=false;
    fb_console_tty_cursor_visible=true; /* no cursor restore through bad pixels */
    fb_console_tty_cursor_x=fb_console_tty_cursor_y=0;
    fb_console_tty_cursor_bpp=4;
    unsigned before=vga_bytes;
    unsigned io_before=io_writes;
    serial_used=0;serial_output[0]=0;
    panic_phase=true;
    (void)console_panic_begin();
    console_panic_write("KERNEL PANIC\n",13,PANIC_RED);
    console_panic_flush();
    CHECK(!fb_console_enabled);
    CHECK(vga_bytes>before);
    CHECK(io_writes==io_before);
    CHECK(strstr(serial_output,"KERNEL PANIC")!=NULL);
    panic_phase=false;fb=(struct framebuffer){0};vmware_svga=(struct vmware_svga_state){0};
}

/** @brief Explicit panic and emergency frames must label unavailable data honestly. */
static void report_scenarios(void)
{
    panic_phase=true;serial_used=0;serial_output[0]=0;
    struct bugcheck_info info={.reason="Explicit kernel panic",
        .detail="literal \033[47m control",.frame={.rip=0x8012345,.cs=8}};
    render_bugcheck_text(&info);
    CHECK(strstr(serial_output,"explicit panic (RIP = caller)")!=NULL);
    CHECK(strstr(serial_output,"VECTOR=")==NULL);
    CHECK(strstr(serial_output,"literal ?[47m control")!=NULL);
    CHECK(strstr(serial_output,"\033[47m")==NULL);
    CHECK(strstr(serial_output,"RSP: not saved")!=NULL);
    CHECK(strstr(serial_output,"General registers: not saved")!=NULL);
    serial_used=0;serial_output[0]=0;
    info.reason="Double Fault";info.has_exception=true;info.frame.vector=8;
    render_bugcheck_text(&info);
    CHECK(strstr(serial_output,"VECTOR=8")!=NULL);
    CHECK(strstr(serial_output,"Task/time: unavailable")!=NULL);
    CHECK(strstr(serial_output,"PF:")==NULL);
    panic_phase=false;
}

/** @brief Only literal glyph cells may replace GUI pixels, even at screen edges. */
static void overlay_cells(uint8_t bytes,bool soft_wrap)
{
    const unsigned width=43,height=35,pitch=width*bytes+11;
    const size_t size=(size_t)pitch*height;
    uint8_t *memory=malloc(size+2*GUARD),*before=malloc(size);
    CHECK(memory && before);
    memset(memory,0xa5,size+2*GUARD);
    fb=(struct framebuffer){.pixels=(uint32_t *)(void *)(memory+GUARD),
        .width=width,.height=height,.pitch=pitch,.bpp=bytes*8,.bytes_per_pixel=bytes,
        .type=MULTIBOOT2_FRAMEBUFFER_TYPE_RGB,.red_field_position=16,.red_mask_size=8,
        .green_field_position=8,.green_mask_size=8,.blue_field_position=0,.blue_mask_size=8,.available=true};
    framebuffer_rect(0,0,width,height,0x00336699);
    memcpy(before,memory+GUARD,size);
    console_active_vt=1;console_vt_graphical=true;fb_console_enabled=true;
    fb_console_render_target=NULL;console_boot_logo_active=false;
    serial_used=0;serial_output[0]=0;
    panic_phase=true;
    (void)console_panic_begin();
    CHECK(memcmp(before,memory+GUARD,size)==0); /* takeover itself paints nothing */
    const char *text=soft_wrap?"1234567890":"A B\nC";
    console_panic_write(text,strlen(text),PANIC_WHITE);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x) {
        bool painted=soft_wrap?(x<40 && y<32):((y<16 && x<24)||(y>=16 && y<32 && x<8));
        uint8_t *pixel=memory+GUARD+(size_t)y*pitch+x*bytes;
        if(!painted) CHECK(memcmp(pixel,before+(size_t)y*pitch+x*bytes,bytes)==0);
        else {
            CHECK((pixel[0]==0 && pixel[1]==0 && pixel[2]==0)||
                  (pixel[0]==255 && pixel[1]==255 && pixel[2]==255));
            if(!soft_wrap && y<16 && x>=8 && x<16)
                CHECK(pixel[0]==0 && pixel[1]==0 && pixel[2]==0); /* space has black background */
        }
    }
    memcpy(before,memory+GUARD,size);
    console_panic_write(soft_wrap?"Z":"\nTRUNCATED\n",soft_wrap?1:11,PANIC_RED);
    CHECK(memcmp(before,memory+GUARD,size)==0); /* no scroll/copy outside glyphs */
    CHECK(strstr(serial_output,soft_wrap?"Z":"TRUNCATED")!=NULL); /* full serial report continues */
    console_active_vt=2;console_vt_graphical=false;
    (void)console_panic_begin(); /* text VT still scrolls after a clipped GUI case */
    for(unsigned y=0;y<16;++y)
        CHECK(memcmp(memory+GUARD+(size_t)y*pitch,before+(size_t)(y+16)*pitch,40*bytes)==0);
    for(unsigned y=16;y<32;++y)for(unsigned x=0;x<40*bytes;++x)
        CHECK(memory[GUARD+(size_t)y*pitch+x]==0);
    for(unsigned y=0;y<height;++y)for(unsigned x=width*bytes;x<pitch;++x)
        CHECK(memory[GUARD+(size_t)y*pitch+x]==0xa5);
    for(unsigned i=0;i<GUARD;++i) {CHECK(memory[i]==0xa5);CHECK(memory[GUARD+size+i]==0xa5);}
    panic_phase=false;free(before);free(memory);fb=(struct framebuffer){0};
}

int main(void)
{
    scenario(4,false);scenario(4,true);scenario(3,false);scenario(3,true);
    fifo_scenario();
    disabled_framebuffer_scenario();
    report_scenarios();
    overlay_cells(4,false);overlay_cells(3,false);
    overlay_cells(4,true);overlay_cells(3,true);
    printf("PASS panic console: %u checks; current tty2, GUI glyph-cell black overlay, untouched desktop, clipping, real 24/32-bit glyphs, detailed registers, no heap or display locks\n",checks);
    return 0;
}
