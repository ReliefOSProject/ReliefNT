/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/framebuffer.h>

static int request(struct usb_device *device,uint8_t type,uint8_t code,uint16_t value,
                   uint16_t index,void *data,uint16_t length,uint32_t *actual)
{
    uint8_t setup[8]={type,code,(uint8_t)value,(uint8_t)(value>>8),
        (uint8_t)index,(uint8_t)(index>>8),(uint8_t)length,(uint8_t)(length>>8)};
    return device->operations->control(device,setup,data,length,actual);
}
static unsigned dci_for(uint8_t address)
{return (address&15u)*2u+((address&0x80u)!=0);}

static int endpoint_allocate(struct xhci_device *device,unsigned dci,const struct usb_endpoint *endpoint)
{
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if(state->dma.physical)return -EINVAL;
    int rc=xhci_dma_alloc(device->controller,&state->dma,1);if(rc)return rc;
    rc=xhci_ring_init(&state->ring,state->dma.cpu,state->dma.physical,XHCI_RING_COUNT);
    if(rc)return rc;
    if(endpoint)state->endpoint=*endpoint;
    return 0;
}

/* Called with event_lock held. Report storage occupies the rest of the same
 * aligned page, separate from the 64-TRB transfer ring. */
int xhci_hid_arm(struct xhci_device *device,unsigned dci)
{
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if(state->active || !state->hid || device->usb.disconnected)return -EINVAL;
    struct xhci_trb trb;
    int rc=xhci_build_normal(state->dma.physical+1024u,state->endpoint.max_packet,true,&trb);
    if(rc)return rc;
    state->completion=(struct xhci_completion){.pending={.length=state->endpoint.max_packet,
        .event_type=XHCI_EVENT_TRANSFER,.slot=device->slot_id,.endpoint=(uint8_t)dci},.input=true};
    rc=xhci_ring_submit(&state->ring,&trb,1,&state->completion.pending);
    if(!rc) {
        state->active=true;__atomic_thread_fence(__ATOMIC_SEQ_CST);
        xhci_write(controller,controller->doorbells+(uint32_t)device->slot_id*4u,dci);
    }
    return rc;
}

/* Called by the event consumer while it owns event_lock; no allocation/wait. */
void xhci_hid_event(struct xhci_device *device,unsigned dci)
{
    struct xhci_controller *controller=device->controller;
    struct xhci_endpoint_state *state=&device->endpoints[dci];
    if(state->completion.recovery) {
        state->needs_recovery=true;xhci_ring_quarantine(&state->ring);
        usb_hid_boot_release(&state->hid_state);usb_host_request_service(controller->host);return;
    }
    if(!state->completion.result && !device->usb.disconnected) {
        const uint8_t *report=(const uint8_t *)state->dma.cpu+1024u;
        if(state->hid_protocol==1)usb_hid_boot_keyboard(&state->hid_state,report,state->completion.actual);
        else usb_hid_boot_mouse(&state->hid_state,report,state->completion.actual);
    }
    (void)xhci_ring_reclaim(&state->ring,1,false);state->active=false;
    if(!device->usb.disconnected && xhci_hid_arm(device,dci)) {
        state->needs_recovery=true;usb_host_request_service(controller->host);
    }
}

static int hid_start(struct xhci_device *device,const struct usb_config_interface *interface)
{
    struct xhci_controller *controller=device->controller;
    unsigned dci=dci_for(interface->interrupt.address);struct xhci_endpoint_state *state=&device->endpoints[dci];
    uint32_t actual;int rc=request(&device->usb,0x21,11,0,interface->number,NULL,0,&actual);
    if(rc || actual)return rc ? rc : -EPROTO;
    rc=request(&device->usb,0x21,10,0,interface->number,NULL,0,&actual);
    if(rc && rc!=-EPIPE)return rc;
    state->hid=true;state->hid_protocol=interface->protocol;state->interface_number=interface->number;
    const struct framebuffer *fb=framebuffer_get();
    if(fb->available){state->hid_state.x=fb->width/2;state->hid_state.y=fb->height/2;}
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    rc=xhci_hid_arm(device,dci);
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    return rc;
}

static int set_ep0_packet(struct xhci_device *device,uint16_t packet)
{
    if(packet==device->usb.ep0_max_packet)return 0;
    struct xhci_controller *controller=device->controller;uint32_t *input=device->input.cpu;
    __builtin_memset(input,0,4096);
    input[1]=2u;input[2u*(controller->stride/4u)+1u]=(uint32_t)packet<<16;
    struct xhci_trb command={.lo=(uint32_t)device->input.physical,
        .hi=(uint32_t)(device->input.physical>>32),
        .control=XHCI_TRB_TYPE(13)|((uint32_t)device->slot_id<<24)};
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int rc=xhci_command(controller,command,NULL);
    if(!rc)device->usb.ep0_max_packet=packet;
    return rc;
}

static int add_endpoint(struct xhci_device *device,const struct usb_endpoint *endpoint,bool interrupt)
{
    struct xhci_controller *controller=device->controller;unsigned dci=dci_for(endpoint->address);
    if(dci<2 || dci>=XHCI_ENDPOINTS)return -EINVAL;
    int rc=endpoint_allocate(device,dci,endpoint);if(rc)return rc;
    return xhci_context_endpoint(device->input.cpu,4096,controller->stride,device->usb.speed,
                                 endpoint,interrupt,device->endpoints[dci].dma.physical);
}

int xhci_device_attach(struct xhci_controller *controller,uint8_t root_port,uint8_t speed_id,
                       enum xhci_port_rejection *rejection)
{
    if(!rejection)return -EINVAL;
    *rejection=XHCI_REJECT_NONE;
    if(root_port>=controller->ports || !speed_id || speed_id>15 ||
       !(controller->protocol[root_port].valid_speeds&(1u<<speed_id))) {
        if(root_port<controller->ports && speed_id && speed_id<=15)*rejection=XHCI_REJECT_SPEED;
        return -EOPNOTSUPP;
    }
    uint8_t speed=controller->protocol[root_port].speeds[speed_id];
    struct xhci_device *device=kernel_malloc(sizeof(*device));if(!device)return -ENOMEM;
    *device=(struct xhci_device){.controller=controller,.speed_id=speed_id,
        .usb={.operations=&xhci_device_operations,.owner=THIS_MODULE,.speed=speed,.root_port=root_port,
            .ep0_max_packet=speed>=USB_SPEED_SUPER ? 512 : speed==USB_SPEED_HIGH ? 64 : 8,
            .bus=controller->pci.bus,.slot=controller->pci.slot,.function=controller->pci.function}};
    device->usb.host_context=device;
    struct xhci_trb command={.control=XHCI_TRB_TYPE(9)|
        ((uint32_t)controller->protocol[root_port].slot_type<<16)};
    uint8_t slot=0;int rc=xhci_command(controller,command,&slot);
    if(rc || !slot || slot>controller->slots){
        if(!rc)__atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
        kernel_free(device);return rc ? rc : -EPROTO;
    }
    device->slot_id=slot;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    device->next=controller->devices;controller->devices=device;
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
    if((rc=xhci_dma_alloc(controller,&device->input,1)) ||
       (rc=xhci_dma_alloc(controller,&device->output,1)) ||
       (rc=endpoint_allocate(device,1,NULL)))goto fail;
    ((uint64_t *)controller->dcbaa.cpu)[slot]=device->output.physical;
    rc=xhci_context_address(device->input.cpu,4096,controller->stride,speed_id,root_port+1u,
                            device->usb.ep0_max_packet,device->endpoints[1].dma.physical);
    if(rc)goto fail;
    command=(struct xhci_trb){.lo=(uint32_t)device->input.physical,.hi=(uint32_t)(device->input.physical>>32),
        .control=XHCI_TRB_TYPE(11)|((uint32_t)slot<<24)};
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rc=xhci_command(controller,command,NULL);if(rc)goto fail;
    device->endpoints[1].configured=true;
    device->usb.address=((uint32_t *)device->output.cpu)[3]&255u;
    if(!device->usb.address){rc=-EPROTO;goto fail;}
    uint8_t descriptor[18];uint32_t actual=0;uint16_t packet;
    rc=request(&device->usb,0x80,6,0x100,0,descriptor,8,&actual);
    if(rc || actual!=8 || descriptor[0]!=18 || descriptor[1]!=1){rc=rc ? rc : -EPROTO;goto fail;}
    rc=xhci_ep0_packet(speed,descriptor[7],&packet);if(rc)goto fail;
    rc=set_ep0_packet(device,packet);if(rc)goto fail;
    rc=request(&device->usb,0x80,6,0x100,0,descriptor,sizeof(descriptor),&actual);
    if(rc || actual!=sizeof(descriptor) || descriptor[0]!=18 || descriptor[1]!=1 || !descriptor[17]) {
        rc=rc ? rc : -EPROTO;goto fail;
    }
    if(descriptor[4]==9){*rejection=XHCI_REJECT_HUB;rc=-EOPNOTSUPP;goto fail;}
    device->usb.vendor=descriptor[8]|(uint16_t)descriptor[9]<<8;
    device->usb.product=descriptor[10]|(uint16_t)descriptor[11]<<8;
    uint8_t *bytes=kernel_malloc(USB_MAX_CONFIG);struct usb_config *config=kernel_malloc(sizeof(*config));
    if(!bytes || !config){kernel_free(bytes);kernel_free(config);rc=-ENOMEM;goto fail;}
    rc=request(&device->usb,0x80,6,0x200,0,bytes,9,&actual);
    if(!rc && actual!=9)rc=-EPROTO;
    unsigned total=!rc ? bytes[2]|(unsigned)bytes[3]<<8 : 0;
    if(!rc && (total<9 || total>USB_MAX_CONFIG)){*rejection=XHCI_REJECT_CONFIG;rc=-EOPNOTSUPP;}
    if(!rc)rc=request(&device->usb,0x80,6,0x200,0,bytes,(uint16_t)total,&actual);
    if(!rc && actual!=total)rc=-EPROTO;
    if(!rc) {
        rc=usb_parse_configuration(bytes,total,speed,config);
        if(rc==-EOPNOTSUPP)*rejection=XHCI_REJECT_CONFIG;
    }
    kernel_free(bytes);
    if(rc){kernel_free(config);goto fail;}
    unsigned selected=0;uint32_t *input=device->input.cpu,*output=device->output.cpu;
    __builtin_memset(input,0,4096);
    unsigned stride=controller->stride/4u;
    input[1]=1u;input[stride]=output[0];input[stride+1]=output[1];input[stride+2]=output[2];
    for(unsigned i=0;i<config->count && !rc;++i) {
        const struct usb_config_interface *interface=&config->interfaces[i];
        int kind=xhci_classify_interface(interface);
        if(kind<0){if(kind==-EOPNOTSUPP && interface->class_code==9)*rejection=XHCI_REJECT_HUB;
            rc=kind;break;}
        if(kind==1) {
            rc=add_endpoint(device,&interface->in,false);
            if(!rc)rc=add_endpoint(device,&interface->out,false);
            if(!rc)++selected;
        } else if(kind==2 || kind==3) {
            rc=add_endpoint(device,&interface->interrupt,true);
            if(!rc)++selected;
        }
    }
    if(!rc && !selected){*rejection=XHCI_REJECT_INTERFACE;rc=-EOPNOTSUPP;}
    if(!rc) {
        command=(struct xhci_trb){.lo=(uint32_t)device->input.physical,.hi=(uint32_t)(device->input.physical>>32),
            .control=XHCI_TRB_TYPE(12)|((uint32_t)slot<<24)};
        __atomic_thread_fence(__ATOMIC_SEQ_CST);rc=xhci_command(controller,command,NULL);
    }
    if(!rc) {
        for(unsigned dci=2;dci<XHCI_ENDPOINTS;++dci)
            if(device->endpoints[dci].dma.physical)device->endpoints[dci].configured=true;
        rc=request(&device->usb,0,9,config->value,0,NULL,0,&actual);
        if(!rc && actual)rc=-EPROTO;
    }
    unsigned bound=0;
    if(!rc)for(unsigned i=0;i<config->count;++i) {
        const struct usb_config_interface *interface=&config->interfaces[i];
        int kind=xhci_classify_interface(interface),result=0;
        if(kind==1) {
            struct usb_storage *storage=NULL;
            result=usb_storage_probe(&device->usb,interface->number,interface->in,interface->out,&storage);
            if(storage)device->storage[device->storage_count++]=storage;
        } else if(kind==2 || kind==3)result=hid_start(device,interface);
        else continue;
        if(!result)++bound;
        else console_warn("[usb] xHCI class unavailable slot=%u interface=%u error=%d\n",slot,interface->number,result);
        if(controller->stop.failed)break;
    }
    kernel_free(config);
    if(rc || !bound || controller->stop.failed){if(!rc)rc=-EOPNOTSUPP;goto fail;}
    static const char *const speed_names[]={"FULL","LOW","HIGH","SUPER","SUPER_PLUS"};
    const struct xhci_protocol *protocol=&controller->protocol[root_port];
    uint32_t raw_psi=0;
    for(unsigned i=0;i<protocol->psi_count;++i)
        if((protocol->psi[i]&15u)==speed_id){raw_psi=protocol->psi[i];break;}
    console_info("[usb] xHCI device slot=%u address=%u root_port=%u psi=%u speed=%s bps=%llu psi_raw=%08x psi_source=%s vid=%04x pid=%04x\n",
        slot,device->usb.address,root_port+1u,speed_id,speed_names[speed],
        (unsigned long long)protocol->rates[speed_id],raw_psi,protocol->psi_count ? "explicit" : "implied",
        device->usb.vendor,device->usb.product);
    return 0;
fail:
    xhci_device_disconnect(device);xhci_devices_collect(controller);return rc;
}

void xhci_device_disconnect(struct xhci_device *device)
{
    if(__atomic_exchange_n(&device->usb.disconnected,true,__ATOMIC_ACQ_REL))return;
    uint64_t flags;kernel_spin_lock_irqsave(&device->controller->event_lock,&flags);
    for(unsigned dci=2;dci<XHCI_ENDPOINTS;++dci)
        if(device->endpoints[dci].hid)usb_hid_boot_release(&device->endpoints[dci].hid_state);
    kernel_spin_unlock_irqrestore(&device->controller->event_lock,flags);
}

static void release_device(struct xhci_device *device)
{
    for(unsigned i=0;i<device->storage_count;++i)usb_storage_release(device->storage[i]);
    for(unsigned dci=1;dci<XHCI_ENDPOINTS;++dci) {
        xhci_dma_release(&device->endpoints[dci].replacement);
        xhci_dma_release(&device->endpoints[dci].dma);
    }
    xhci_dma_release(&device->input);xhci_dma_release(&device->output);kernel_free(device);
}

void xhci_devices_collect(struct xhci_controller *controller)
{
    struct xhci_device **at=&controller->devices;
    bool retry=false;
    while(*at) {
        struct xhci_device *device=*at;
        if(!device->usb.disconnected){at=&device->next;continue;}
        if(!device->disabled && !__atomic_load_n(&controller->stop.failed,__ATOMIC_ACQUIRE)) {
            struct xhci_trb command={.control=XHCI_TRB_TYPE(10)|((uint32_t)device->slot_id<<24)};
            int rc=xhci_command(controller,command,NULL);
            if(rc)__atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
            else {
                ((uint64_t *)controller->dcbaa.cpu)[device->slot_id]=0;
                __atomic_thread_fence(__ATOMIC_SEQ_CST);device->disabled=true;
            }
        }
        bool busy=false;
        for(unsigned i=0;i<device->storage_count;++i)
            if(usb_storage_disconnect(device->storage[i]))busy=true;
        if(busy){retry=true;at=&device->next;continue;}
        /* Class admission can close even when hardware cannot be stopped.
         * Retain every hardware-reachable allocation until HALT is proven. */
        if(!device->disabled && !controller->stop.stopped){at=&device->next;continue;}
        uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
        *at=device->next;
        kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
        release_device(device);
    }
    __atomic_store_n(&controller->collect_pending,retry,__ATOMIC_RELEASE);
}

void xhci_devices_release(struct xhci_controller *controller)
{
    while(controller->devices) {
        struct xhci_device *device=controller->devices;controller->devices=device->next;
        device->usb.disconnected=true;
        for(unsigned dci=2;dci<XHCI_ENDPOINTS;++dci)
            if(device->endpoints[dci].hid)usb_hid_boot_release(&device->endpoints[dci].hid_state);
        for(unsigned i=0;i<device->storage_count;++i)(void)usb_storage_disconnect(device->storage[i]);
        release_device(device);
    }
}
