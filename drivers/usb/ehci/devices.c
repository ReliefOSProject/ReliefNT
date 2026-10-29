/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/heap.h>

/** @brief Encode an unaligned standard control setup packet into stack CPU memory. */
int ehci_request(struct usb_device *device, uint8_t type, uint8_t request,
                   uint16_t value, uint16_t index, void *data, uint16_t length, uint32_t *actual)
{
    uint8_t setup[8]={type,request,value,value>>8,index,index>>8,length,length>>8};
    return device->operations->control(device,setup,data,length,actual);
}

/** @brief Reserve a wire address until its old physical device is disconnected. */
static unsigned allocate_address(struct ehci_controller *controller)
{
    for (unsigned address=1;address<128;++address) {
        uint64_t mask=1ull<<(address%64);
        if (!(controller->addresses[address/64]&mask)) {
            controller->addresses[address/64]|=mask;return address;
        }
    }
    return 0;
}

/** @brief Configure a reset USB device and bind only explicitly supported classes. */
int ehci_device_attach(struct ehci_controller *controller, struct ehci_device *parent,
                         uint8_t parent_port, uint8_t root_port, uint8_t speed)
{
    if (parent && parent->depth>=5) return -ELOOP;
    struct ehci_device *device=kernel_malloc(sizeof(*device));if(!device)return -ENOMEM;
    *device=(struct ehci_device){.parent=parent,.parent_port=parent_port,.depth=parent ? parent->depth+1 : 0,
        .usb={.operations=&ehci_device_operations,.host_context=controller,.owner=THIS_MODULE,
            .speed=speed,.ep0_max_packet=speed==USB_SPEED_HIGH ? 64 : 8,.root_port=root_port,
            .bus=controller->pci.bus,.slot=controller->pci.slot,.function=controller->pci.function}};
    if (speed!=USB_SPEED_HIGH && parent) {
        device->usb.hub_address=parent->usb.speed==USB_SPEED_HIGH ? parent->usb.address : parent->usb.hub_address;
        device->usb.hub_port=parent->usb.speed==USB_SPEED_HIGH ? parent_port : parent->usb.hub_port;
    }
    uint8_t descriptor[18]; uint32_t actual=0;
    int rc=ehci_request(&device->usb,0x80,6,0x100,0,descriptor,8,&actual);
    if (rc || actual!=8 || descriptor[0]!=18 || descriptor[1]!=1) { rc=rc ? rc : -EPROTO;goto fail; }
    unsigned packet=descriptor[7];
    if ((speed==USB_SPEED_HIGH && packet!=64) || (speed==USB_SPEED_LOW && packet!=8) ||
        (packet!=8 && packet!=16 && packet!=32 && packet!=64)) { rc=-EPROTO;goto fail; }
    device->usb.ep0_max_packet=packet;
    unsigned address=allocate_address(controller);
    if (!address) {rc=-ENOSPC;goto fail;}
    rc=ehci_request(&device->usb,0,5,address,0,NULL,0,&actual);
    if (rc) { controller->addresses[address/64]&=~(1ull<<(address%64));goto fail; }
    device->usb.address=address;
    ehci_delay(2);
    rc=ehci_request(&device->usb,0x80,6,0x100,0,descriptor,sizeof(descriptor),&actual);
    if (rc || actual!=sizeof(descriptor) || descriptor[0]!=18 || descriptor[1]!=1 || !descriptor[17]) {
        rc=rc ? rc : -EPROTO;goto fail;
    }
    device->usb.vendor=descriptor[8]|(uint16_t)descriptor[9]<<8;
    device->usb.product=descriptor[10]|(uint16_t)descriptor[11]<<8;
    bool hub=descriptor[4]==9;
    uint8_t *bytes=kernel_malloc(EHCI_MAX_CONFIG);
    struct usb_config *config=kernel_malloc(sizeof(*config));
    if (!bytes || !config) { kernel_free(bytes);kernel_free(config);rc=-ENOMEM;goto fail; }
    rc=ehci_request(&device->usb,0x80,6,0x200,0,bytes,9,&actual);
    if (!rc && actual!=9)rc=-EPROTO;
    unsigned total=!rc ? bytes[2]|(unsigned)bytes[3]<<8 : 0;
    if (!rc && (total<9 || total>EHCI_MAX_CONFIG))rc=-EOPNOTSUPP;
    if (!rc) rc=ehci_request(&device->usb,0x80,6,0x200,0,bytes,total,&actual);
    if (!rc && actual!=total)rc=-EPROTO;
    if (!rc) rc=usb_parse_configuration(bytes,total,speed,config);
    if (!rc) rc=ehci_request(&device->usb,0,9,config->value,0,NULL,0,&actual);
    kernel_free(bytes);
    if (rc) {kernel_free(config);goto fail;}
    device->next=controller->devices;controller->devices=device;
    console_info("[usb] EHCI device address=%u port=%u vid=%04x pid=%04x speed=%u\n",
        address,root_port+1,device->usb.vendor,device->usb.product,speed);
    for (unsigned i=0;i<config->count;++i) {
        const struct usb_config_interface *interface=&config->interfaces[i];
        int result=0;
        if (interface->class_code==3) result=ehci_hid_add(controller,device,interface);
        else if (interface->class_code==8 && interface->subclass==6 && interface->protocol==0x50 &&
                 interface->in.address && interface->out.address) {
            struct usb_storage **storage=&device->storage[device->storage_count++];
            result=usb_storage_probe(&device->usb,interface->number,interface->in,interface->out,storage);
        } else if (interface->class_code==9) hub=true;
        if (result) console_warn("[usb] EHCI class unavailable address=%u interface=%u class=%u error=%d\n",
            address,interface->number,interface->class_code,result);
        if (controller->failed)break;
    }
    kernel_free(config);
    if (hub && !controller->failed) {
        int result=ehci_hub_initialize(controller,device);
        if (result) console_warn("[usb] EHCI hub unavailable address=%u error=%d\n",address,result);
    }
    return controller->failed ? -EIO : 0;
fail:
    if (device->usb.address)
        controller->addresses[device->usb.address/64]&=~(1ull<<(device->usb.address%64));
    kernel_free(device);return rc;
}

/** @brief Revoke a removed device subtree without freeing DMA or active block state. */
void ehci_device_disconnect(struct ehci_controller *controller, struct ehci_device *device)
{
    if (__atomic_exchange_n(&device->usb.disconnected,true,__ATOMIC_ACQ_REL))return;
    if (device->usb.address)
        controller->addresses[device->usb.address/64]&=~(1ull<<(device->usb.address%64));
    for (struct ehci_device *child=controller->devices;child;child=child->next)
        if (child->parent==device)ehci_device_disconnect(controller,child);
}

/** @brief Retire unplugged class state only after its exact old disk users close. */
void ehci_devices_collect(struct ehci_controller *controller)
{
    if (ehci_hid_collect(controller))return;
    struct ehci_device **at=&controller->devices;
    while (*at) {
        struct ehci_device *device=*at;
        if (!device->usb.disconnected) {at=&device->next;continue;}
        bool busy=false;
        for (unsigned i=0;i<device->storage_count;++i)
            if (usb_storage_disconnect(device->storage[i]))busy=true;
        /* A retired hub must outlive every retained child's parent link. */
        for (struct ehci_device *child=controller->devices;child;child=child->next)
            if (child->parent==device)busy=true;
        if (busy) {at=&device->next;continue;}
        for (unsigned i=0;i<device->storage_count;++i)usb_storage_release(device->storage[i]);
        *at=device->next;kernel_free(device);
    }
}

/** @brief Release all class/input metadata after owner users and hardware are drained. */
void ehci_devices_release(struct ehci_controller *controller)
{
    for (unsigned i=0;i<EHCI_MAX_HID;++i)usb_hid_boot_release(&controller->hid[i].input);
    while (controller->devices) {
        struct ehci_device *device=controller->devices;controller->devices=device->next;
        device->usb.disconnected=true;
        for (unsigned i=0;i<device->storage_count;++i) {
            (void)usb_storage_disconnect(device->storage[i]);
            usb_storage_release(device->storage[i]);
        }
        kernel_free(device);
    }
}
