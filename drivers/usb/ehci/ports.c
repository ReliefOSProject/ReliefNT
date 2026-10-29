/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/time.h>

/** @brief Write only intentional port changes without echoing W1C status. */
static void port_update(struct ehci_controller *controller, unsigned port,
                          uint32_t set, uint32_t clear, uint32_t acknowledge)
{
    uint32_t offset=EHCI_PORTSC+4u*port;
    uint32_t value=ehci_read(controller,offset)&~EHCI_PORT_W1C;
    ehci_write(controller,offset,(value&~clear)|set|(acknowledge&EHCI_PORT_W1C));
}

/** @brief Reset a high-speed root port, handing full/low-speed devices to companions. */
static int root_reset(struct ehci_controller *controller, unsigned port)
{
    uint32_t status=ehci_read(controller,EHCI_PORTSC+4u*port);
    if (!(status&EHCI_PORT_CONNECT))return -ENODEV;
    if ((status>>10&3u)==1u) {
        port_update(controller,port,EHCI_PORT_OWNER,0,EHCI_PORT_W1C);return -EOPNOTSUPP;
    }
    port_update(controller,port,EHCI_PORT_RESET,EHCI_PORT_ENABLE|EHCI_PORT_OWNER,EHCI_PORT_W1C);
    ehci_delay(50);
    port_update(controller,port,0,EHCI_PORT_RESET,0);
    int rc=ehci_wait(controller,EHCI_PORTSC+4u*port,EHCI_PORT_RESET,0,100);
    if(rc)return rc;
    ehci_delay(10);
    status=ehci_read(controller,EHCI_PORTSC+4u*port);
    if (!(status&EHCI_PORT_CONNECT))return -ENODEV;
    if (!(status&EHCI_PORT_ENABLE)) {
        port_update(controller,port,EHCI_PORT_OWNER,0,EHCI_PORT_W1C);return -EOPNOTSUPP;
    }
    port_update(controller,port,0,0,EHCI_PORT_W1C);
    return 0;
}

/** @brief Find a still-connected child without lending its pointer outside the gate. */
static struct ehci_device *child_at(struct ehci_controller *controller,
                                     struct ehci_device *parent,unsigned port)
{
    for(struct ehci_device *device=controller->devices;device;device=device->next)
        if(!device->usb.disconnected && device->parent==parent &&
            (parent ? device->parent_port==port : device->usb.root_port==port))return device;
    return NULL;
}

/** @brief Read the four-byte USB hub port status without trusting a short response. */
static int hub_status(struct ehci_device *hub,unsigned port,uint16_t *status,uint16_t *change)
{
    uint8_t bytes[4];uint32_t actual;
    int rc=ehci_request(&hub->usb,0xa3,0,0,port,bytes,sizeof(bytes),&actual);
    if(rc)return rc;
    if(actual!=sizeof(bytes))return -EPROTO;
    *status=bytes[0]|(uint16_t)bytes[1]<<8;*change=bytes[2]|(uint16_t)bytes[3]<<8;
    return 0;
}

/** @brief Clear only hub change features reported by GET_PORT_STATUS. */
static void hub_ack(struct ehci_device *hub,unsigned port,uint16_t change)
{
    for(unsigned bit=0;bit<5;++bit)if(change&(1u<<bit)) {
        uint32_t actual;(void)ehci_request(&hub->usb,0x23,1,16u+bit,port,NULL,0,&actual);
    }
}

/** @brief Reset one hub port and propagate its actual speed/TT route to enumeration. */
static int hub_attach(struct ehci_controller *controller,struct ehci_device *hub,unsigned port)
{
    uint32_t actual;uint16_t status,change;
    ehci_delay(100);
    int rc=ehci_request(&hub->usb,0x23,3,4,port,NULL,0,&actual);if(rc)return rc;
    for(unsigned retry=0;retry<10;++retry) {
        ehci_delay(20);
        rc=hub_status(hub,port,&status,&change);if(rc)return rc;
        if(!(status&1u))return -ENODEV;
        if(!(status&(1u<<4)) && (status&2u)) {
            hub_ack(hub,port,change);
            ehci_delay(10);
            unsigned speed=status&(1u<<9) ? USB_SPEED_HIGH : status&(1u<<8) ? USB_SPEED_LOW : USB_SPEED_FULL;
            return ehci_device_attach(controller,hub,port,hub->usb.root_port,speed);
        }
    }
    return -ETIMEDOUT;
}

/** @brief Power USB2 hub ports then enumerate devices using single-/multi-TT routing. */
int ehci_hub_initialize(struct ehci_controller *controller,struct ehci_device *hub)
{
    uint8_t descriptor[7];uint32_t actual;
    int rc=ehci_request(&hub->usb,0xa0,6,0x2900,0,descriptor,sizeof(descriptor),&actual);
    if(rc)return rc;
    if(actual!=sizeof(descriptor) || descriptor[0]<7 || descriptor[1]!=0x29 ||
        !descriptor[2] || descriptor[2]>127)return -EPROTO;
    hub->hub_ports=descriptor[2];
    for(unsigned port=1;port<=hub->hub_ports;++port) {
        rc=ehci_request(&hub->usb,0x23,3,8,port,NULL,0,&actual);
        if(rc && rc!=-EPIPE)return rc;
    }
    ehci_delay((unsigned)descriptor[5]*2u+20u);
    for(unsigned port=1;port<=hub->hub_ports && !controller->failed;++port) {
        uint16_t status,change;
        rc=hub_status(hub,port,&status,&change);if(rc)continue;
        hub_ack(hub,port,change);
        if((status&1u) && !(status&8u)) {
            int result=hub_attach(controller,hub,port);
            if(result && result!=-ENODEV)console_warn("[usb] hub=%u port=%u error=%d\n",hub->usb.address,port,result);
        }
    }
    return 0;
}

/** @brief Scan hub changes in task context; existing mounts keep removed identities alive. */
void ehci_hubs_scan(struct ehci_controller *controller)
{
    for(struct ehci_device *hub=controller->devices;hub && !controller->failed;hub=hub->next) {
        if(hub->usb.disconnected || !hub->hub_ports)continue;
        for(unsigned port=1;port<=hub->hub_ports && !controller->failed;++port) {
            uint16_t status,change;
            if(hub_status(hub,port,&status,&change))continue;
            struct ehci_device *child=child_at(controller,hub,port);
            if(child && ((change&1u) || !(status&1u) || !(status&2u))) {
                ehci_device_disconnect(controller,child);child=NULL;
            }
            hub_ack(hub,port,change);
            if(!child && (status&1u) && !(status&8u)) {
                int rc=hub_attach(controller,hub,port);
                if(rc && rc!=-ENODEV)console_warn("[usb] hub=%u port=%u attach error=%d\n",hub->usb.address,port,rc);
            }
        }
    }
}

/** @brief Observe hotplug/fatal status and input without sleeping in the timer IRQ. */
void ehci_poll(void *context)
{
    struct ehci_controller *controller=context;
    if(__atomic_load_n(&controller->failed,__ATOMIC_ACQUIRE))return;
    uint32_t status=ehci_read(controller,EHCI_STATUS);
    if(status&0x10u) {
        __atomic_store_n(&controller->failed,true,__ATOMIC_RELEASE);
        for(struct ehci_device *device=controller->devices;device;device=device->next)
            ehci_device_disconnect(controller,device);
        console_error("[usb] EHCI host-system error, DMA retained until safe teardown\n");return;
    }
    for(unsigned port=0;port<controller->port_count;++port) {
        uint32_t value=ehci_read(controller,EHCI_PORTSC+4u*port);
        if(value&EHCI_PORT_CHANGE) {
            __atomic_fetch_or(&controller->port_work,1u<<port,__ATOMIC_RELEASE);
            for(struct ehci_device *device=controller->devices;device;device=device->next)
                if(device->usb.root_port==port)ehci_device_disconnect(controller,device);
        }
    }
    ehci_hid_poll(controller);
    if(controller->port_work || time_uptime_us()>=controller->next_hub_scan)
        usb_host_request_service(controller->host);
    ehci_write(controller,EHCI_STATUS,status&0x3fu);
}

/** @brief Debounce/enumerate ports under the host gate with execution ownership released. */
void ehci_service(void *context)
{
    struct ehci_controller *controller=context;uint32_t expected=0;
    if(controller->failed)return;
    if(!__atomic_compare_exchange_n(&controller->busy,&expected,1,false,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) {
        usb_host_request_service(controller->host);return;
    }
    uint32_t ports=__atomic_exchange_n(&controller->port_work,0,__ATOMIC_ACQ_REL);
    for(unsigned port=0;port<controller->port_count && !controller->failed;++port)if(ports&(1u<<port)) {
        struct ehci_device *old=child_at(controller,NULL,port);
        if(old)ehci_device_disconnect(controller,old);
        uint32_t value=ehci_read(controller,EHCI_PORTSC+4u*port);
        port_update(controller,port,0,0,EHCI_PORT_W1C);
        if(!(value&EHCI_PORT_CONNECT))continue;
        ehci_delay(100);
        int rc=root_reset(controller,port);
        if(rc==-EOPNOTSUPP) {
            console_info("[usb] EHCI port=%u low/fullspeed handed to companion controller\n",port+1);continue;
        }
        if(!rc)rc=ehci_device_attach(controller,NULL,0,port,USB_SPEED_HIGH);
        if(rc && rc!=-ENODEV)console_warn("[usb] EHCI port=%u enumeration error=%d\n",port+1,rc);
    }
    if(!controller->failed) {
        ehci_hid_service(controller);
        if(time_uptime_us()>=controller->next_hub_scan) {
            ehci_hubs_scan(controller);controller->next_hub_scan=time_uptime_us()+500000u;
        }
        ehci_devices_collect(controller);
    }
    __atomic_store_n(&controller->busy,0,__ATOMIC_RELEASE);
}
