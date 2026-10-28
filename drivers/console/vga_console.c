#include "internal.h"
#include "vga_console.h"
#include "vga_terminal.h"
#include <reliefnt/vga_text.h>
#include <reliefnt/pty.h>

static struct vga_terminal vga_screens[CONSOLE_VT_COUNT];
static struct vga_terminal vga_panic_screen;

/** @brief Match whole whitespace-separated option tokens, never substrings. */
bool vga_console_requested(const char *s)
{
    bool enabled=false;
    while (s && *s) {
        while (*s && (unsigned char)*s<=32) ++s;
        const char *first=s;
        while (*s && (unsigned char)*s>32) ++s;
        size_t n=(size_t)(s-first);
        if (n==11 && !__builtin_memcmp(first,"console=vga",11)) enabled=true;
        else if (n==19 && !__builtin_memcmp(first,"console=framebuffer",19)) enabled=false;
    }
    return enabled;
}

/** @brief Reset and enable only a hardware-probed, explicitly requested backend. */
void vga_console_configure(const char *cmdline, bool legacy_boot)
{
    if (!vga_console_requested(cmdline)) return;
    int ret=vga_text_hw_init(legacy_boot);
    if (ret) {
        console_warn("[console] console=vga unavailable ret=%d; keeping original backend\n",ret);
        return;
    }
    for (unsigned i=0;i<CONSOLE_VT_COUNT;++i) vga_terminal_reset(&vga_screens[i]);
    console_vga_text_mode=true;
    for (size_t i=console_log_replay_start();i<log_len;++i)
        vga_console_boot_byte(log_buffer[(log_start+i)%CONSOLE_LOG_CAP]);
    vga_console_present();
}

/** @brief Make raw kernel newlines conventional CR/LF without altering TTY output. */
void vga_console_boot_byte(char ch)
{
    if (ch=='\n') vga_terminal_feed(&vga_screens[0],'\r');
    vga_terminal_feed(&vga_screens[0],(uint8_t)ch);
    if (ch=='\n') vga_console_present();
}

/** @brief Send query replies directly to the addressed VT's input, without echo. */
void vga_console_write(uint32_t number, const char *s, size_t len)
{
    if (number<1 || number>CONSOLE_VT_COUNT || !s) return;
    struct vga_terminal *t=&vga_screens[number-1];
    for (size_t i=0;i<len;++i) {
        vga_terminal_feed(t,(uint8_t)s[i]);
        if (t->reply_length) {
            pty_vt_reply(number,t->reply,t->reply_length);
            t->reply_length=0;
        }
    }
    if (number==console_active_vt) vga_console_present();
}

/** @brief Never touch text memory or cursor registers while graphics owns scanout. */
void vga_console_present(void)
{
    if (console_vt_graphical) return;
    unsigned index=console_active_vt ? console_active_vt-1 : 0;
    if (index>=CONSOLE_VT_COUNT) return;
    struct vga_terminal *t=&vga_screens[index];
    const struct vga_page *p=&t->page[t->alternate];
    vga_text_hw_present(p->cells,p->cursor.x,p->cursor.y,t->visible,t->cursor_style,t->reverse_screen);
}

/** @brief Present the new text owner; graphics frames are restored by shared VT code. */
void vga_console_activate(void) { vga_console_present(); }

/** @brief Perform a checked hardware transaction before committing a VT change. */
int vga_console_switch(bool graphical)
{ return graphical ? vga_text_hw_leave() : vga_text_hw_enter(); }

/** @brief Start a literal, black-background emergency screen with static storage. */
void vga_console_panic_begin(void)
{
    vga_text_hw_panic();
    vga_terminal_reset(&vga_panic_screen);
    vga_panic_screen.page[0].cursor.fg=15;
}

/** @brief Bypass parser state left by a failing user task and publish panic bytes. */
void vga_console_panic_write(const char *s, size_t len, uint8_t color)
{
    struct vga_page *p=&vga_panic_screen.page[0];
    p->cursor.fg=color&15;
    for (size_t i=0;i<len;++i) {
        uint8_t b=(uint8_t)s[i];
        if (b=='\n') vga_terminal_index(p,false,true);
        else if (b=='\r') p->cursor.x=0;
        else if (b>=32 && b<127) vga_terminal_scalar(&vga_panic_screen,b);
    }
    vga_text_hw_present(p->cells,p->cursor.x,p->cursor.y,false,0,false);
}

/** @brief Encode common full-screen-tool keys; return zero for ordinary typing. */
unsigned vga_console_key(uint32_t vt, uint8_t key, unsigned mod, char *out)
{
    if (vt<1 || vt>6 || !out || mod<1 || mod>8) return 0;
    char final=key==72?'A':key==80?'B':key==77?'C':key==75?'D':key==71?'H':key==79?'F':0;
    unsigned code=key==82?2:key==83?3:key==73?5:key==81?6:0;
    if (key>=59 && key<=62) final=(char)('P'+key-59);
    if (key>=63 && key<=68) {
        static const unsigned function_codes[]={15,17,18,19,20,21};
        code=function_codes[key-63];
    }
    if (key==87 || key==88) code=key==87?23:24;
    if (!final && !code) return 0;
    unsigned used=0;out[used++]=27;
    if (final && mod==1) {
        out[used++]=(vga_screens[vt-1].application_cursor || (key>=59 && key<=62))?'O':'[';
        out[used++]=final;
        return used;
    }
    out[used++]='[';
    if (final) out[used++]='1';
    else {
        if (code>=10) out[used++]=(char)('0'+code/10);
        out[used++]=(char)('0'+code%10);
    }
    if (mod!=1) {out[used++]=';';out[used++]=(char)('0'+mod);}
    out[used++]=final?final:'~';
    return used;
}
