/* Reuse the real framebuffer regression fixture and then exercise the new
 * dispatch path against a hardware aperture substitute. No production I/O. */
#define VGA_CONSOLE_TEST_HW
#define main framebuffer_reference_main
#include "vt_console_test.c"
#undef main

static uint16_t aperture[2000];
static unsigned presents, inits, panic_enters, query_vt;
static bool supported=true;
static int enter_error, leave_error;
static char response[80];
int vga_text_hw_init(bool legacy) { assert(legacy); ++inits; return supported?0:-19; }
int vga_text_hw_enter(void) { return enter_error; }
int vga_text_hw_leave(void) { return leave_error; }
void vga_text_hw_present(const uint16_t *cells,unsigned x,unsigned y,bool v,unsigned s,bool r)
{ (void)v;(void)s;(void)r; assert(x<80 && y<25); memcpy(aperture,cells,sizeof(aperture)); ++presents; }
void vga_text_hw_panic(void) { ++panic_enters; }
void pty_vt_reply(uint32_t vt,const char *bytes,size_t len)
{ assert(len<sizeof(response)); query_vt=vt; memcpy(response,bytes,len); response[len]=0; }

int main(void)
{
    framebuffer_reference_main();
    assert(!console_vga_text_mode);
    assert(!vga_console_requested(NULL) && !vga_console_requested("xconsole=vga"));
    assert(!vga_console_requested("console=vga-extra"));
    assert(vga_console_requested("root=x\tconsole=vga logcolor=0"));
    assert(!vga_console_requested("console=vga console=framebuffer"));
    assert(vga_console_requested("console=framebuffer console=vga"));
    vga_console_configure("quiet",true); assert(!inits);
    supported=false;
    vga_console_configure("console=vga",true); assert(!console_vga_text_mode && inits==1);
    supported=true;
    log_len=log_start=0; console_active_vt=0; console_vt_graphical=false;
    console_service_logs_only=false; fb_console_enabled=false;
    vga_console_configure("console=vga",true); assert(console_vga_text_mode && inits==2);
    uint32_t unchanged[80*48]; memcpy(unchanged,pixels,sizeof(pixels));
    console_enable_framebuffer(); assert(!fb_console_enabled);
    console_write("BOOT\n");
    assert((aperture[0]&255)=='[' || (aperture[0]&255)=='B');
    console_vt_activate(1,false);
    console_vt_write(1,"\033[2J\033[Hone",11);
    assert((aperture[0]&255)=='o');
    char key[8];
    assert(vga_console_key(1,72,1,key)==3 && !memcmp(key,"\033[A",3));
    console_vt_write(1,"\033[?1h",5);
    assert(vga_console_key(1,72,1,key)==3 && !memcmp(key,"\033OA",3));
    assert(vga_console_key(2,72,1,key)==3 && !memcmp(key,"\033[A",3));
    assert(vga_console_key(1,72,6,key)==6 && !memcmp(key,"\033[1;6A",6));
    assert(vga_console_key(1,88,8,key)==7 && !memcmp(key,"\033[24;8~",7));
    assert(vga_console_key(1,73,1,key)==4 && !memcmp(key,"\033[5~",4));
    for(unsigned vt=2;vt<=6;++vt) {
        char text[]="\033[2J\033[HttyX"; text[10]=(char)('0'+vt);
        console_vt_write(vt,text,sizeof(text)-1);
    }
    assert((aperture[0]&255)=='o');
    for(unsigned vt=2;vt<=6;++vt) {
        assert(!console_prepare_vt(false)); console_vt_activate(vt,false);
        assert((aperture[0]&255)=='t' && (aperture[3]&255)=='0'+vt);
    }
    assert(!memcmp(unchanged,pixels,sizeof(pixels)));
    console_vt_write(6,"\033[6n",4);
    assert(query_vt==6 && !strcmp(response,"\033[1;5R"));
    unsigned previous=presents;
    assert(!console_prepare_vt(true)); console_vt_activate(1,true);
    assert(presents==previous);
    pixels[100]=0x123456; memcpy(unchanged,pixels,sizeof(pixels));
    console_vt_write(1,"hidden",6); console_write("kernel diagnostic\n");
    assert(presents==previous && !memcmp(unchanged,pixels,sizeof(pixels)));
    enter_error=-16;
    assert(console_prepare_vt(false)==-16 && console_vt_graphical);
    enter_error=0;
    assert(!console_prepare_vt(false)); console_vt_activate(2,false);
    assert((aperture[0]&255)=='t');
    leave_error=-5;
    assert(console_prepare_vt(true)==-5 && !console_vt_graphical);
    leave_error=0;
    assert(!console_prepare_vt(true)); console_vt_activate(1,true);
    assert(!memcmp(unchanged,pixels,sizeof(pixels)));
    vga_console_panic_begin(); vga_console_panic_write("PANIC\n",6,12);
    assert(panic_enters==1 && aperture[0]==0x0c50);
    puts("PASS VGA dispatch: explicit parameter, boot output, six independent VTs, hidden output, query routing, graphics restore/failure isolation and panic");
}
