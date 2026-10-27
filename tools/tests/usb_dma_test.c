/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../drivers/usb/uhci/usb_uhci.c"
static void *backing;
static uint64_t allocated_phys=0x900000;
static unsigned allocations,frees;
static int mapping_fails,halt_stuck,master_stuck,teardown_error;
static uint16_t pci_command=5,command_register;
static uint32_t frame_base;
static int transfer_timeout;
static unsigned progress_calls;
static uint64_t allocate_dma(uint32_t pages,uint64_t mask)
{ assert(mask==UINT32_MAX && pages==sizeof(struct uhci_dma)/4096);++allocations;return allocated_phys; }
static void free_dma(uint64_t physical,uint32_t pages)
{ assert(physical==allocated_phys && pages==sizeof(struct uhci_dma)/4096);++frees; }
static void *map_dma(uint64_t physical)
{ assert(physical==allocated_phys);return mapping_fails ? NULL : backing; }
uint16_t x86_64_inw(uint16_t port)
{
    if(port==0x40)return command_register;
    assert(port==0x42);return halt_stuck ? 0 : 0x20;
}
void x86_64_outw(uint16_t value,uint16_t port)
{ if(port==0x40)command_register=value;else assert(port==0x44 || port==0x46); }
void x86_64_outl(uint32_t value,uint16_t port)
{ assert(port==0x48);frame_base=value; }
static uint16_t read_pci(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{ (void)bus;(void)slot;(void)function;assert(offset==4);return pci_command; }
static void write_pci(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset,uint16_t value)
{ (void)bus;(void)slot;(void)function;assert(offset==4);if(!master_stuck)pci_command=value; }
static void sleep_ms(uint64_t ms) { assert(ms==1); }
static void report_teardown(int error) { teardown_error=error; }
static int enumerate_pci(uint32_t index,struct reliefos_driver_pci_device *pci)
{
    if(index)return -ENODEV;
    *pci=(struct reliefos_driver_pci_device){.class_code=UHCI_PCI_CLASS,
        .subclass=UHCI_PCI_SUBCLASS,.prog_if=UHCI_PCI_PROGIF};return 0;
}
static uint32_t read_bar(uint8_t bus,uint8_t slot,uint8_t function,uint8_t offset)
{ (void)bus;(void)slot;(void)function;assert(offset==0x20);return 0x41; }
static int geometry(uint32_t *width,uint32_t *height)
{ *width=800;*height=600;return 0; }
static void mouse_motion(int32_t x,int32_t y,int32_t dx,int32_t dy,uint8_t buttons)
{ (void)x;(void)y;(void)dx;(void)dy;(void)buttons;abort(); }
static void mouse_wheel(int32_t x,int32_t y,int32_t wheel,uint8_t buttons)
{ (void)x;(void)y;(void)wheel;(void)buttons;abort(); }
int usb_host_unregister(struct usb_host *host) { assert(!host);return 0; }
int usb_host_register(void (*poll)(void *),void *context,struct module *owner,struct usb_host **out)
{ (void)poll;(void)context;(void)owner;(void)out;abort(); }
void console_log(enum console_log_level level,const char *format,...)
{ (void)level;(void)format; }
void input_handle_scancode(uint8_t code,uint8_t pressed) { (void)code;(void)pressed;abort(); }
void uhci_test_control_progress(void)
{
    ++progress_calls;
    if(transfer_timeout)return;
    struct uhci_td *tds=uhci_controllers[0].dma->control_tds;
    unsigned offset=0;
    for(unsigned i=0;i<USB_UHCI_MAX_CONTROL_TDS;++i) {
        unsigned bytes=((tds[i].token>>21)&0x7ffu)+1u;
        if(bytes==2048)bytes=0;
        unsigned pid=tds[i].token&0xffu;
        if(pid!=UHCI_PID_SETUP && bytes) {
            assert(tds[i].buffer>=allocated_phys &&
                   (uint64_t)tds[i].buffer+bytes<=allocated_phys+sizeof(struct uhci_dma));
            unsigned char *buffer=(unsigned char *)backing+tds[i].buffer-allocated_phys;
            for(unsigned n=0;n<bytes;++n,++offset) {
                if(pid==UHCI_PID_IN)buffer[n]=(unsigned char)(0xa0u+offset);
                else assert(buffer[n]==(unsigned char)(0x30u+offset));
            }
        }
        tds[i].status=bytes ? bytes-1u : 0x7ffu;
        if(tds[i].link&UHCI_LINK_TERMINATE)return;
    }
    abort();
}
int main(void)
{
    unsigned char stack[16]={0};static unsigned char module_static[16];
    assert(!usb_phys(stack));
    assert(!usb_phys(module_static));
    puts("PASS UHCI rejects arbitrary stack/module-static addresses as DMA backing");
    struct reliefos_driver_kernel_api api={.alloc_dma=allocate_dma,.free_dma=free_dma,
        .phys_to_virt=map_dma,.pci_read16=read_pci,.pci_write16=write_pci,
        .sleep_ms=sleep_ms,.report_teardown_failure=report_teardown};
    kernel_api=&api;
    backing=aligned_alloc(4096,sizeof(struct uhci_dma));assert(backing && (uintptr_t)backing>UINT32_MAX);
    memset(backing,0xa5,sizeof(struct uhci_dma));
    struct uhci_controller *controller=&uhci_controllers[0];
    assert(!uhci_alloc_dma(controller) && allocations==1 && !frees);
    assert(usb_phys(controller->dma->frame_list)==allocated_phys);
    assert(usb_phys(&controller->dma->qhs[3])==allocated_phys+offsetof(struct uhci_dma,qhs)+3*sizeof(struct uhci_qh));
    assert(!usb_phys((char *)backing+sizeof(struct uhci_dma)));
    controller->io_base=0x40;controller->ready=1;
    uhci_schedule_init(0);assert(frame_base==allocated_phys);
    uint8_t request[8]={0x80},data[16]={0};
    assert(!uhci_control_transfer(controller,1,0,8,request,data,sizeof(data)));
    for(unsigned n=0;n<sizeof(data);++n)assert(data[n]==(uint8_t)(0xa0u+n));
    request[0]=0;
    for(unsigned n=0;n<sizeof(data);++n)data[n]=(uint8_t)(0x30u+n);
    assert(!uhci_control_transfer(controller,1,0,8,request,data,sizeof(data)));
    puts("PASS UHCI IN/OUT control buffers use owned DMA and copy stack data only at completion");
    uint8_t largest[USB_UHCI_MAX_CONFIG]={0};request[0]=0x80;
    assert(!uhci_control_transfer(controller,1,0,8,request,largest,sizeof(largest)));
    for(unsigned n=0;n<sizeof(largest);++n)assert(largest[n]==(uint8_t)(0xa0u+n));
    puts("PASS UHCI whole advertised configuration capacity fits endpoint-zero minimum packets");
    transfer_timeout=1;
    assert(uhci_control_transfer(controller,1,0,8,request,data,sizeof(data))==-ETIMEDOUT);
    unsigned last_progress=progress_calls;
    assert(uhci_control_transfer(controller,1,0,8,request,data,sizeof(data))==-EIO &&
           progress_calls==last_progress && controller->control_failed);
    puts("PASS UHCI timed-out control DMA remains owned and cannot be overwritten by a new request");
    halt_stuck=1;
    uhci_driver_fini();
    assert(teardown_error==-ETIMEDOUT && !frees && controller->dma==backing && !(pci_command&4));
    halt_stuck=0;teardown_error=0;master_stuck=1;pci_command=5;
    uhci_driver_fini();
    assert(teardown_error==-EIO && !frees && controller->dma==backing);
    master_stuck=0;teardown_error=0;
    uhci_driver_fini();
    assert(!teardown_error && frees==1 && !controller->dma && !controller->dma_phys);
    allocated_phys=0x100000000ULL;
    assert(uhci_alloc_dma(controller)==-EIO && frees==2 && !controller->dma);
    allocated_phys=0x900000;mapping_fails=1;
    assert(uhci_alloc_dma(controller)==-EFAULT && frees==3 && !controller->dma);
    free(backing);
    puts("PASS UHCI explicit 32-bit backing, nonidentity CPU alias, page alignment, frame bus address, failed HALT/master retain and retry");
    api.abi_version=RELIEFOS_DRIVER_ABI_VERSION;api.struct_size=sizeof(api);
    api.pci_enumerate=enumerate_pci;api.pci_read32=read_bar;
    api.framebuffer_size=geometry;api.input_push_mouse=mouse_motion;
    api.input_push_mouse_wheel=mouse_wheel;
    master_stuck=1;pci_command=5;allocated_phys=0;mapping_fails=0;
    unsigned before=allocations;
    assert(uhci_driver_init(&api)==-EIO && allocations==before);
    master_stuck=0;teardown_error=0;uhci_driver_fini();
    assert(!teardown_error);
    puts("PASS UHCI probe first quiesces a previously active controller before allocating its schedule");
}
