/* SPDX-License-Identifier: Apache-2.0 */
/* Link production HID, input fan-out and terminal key translation separately. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <reliefnt/usb_hid.h>
#include <reliefnt/input.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>
#include <reliefnt/storage.h>
#include <linux/input.h>
#include <linux/kd.h>
#include "../../drivers/tty/pty/internal.h"

uint8_t vt_ready,vt_graphical[6],console_shift_down,console_ctrl_down,console_alt_down;
uint32_t active_vt=1,console_modifier_keys;
int vt_keyboard_modes[6];bool console_vga_text_mode;
static struct pty_session session={.used=1,.owner_pid=1};
static uint8_t output[512];static uint32_t output_size;
uint32_t pty_vt_active(void){return 1;}
uint32_t pty_vt_number(uint32_t id){return id==1 ? 1 : 0;}
struct pty_session *find_session(uint32_t id){return id==1 ? &session : NULL;}
int pty_vt_switch(uint32_t number){(void)number;return 0;}
int64_t pty_write_input(uint32_t owner,uint32_t id,const char *bytes,uint32_t count)
{assert(owner==1 && id==1 && count<=sizeof(output)-output_size);memcpy(output+output_size,bytes,count);output_size+=count;return count;}
void console_vt_write(uint32_t vt,const char *bytes,size_t count){(void)vt;(void)bytes;(void)count;}
void console_log(enum console_log_level level,const char *format,...){(void)level;(void)format;}
unsigned vga_console_key(uint32_t vt,uint8_t key,unsigned modifiers,char *bytes)
{(void)vt;(void)key;(void)modifiers;(void)bytes;return 0;}
uint64_t time_uptime_us(void){return 0;}
const struct framebuffer *framebuffer_get(void){static const struct framebuffer fb={0};return &fb;}
void kernel_spin_lock_irqsave(struct kernel_spinlock *lock,uint64_t *flags)
{assert(!lock->state);lock->state=1;*flags=0;}
void kernel_spin_unlock_irqrestore(struct kernel_spinlock *lock,uint64_t flags)
{assert(lock->state && !flags);lock->state=0;}
void kernel_poll_readiness_notify(void){}

static void send_key(struct usb_hid_state *state,uint8_t usage)
{
    uint8_t pressed[8]={0,0,usage},released[8]={0};
    usb_hid_boot_keyboard(state,pressed,sizeof(pressed));input_process_pending();
    usb_hid_boot_keyboard(state,released,sizeof(released));input_process_pending();
}
static void keypad_events_and_raw(void)
{
    /* HID 53..63 map to standard Linux keypad codes and PC raw scan codes. */
    static const uint8_t linux_code[]={69,98,55,74,78,96,79,80,81,75,76,77,71,72,73,82,83};
    static const uint8_t set1[]={0x45,0x35,0x37,0x4a,0x4e,0x1c,0x4f,0x50,0x51,0x4b,0x4c,0x4d,0x47,0x48,0x49,0x52,0x53};
    struct usb_hid_state state={0};unsigned errors=0;input_init();vt_keyboard_modes[0]=K_RAW;
    uint64_t cursor=input_evdev_cursor_now();
    for(unsigned i=0;i<sizeof(linux_code);++i) {
        struct input_event events[6];output_size=0;send_key(&state,(uint8_t)(0x53+i));
        int count=input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0);
        if(count!=(int)sizeof(events) || events[1].type!=EV_KEY || events[1].code!=linux_code[i] || events[1].value!=1 ||
           events[4].type!=EV_KEY || events[4].code!=linux_code[i] || events[4].value!=0) {
            fprintf(stderr,"usage=%02x expected Linux=%u press/release, bytes=%d\n",0x53+i,linux_code[i],count);++errors;
        }
        bool extended=i==1 || i==5;
        uint8_t expected[4];unsigned length=0;
        if(extended)expected[length++]=0xe0;
        expected[length++]=set1[i];
        if(extended)expected[length++]=0xe0;
        expected[length++]=(uint8_t)(set1[i]|0x80);
        if(output_size!=length || memcmp(output,expected,length)) {
            fprintf(stderr,"usage=%02x bad raw make/break length=%u expected=%u\n",0x53+i,output_size,length);++errors;
        }
    }
    assert(!errors);
}
static void keypad_cooked_and_navigation(void)
{
    struct usb_hid_state state={0};input_init();vt_keyboard_modes[0]=K_XLATE;output_size=0;
    send_key(&state,0x60); /* numlock off: keypad 8 acts as up */
    send_key(&state,0x53); /* numlock on */
    send_key(&state,0x60);send_key(&state,0x57);send_key(&state,0x58);
    send_key(&state,0x53);send_key(&state,0x60); /* numlock off again */
    static const char expected[]="\033[A8+\r\033[A";
    assert(output_size==sizeof(expected)-1 && !memcmp(output,expected,output_size));
    /* Existing PS/2-normalized cursor input remains a Linux up event. */
    uint64_t cursor=input_evdev_cursor_now();
    input_handle_scancode(72,1);input_process_pending();input_handle_scancode(72,0);input_process_pending();
    struct input_event events[6];
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0)==sizeof(events));
    assert(events[1].code==KEY_UP && events[1].value==1 && events[4].code==KEY_UP && !events[4].value);
}
static void keypad_mediumraw(void)
{
    struct usb_hid_state state={0};input_init();vt_keyboard_modes[0]=K_MEDIUMRAW;output_size=0;
    send_key(&state,0x58);send_key(&state,0x60);
    static const uint8_t expected[]={96,96|0x80,72,72|0x80};
    assert(output_size==sizeof(expected) && !memcmp(output,expected,sizeof(expected)));
}
static void navigation_events_and_raw(void)
{
    static const uint8_t linux_code[]={110,102,104,111,107,109,106,105,108,103};
    static const uint8_t set1[]={0x52,0x47,0x49,0x53,0x4f,0x51,0x4d,0x4b,0x50,0x48};
    struct usb_hid_state state={0};input_init();vt_keyboard_modes[0]=K_RAW;
    uint64_t cursor=input_evdev_cursor_now();
    for(unsigned i=0;i<sizeof(linux_code);++i) {
        struct input_event events[6];output_size=0;send_key(&state,(uint8_t)(0x49+i));
        assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0)==sizeof(events));
        assert(events[1].type==EV_KEY && events[1].code==linux_code[i] && events[1].value==1);
        assert(events[4].type==EV_KEY && events[4].code==linux_code[i] && !events[4].value);
        uint8_t expected[]={0xe0,set1[i],0xe0,(uint8_t)(set1[i]|0x80)};
        assert(output_size==sizeof(expected) && !memcmp(output,expected,sizeof(expected)));
    }
}
static void simultaneous_keypad_and_navigation(void)
{
    struct usb_hid_state state={0};uint8_t held[8]={0,0,0x52,0x60};
    uint8_t keypad_only[8]={0,0,0x60},released[8]={0},bits[16];struct input_event events[6];
    input_init();vt_keyboard_modes[0]=K_RAW;output_size=0;uint64_t cursor=input_evdev_cursor_now();
    usb_hid_boot_keyboard(&state,held,sizeof(held));input_process_pending();
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0)==sizeof(events));
    assert(events[1].code==KEY_UP && events[1].value==1 && events[4].code==72 && events[4].value==1);
    usb_hid_boot_keyboard(&state,keypad_only,sizeof(keypad_only));input_process_pending();
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0)==3*(int)sizeof(events[0]));
    assert(events[1].code==KEY_UP && !events[1].value);
    input_evdev_key_state(bits,sizeof(bits));
    assert(!(bits[KEY_UP/8]&(1u<<(KEY_UP%8))) && (bits[72/8]&(1u<<(72%8))));
    usb_hid_boot_keyboard(&state,released,sizeof(released));input_process_pending();
    assert(input_evdev_read(STORAGE_DEV_KIND_KEYBOARD,&cursor,events,sizeof(events),0)==3*(int)sizeof(events[0]));
    assert(events[1].code==72 && !events[1].value);
}
int main(void)
{
    keypad_events_and_raw();navigation_events_and_raw();simultaneous_keypad_and_navigation();
    keypad_cooked_and_navigation();keypad_mediumraw();
    puts("PASS USB HID keypad through production evdev and raw/cooked terminal, preserving PS/2 navigation");
}
