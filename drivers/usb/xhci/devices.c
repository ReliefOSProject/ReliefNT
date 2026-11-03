/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/heap.h>
#include <reliefnt/framebuffer.h>

/** @brief Format a bounded USB control request for the host transport. */
int xhci_request(struct usb_device *device,uint8_t type,uint8_t code,uint16_t value,
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

/** @brief Keep the HID submission boundary while sharing interrupt-ring ownership with Hubs. */
int xhci_hid_arm(struct xhci_device *device,unsigned dci)
{return xhci_interrupt_arm(device,dci);}

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
    uint32_t actual;int rc=xhci_request(&device->usb,0x21,11,0,interface->number,NULL,0,&actual);
    if(rc || actual)return rc ? rc : -EPROTO;
    rc=xhci_request(&device->usb,0x21,10,0,interface->number,NULL,0,&actual);
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

/** @brief Bind one connection after deriving its parent route, without changing public USB ABI. */
static int device_attach(struct xhci_controller *controller,uint8_t root_port,uint8_t speed_id,
                          struct xhci_device *parent,uint8_t parent_port,
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
    if(parent) {
        int topology=xhci_topology_child(device,parent,parent_port);
        if(topology){*rejection=topology==-EOPNOTSUPP ? XHCI_REJECT_HUB : XHCI_REJECT_NONE;
            kernel_free(device);return topology;}
    }
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
    xhci_context_topology(device);
    command=(struct xhci_trb){.lo=(uint32_t)device->input.physical,.hi=(uint32_t)(device->input.physical>>32),
        .control=XHCI_TRB_TYPE(11)|((uint32_t)slot<<24)};
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rc=xhci_command(controller,command,NULL);
    if(rc) {
        if(rc==-EIO && (controller->command_event.status>>24)==4u && xhci_tt_clear(device,1))
            __atomic_store_n(&controller->stop.failed,true,__ATOMIC_RELEASE);
        goto fail;
    }
    device->endpoints[1].configured=true;
    device->usb.address=((uint32_t *)device->output.cpu)[3]&255u;
    if(!device->usb.address){rc=-EPROTO;goto fail;}
    uint8_t descriptor[18];uint32_t actual=0;uint16_t packet;
    rc=xhci_request(&device->usb,0x80,6,0x100,0,descriptor,8,&actual);
    if(rc || actual!=8 || descriptor[0]!=18 || descriptor[1]!=1){rc=rc ? rc : -EPROTO;goto fail;}
    rc=xhci_ep0_packet(speed,descriptor[7],&packet);if(rc)goto fail;
    rc=set_ep0_packet(device,packet);if(rc)goto fail;
    rc=xhci_request(&device->usb,0x80,6,0x100,0,descriptor,sizeof(descriptor),&actual);
    if(rc || actual!=sizeof(descriptor) || descriptor[0]!=18 || descriptor[1]!=1 || !descriptor[17]) {
        rc=rc ? rc : -EPROTO;goto fail;
    }
    device->usb.vendor=descriptor[8]|(uint16_t)descriptor[9]<<8;
    device->usb.product=descriptor[10]|(uint16_t)descriptor[11]<<8;
    uint8_t *bytes=kernel_malloc(USB_MAX_CONFIG);struct usb_config *config=kernel_malloc(sizeof(*config));
    if(!bytes || !config){kernel_free(bytes);kernel_free(config);rc=-ENOMEM;goto fail;}
    rc=xhci_request(&device->usb,0x80,6,0x200,0,bytes,9,&actual);
    if(!rc && actual!=9)rc=-EPROTO;
    unsigned total=!rc ? bytes[2]|(unsigned)bytes[3]<<8 : 0;
    if(!rc && (total<9 || total>USB_MAX_CONFIG)){*rejection=XHCI_REJECT_CONFIG;rc=-EOPNOTSUPP;}
    if(!rc)rc=xhci_request(&device->usb,0x80,6,0x200,0,bytes,(uint16_t)total,&actual);
    if(!rc && actual!=total)rc=-EPROTO;
    if(!rc) {
        rc=usb_parse_configuration(bytes,total,speed,config);
        if(rc==-EOPNOTSUPP)*rejection=XHCI_REJECT_CONFIG;
    }
    kernel_free(bytes);
    if(rc){kernel_free(config);goto fail;}
    bool hub_device=descriptor[4]==9;
    if(hub_device && (config->count!=1 || xhci_classify_interface(&config->interfaces[0])!=4)) {
        *rejection=XHCI_REJECT_HUB;kernel_free(config);rc=-EOPNOTSUPP;goto fail;
    }
    /* Class Hub descriptor requests are defined only after SET_CONFIGURATION.
     * The xHC's first Configure Endpoint still receives all Hub slot fields. */
    if(hub_device) {
        rc=xhci_request(&device->usb,0,9,config->value,0,NULL,0,&actual);
        if(rc || actual){kernel_free(config);if(!rc)rc=-EPROTO;goto fail;}
    }
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
        } else if(kind==2 || kind==3 || kind==4) {
            if(kind==4) {
                rc=hub_device ? xhci_hub_prepare(device,interface,descriptor) : -EOPNOTSUPP;
                if(rc==-EOPNOTSUPP)*rejection=XHCI_REJECT_HUB;
            }
            if(!rc)rc=add_endpoint(device,&interface->interrupt,true);
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
        if(!hub_device) {
            rc=xhci_request(&device->usb,0,9,config->value,0,NULL,0,&actual);
            if(!rc && actual)rc=-EPROTO;
        }
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
        else if(kind==4)result=xhci_hub_start(device);
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
    console_info("[usb] xHCI device slot=%u address=%u root_port=%u psi=%u speed=%s bps=%llu psi_raw=%08x psi_source=%s vid=%04x pid=%04x route=%05x parent=%u port=%u\n",
        slot,device->usb.address,root_port+1u,speed_id,speed_names[speed],
        (unsigned long long)protocol->rates[speed_id],raw_psi,protocol->psi_count ? "explicit" : "implied",
        device->usb.vendor,device->usb.product,device->route,parent ? parent->slot_id : 0,parent_port);
    return 0;
fail:
    xhci_device_disconnect(device);xhci_devices_collect(controller);return rc;
}

/** @brief Address a root-port connection using its unmodified hardware PSI value. */
int xhci_device_attach(struct xhci_controller *controller,uint8_t root_port,uint8_t speed_id,
                       enum xhci_port_rejection *rejection)
{return device_attach(controller,root_port,speed_id,NULL,0,rejection);}

/** @brief Bind one Hub port while preserving the root-port and raw speed namespace. */
int xhci_device_attach_child(struct xhci_device *parent,uint8_t port,uint8_t speed_id,
                             enum xhci_port_rejection *rejection)
{
    if(!parent)return -EINVAL;
    return device_attach(parent->controller,parent->usb.root_port,speed_id,parent,port,rejection);
}

/** @brief Mark an entire subtree dead and release keys/buttons before deferred storage removal. */
void xhci_device_disconnect(struct xhci_device *device)
{
    struct xhci_controller *controller=device->controller;
    uint64_t flags;kernel_spin_lock_irqsave(&controller->event_lock,&flags);
    for(struct xhci_device *child=controller->devices;child;child=child->next) {
        struct xhci_device *ancestor=child;
        while(ancestor && ancestor!=device)ancestor=ancestor->parent;
        if(!ancestor || __atomic_exchange_n(&child->usb.disconnected,true,__ATOMIC_ACQ_REL))continue;
        for(unsigned dci=2;dci<XHCI_ENDPOINTS;++dci)
            if(child->endpoints[dci].hid)usb_hid_boot_release(&child->endpoints[dci].hid_state);
    }
    kernel_spin_unlock_irqrestore(&controller->event_lock,flags);
}

/** @brief Keep a Hub alive until every child object and its old block users have drained. */
static bool has_child(struct xhci_device *device)
{
    for(struct xhci_device *child=device->controller->devices;child;child=child->next)
        if(child->parent==device)return true;
    return false;
}

static void release_device(struct xhci_device *device)
{
    for(unsigned i=0;i<device->storage_count;++i)usb_storage_release(device->storage[i]);
    for(unsigned dci=1;dci<XHCI_ENDPOINTS;++dci) {
        xhci_dma_release(&device->endpoints[dci].replacement);
        xhci_dma_release(&device->endpoints[dci].dma);
    }
    xhci_dma_release(&device->input);xhci_dma_release(&device->output);
    kernel_free(device->hub);kernel_free(device);
}

/** @brief Retire leaves first; failed hardware retains every reachable DMA allocation. */
void xhci_devices_collect(struct xhci_controller *controller)
{
    struct xhci_device **at=&controller->devices;
    bool retry=false;
    while(*at) {
        struct xhci_device *device=*at;
        if(!device->usb.disconnected){at=&device->next;continue;}
        if(has_child(device)){retry=true;at=&device->next;continue;}
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
