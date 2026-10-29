/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/usb/ehci/internal.h"
#include <reliefnt/framebuffer.h>
#include <reliefnt/power.h>

static unsigned keys, motions, wheels;
static uint8_t last_key, last_pressed, buttons;
static int32_t motion_x, motion_y;
static struct framebuffer fb={.available=true,.width=800,.height=600};
static unsigned transfer_mode, target_error_once, target_partial_once, target_timeout_once,
    target_disconnect_once, clear_fail_once, clear_fail_at, clear_count, target_submits,
    fail_stop_on_target, fail_stop_wait;
static struct usb_device *disconnect_target;
static uint16_t clear_values[16], clear_indices[16];
static uint8_t clear_hubs[16];
static uint32_t command;
uint32_t ehci_read(struct ehci_controller *controller, unsigned offset)
{ (void)controller; assert(offset==EHCI_CMD); return command; }
void ehci_write(struct ehci_controller *controller, unsigned offset, uint32_t value)
{
    assert(offset==EHCI_CMD); command=value;
    if (!(value & EHCI_ASE)) return;
    struct ehci_dma *dma=controller->dma;
    bool control=(dma->descriptors[0].token >> 8 & 3u)==EHCI_PID_SETUP;
    bool clear=control && dma->setup[0]==0x23 && dma->setup[1]==8;
    if(clear) {
        assert(clear_count<16);
        clear_values[clear_count]=dma->setup[2]|(uint16_t)dma->setup[3]<<8;
        clear_indices[clear_count]=dma->setup[4]|(uint16_t)dma->setup[5]<<8;
        clear_hubs[clear_count++]=dma->transfer.characteristics&0x7f;
        if(clear_fail_once || (clear_fail_at && clear_count==clear_fail_at)) {
            if(clear_fail_once)--clear_fail_once;
            dma->descriptors[0].token|=EHCI_TD_HALTED|8u;return;
        }
    } else {
        ++target_submits;
        if(fail_stop_on_target) { --fail_stop_on_target;fail_stop_wait=1; }
        if(target_disconnect_once) {--target_disconnect_once;assert(disconnect_target);
            disconnect_target->disconnected=true;return;}
        if(target_timeout_once) {--target_timeout_once;return;}
        if(target_partial_once) {
            --target_partial_once;struct ehci_qtd *td=&dma->descriptors[0];
            unsigned bytes=td->token>>16&0x7fffu;assert(bytes>=14);
            unsigned char *pointer=(unsigned char *)dma+td->buffer[0]-controller->dma_phys;
            memset(pointer,0x5a,14);td->token&=~(EHCI_TD_ACTIVE|0x7fff0000u);
            td->token|=(bytes-14u)<<16|EHCI_TD_HALTED|8u;return;
        }
        if(target_error_once) {--target_error_once;dma->descriptors[0].token|=EHCI_TD_HALTED|8u;return;}
    }
    unsigned count=control ? 3 : 4;
    for (unsigned i=0;i<count;++i) {
        struct ehci_qtd *td=&dma->descriptors[i];
        unsigned bytes=td->token >> 16 & 0x7fffu;
        if (transfer_mode==2 && !i) {td->token|=EHCI_TD_HALTED;return;}
        unsigned received=bytes;
        if (transfer_mode==1 && (!control || i==1)) received=3;
        bool partial_stall=(transfer_mode==3 && !i) || (transfer_mode==4 && i==1);
        if(partial_stall)received=14;
        if (bytes && (td->token >> 8 & 3u)==EHCI_PID_IN) {
            unsigned char *pointer=(unsigned char *)dma+td->buffer[0]-controller->dma_phys;
            memset(pointer,0x6c,received);
        }
        td->token&=~(EHCI_TD_ACTIVE|0x7fff0000u);
        td->token|=(bytes-received)<<16;
        unsigned packet=dma->transfer.characteristics >> 16 & 0x7ffu;
        if (((received+packet-1u)/packet)&1u) td->token^=1u<<31;
        if(partial_stall){td->token|=EHCI_TD_HALTED;return;}
        if (transfer_mode==1 && !control) return;
        if (td->next==EHCI_TERMINATE) return;
    }
}
int ehci_wait(struct ehci_controller *controller, unsigned offset, uint32_t mask,
                uint32_t wanted, unsigned milliseconds)
{
    (void)controller;(void)offset;(void)milliseconds;
    if(fail_stop_wait && mask==EHCI_ASS && !wanted) { fail_stop_wait=0;return -ETIMEDOUT; }
    return 0;
}
static uint32_t pm_count,pm_step=100;
bool power_acpi_pm_timer_read(uint32_t *count,uint32_t *mask)
{*mask=0xffffffu;pm_count=(pm_count+pm_step)&*mask;*count=pm_count;return true;}
const struct framebuffer *framebuffer_get(void) { return &fb; }
void input_handle_scancode(uint8_t key, uint8_t pressed)
{ ++keys;last_key=key;last_pressed=pressed; }
void input_push_mouse(int32_t x,int32_t y,int32_t dx,int32_t dy,uint8_t value)
{ (void)x;(void)y;++motions;motion_x=dx;motion_y=dy;buttons=value; }
void input_push_mouse_wheel(int32_t x,int32_t y,int32_t wheel,uint8_t value)
{ (void)x;(void)y;assert(wheel==-1 && value==1);++wheels; }

int main(void)
{
    pm_count=0xfffff0u;
    struct ehci_timer timer;assert(!ehci_timer_start(&timer));
    while(!ehci_timer_expired(&timer,100000)) { }
    assert(timer.ticks>=ACPI_PM_TIMER_HZ/10 && pm_count<0x100000u);
    ehci_delay(1);
    puts("PASS IRQ-independent USB deadlines cross PM timer wrap with no scheduler/PIT tick");
    struct ehci_dma *dma=aligned_alloc(4096,sizeof(*dma));assert(dma);
    memset(dma,0xa5,sizeof(*dma));
    struct ehci_controller controller={.dma=dma,.dma_phys=0x900000};
    assert(sizeof(struct ehci_qh)==128 && sizeof(struct ehci_qtd)==64);
    assert(ehci_bus(&controller,dma->frames)==0x900000);
    assert(!ehci_bus(&controller,&controller));
    assert(!ehci_bus(&controller,(char *)dma+sizeof(*dma)));
    struct ehci_qtd *td=&dma->descriptors[0];
    assert(!ehci_qtd_prepare(&controller,td,EHCI_PID_IN,1,dma->data+4095,16385));
    assert(td->buffer[0]==ehci_bus(&controller,dma->data+4095));
    for(unsigned i=1;i<5;++i)assert(td->buffer[i]==ehci_bus(&controller,dma->data+i*4096));
    assert(td->token==(EHCI_TD_ACTIVE|(3u<<10)|(EHCI_PID_IN<<8)|(16385u<<16)|(1u<<31)));
    assert(td->next==1 && td->alternate==1);
    for(unsigned i=0;i<5;++i)assert(!td->high_buffer[i]);
    assert(ehci_qtd_prepare(&controller,td,EHCI_PID_IN,0,dma->data+4095,16386)==-EINVAL);
    assert(ehci_qtd_prepare(&controller,td,EHCI_PID_IN,0,&controller,4)==-EINVAL);
    assert(!ehci_qtd_prepare(&controller,td,EHCI_PID_OUT,0,NULL,0));
    for(unsigned i=0;i<5;++i)assert(!td->buffer[i]);
    struct usb_device device={.address=7,.speed=USB_SPEED_LOW,.hub_address=3,.hub_port=4};
    ehci_qh_prepare(&dma->transfer,&device,0,8,true);
    assert(dma->transfer.characteristics==(7|(1u<<12)|(1u<<14)|(8u<<16)|(1u<<27)|(4u<<28)));
    assert(dma->transfer.capabilities==((1u<<30)|(3u<<16)|(4u<<23)));
    puts("PASS owned 32-bit DMA, five-page qTD extent, zero-length TD, speed/TT/control routing");
    device=(struct usb_device){.host_context=&controller,.address=2,.speed=USB_SPEED_HIGH,
        .ep0_max_packet=64,.operations=&ehci_device_operations};
    uint32_t actual;
    uint8_t request[8]={0x80,6,0,1,0,0,8,0},response[8]={0};
    assert(!device.operations->control(&device,request,response,8,&actual) && actual==8);
    for(unsigned i=0;i<8;++i)assert(response[i]==0x6c);
    transfer_mode=1;memset(response,0,sizeof(response));
    assert(!device.operations->control(&device,request,response,8,&actual) && actual==3);
    assert(response[2]==0x6c && !response[3]);
    struct usb_endpoint endpoint={.address=0x81,.max_packet=512};
    uint8_t *large=calloc(1,EHCI_TRANSFER_BYTES);assert(large);
    transfer_mode=0;
    assert(!device.operations->bulk(&device,&endpoint,large,EHCI_TRANSFER_BYTES,&actual));
    assert(actual==EHCI_TRANSFER_BYTES && large[EHCI_TRANSFER_BYTES-1]==0x6c);
    transfer_mode=1;
    assert(!device.operations->bulk(&device,&endpoint,large,4096,&actual) && actual==3 && endpoint.toggle==1);
    transfer_mode=2;
    assert(device.operations->control(&device,request,response,8,&actual)==-EPIPE);
    transfer_mode=3;memset(large,0,EHCI_TRANSFER_BYTES);
    assert(device.operations->bulk(&device,&endpoint,large,512,&actual)==-EPIPE && actual==14);
    assert(large[13]==0x6c && !large[14]);
    transfer_mode=4;memset(large,0,EHCI_TRANSFER_BYTES);
    assert(device.operations->bulk(&device,&endpoint,large,EHCI_TRANSFER_BYTES,&actual)==-EPIPE && actual==16384+14);
    assert(large[16384+13]==0x6c && !large[16384+14]);
    assert(!device.operations->acquire(&device));
    assert(device.operations->acquire(&device)==-EAGAIN);device.operations->release(&device);
    device.disconnected=true;
    assert(device.operations->acquire(&device)==-ENODEV);
    assert(device.operations->bulk(&device,&endpoint,large,4096,&actual)==-ENODEV);
    free(large);
    puts("PASS control short/status, bulk64KiB/toggle, early STALL, serialized gate and removal");

    /* A split fault must clear the correct HS hub's TT before reuse. */
    struct ehci_device hub={.usb={.address=5,.speed=USB_SPEED_HIGH,.ep0_max_packet=64,
        .host_context=&controller,.operations=&ehci_device_operations},.hub_ports=4};
    struct ehci_device other_hub={.usb={.address=6,.speed=USB_SPEED_HIGH,.ep0_max_packet=64,
        .host_context=&controller,.operations=&ehci_device_operations},.hub_ports=2,.next=&hub};
    controller.devices=&other_hub;
    struct usb_device split={.address=9,.speed=USB_SPEED_FULL,.ep0_max_packet=8,
        .hub_address=5,.hub_port=3,.host_context=&controller,.operations=&ehci_device_operations};
    struct usb_endpoint bulk_in={.address=0x81,.max_packet=64};uint8_t split_data[64]={0};
    transfer_mode=0;clear_count=target_submits=0;target_error_once=1;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-EIO);
    assert(clear_count==1 && clear_hubs[0]==5 && clear_indices[0]==1 && clear_values[0]==0x9091);
    assert(!split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual) && actual==64);
    target_error_once=1;clear_count=0;
    assert(split.operations->control(&split,request,response,8,&actual)==-EIO);
    assert(clear_count==2 && clear_hubs[0]==5 && clear_hubs[1]==5 && clear_indices[0]==1 && clear_indices[1]==1);
    assert((clear_values[0]==0x90 && clear_values[1]==0x8090) ||
           (clear_values[1]==0x90 && clear_values[0]==0x8090));
    target_error_once=1;clear_count=0;clear_fail_once=1;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)<0);
    assert(clear_count==1);
    unsigned submits=target_submits;clear_fail_once=1;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)<0);
    assert(target_submits==submits && clear_count==2);
    assert(!split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual));
    assert(target_submits==submits+1 && clear_count==3);
    clear_count=0;target_partial_once=1;memset(split_data,0,sizeof(split_data));
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-EIO && actual==14);
    assert(split_data[13]==0x5a && !split_data[14] && clear_count==1);
    target_timeout_once=1;clear_count=0;pm_step=ACPI_PM_TIMER_HZ;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-ETIMEDOUT);
    assert(clear_count==1 && clear_hubs[0]==5);pm_step=100;
    target_error_once=1;clear_count=0;clear_fail_at=2;
    assert(split.operations->control(&split,request,response,8,&actual)==-EIO && clear_count==2);
    assert(clear_values[0]==0x90 && clear_values[1]==0x8090);
    clear_fail_at=0;submits=target_submits;
    assert(!split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual));
    assert(clear_count==3 && clear_values[2]==0x8090 && target_submits==submits+1);
    disconnect_target=&split;target_disconnect_once=1;clear_count=0;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-ENODEV);
    assert(clear_count==1 && clear_values[0]==0x9091);split.disconnected=false;
    transfer_mode=2;clear_count=0;
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-EPIPE);
    assert(clear_count==0); /* A completed downstream STALL has no pending TT split. */
    transfer_mode=0;
    /* Failed schedule retirement leaves DMA owned by the controller: neither
     * copy partial IN data nor issue a TT recovery request using that DMA. */
    clear_count=0;target_partial_once=1;fail_stop_on_target=1;memset(split_data,0,sizeof(split_data));
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-ETIMEDOUT);
    assert(actual==14 && !split_data[0] && !split_data[13]);
    assert(controller.failed && hub.tt_pending==2 && !clear_count);
    assert(split.operations->bulk(&split,&bulk_in,split_data,sizeof(split_data),&actual)==-ENODEV);
    puts("PASS split TT recovery routing, control directions, fail-closed retry and STALL");

    const uint8_t config[]={9,2,32,0,1,1,0,0x80,50,9,4,2,0,2,8,6,0x50,0,
        7,5,0x81,2,0,2,0,7,5,2,2,0,2,0};
    struct usb_config parsed;
    assert(!usb_parse_configuration(config,sizeof(config),USB_SPEED_HIGH,&parsed));
    assert(parsed.value==1 && parsed.count==1 && parsed.interfaces[0].number==2);
    assert(parsed.interfaces[0].in.address==0x81 && parsed.interfaces[0].out.address==2);
    assert(parsed.interfaces[0].in.max_packet==512);
    for(unsigned n=0;n<sizeof(config);++n)
        assert(usb_parse_configuration(config,n,USB_SPEED_HIGH,&parsed)<0);
    uint8_t bad[sizeof(config)];memcpy(bad,config,sizeof(bad));bad[18]=0;
    assert(usb_parse_configuration(bad,sizeof(bad),USB_SPEED_HIGH,&parsed)==-EINVAL);
    memcpy(bad,config,sizeof(bad));bad[20]=0x80;
    assert(usb_parse_configuration(bad,sizeof(bad),USB_SPEED_HIGH,&parsed)==-EINVAL);
    memcpy(bad,config,sizeof(bad));bad[29]=1;
    assert(usb_parse_configuration(bad,sizeof(bad),USB_SPEED_HIGH,&parsed)==-EINVAL);
    puts("PASS real configuration bounds, malformed lengths, reserved EP0, invalid highspeed packet size");

    struct usb_hid_state state={0};
    const uint8_t press[]={2,0,4,0,0,0,0,0},rollover[]={0,0,1,1,1,1,1,1},zero[8]={0};
    usb_hid_boot_keyboard(&state,press,8);assert(keys==2 && last_key==0x1e && last_pressed);
    usb_hid_boot_keyboard(&state,press,8);assert(keys==2);
    usb_hid_boot_keyboard(&state,rollover,8);assert(keys==2 && state.keys[0]==4 && state.modifier==2);
    usb_hid_boot_keyboard(&state,zero,7);assert(keys==2);
    usb_hid_boot_release(&state);assert(keys==4 && !last_pressed && !state.keys[0]);
    const uint8_t mouse[]={1,5,7,255};
    usb_hid_boot_mouse(&state,mouse,4);
    assert(motions==1 && motion_x==5 && motion_y==7 && buttons==1 && wheels==1);
    usb_hid_boot_release(&state);assert(motions==2 && !buttons);
    puts("PASS HID modifiers/releases, duplicate reports, rollover/truncation, mouse Y and disconnect buttons");
    free(dma);
}
