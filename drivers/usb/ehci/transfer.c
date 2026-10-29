/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"

/** @brief Translate only this controller's retained coherent DMA allocation. */
uint32_t ehci_bus(const struct ehci_controller *controller, const void *pointer)
{
    uintptr_t start = (uintptr_t)controller->dma, address = (uintptr_t)pointer;
    if (!start || address < start || address - start >= sizeof(*controller->dma) ||
        controller->dma_phys > UINT32_MAX - (address - start)) return 0;
    return controller->dma_phys + address - start;
}

/** @brief Fill an inactive qTD without exposing CPU pointers or crossing five pages. */
int ehci_qtd_prepare(struct ehci_controller *controller, struct ehci_qtd *td,
                      uint8_t pid, uint8_t toggle, void *buffer, uint32_t length)
{
    if (!td || pid > EHCI_PID_SETUP || length > 0x7fffu || toggle > 1) return -EINVAL;
    uint32_t address = length ? ehci_bus(controller, buffer) : 0;
    if (length && (!address || length > 5u * 4096u - (address & 4095u) ||
        !ehci_bus(controller, (uint8_t *)buffer + length - 1u))) return -EINVAL;
    *td = (struct ehci_qtd){.next = EHCI_TERMINATE, .alternate = EHCI_TERMINATE,
        .token = EHCI_TD_ACTIVE | 3u << 10 | (uint32_t)pid << 8 |
                 length << 16 | (uint32_t)toggle << 31};
    if (length) {
        td->buffer[0] = address;
        for (unsigned i = 1; i < 5 && i * 4096u < (address & 4095u) + length; ++i)
            td->buffer[i] = (address & ~4095u) + i * 4096u;
    }
    return 0;
}

/** @brief Prepare a detached QH with explicit data-toggle and split-transaction routing. */
void ehci_qh_prepare(struct ehci_qh *qh, const struct usb_device *device,
                     uint8_t endpoint, uint16_t packet, bool control)
{
    *qh = (struct ehci_qh){.link = EHCI_TERMINATE, .next = EHCI_TERMINATE,
        .alternate = EHCI_TERMINATE, .characteristics = device->address |
            (uint32_t)(endpoint & 15u) << 8 | (uint32_t)device->speed << 12 |
            1u << 14 | (uint32_t)packet << 16 | 4u << 28,
        .capabilities = 1u << 30 | (uint32_t)device->hub_address << 16 |
                        (uint32_t)device->hub_port << 23};
    if (control && device->speed != USB_SPEED_HIGH) qh->characteristics |= 1u << 27;
}

/** @brief Set controller-owned empty schedules before enabling hardware DMA. */
void ehci_schedule_init(struct ehci_controller *controller)
{
    struct ehci_dma *dma = controller->dma;
    struct usb_device idle = {.speed = USB_SPEED_HIGH};
    ehci_qh_prepare(&dma->head, &idle, 0, 64, false);
    dma->head.characteristics |= 1u << 15;
    dma->head.token = EHCI_TD_HALTED;
    dma->head.link = ehci_bus(controller, &dma->transfer) | EHCI_LINK_QH;
    ehci_qh_prepare(&dma->transfer, &idle, 0, 64, true);
    dma->transfer.link = ehci_bus(controller, &dma->head) | EHCI_LINK_QH;
    for (unsigned i = 0; i < EHCI_FRAME_COUNT; ++i) dma->frames[i] = EHCI_TERMINATE;
    ehci_write(controller, EHCI_SEGMENT, 0);
    ehci_write(controller, EHCI_PERIODIC, ehci_bus(controller, dma->frames));
    ehci_write(controller, EHCI_ASYNC, ehci_bus(controller, &dma->head));
    ehci_write(controller, EHCI_FRINDEX, 0);
}

/** @brief Change schedule enable only after observing the hardware handshake. */
int ehci_schedule_enable(struct ehci_controller *controller, bool periodic, bool enabled)
{
    uint32_t command = periodic ? EHCI_PSE : EHCI_ASE, status = periodic ? EHCI_PSS : EHCI_ASS;
    int rc = ehci_wait(controller, EHCI_STATUS, status,
        ehci_read(controller, EHCI_CMD) & command ? status : 0, 100);
    if (!rc) {
        uint32_t value = ehci_read(controller, EHCI_CMD);
        ehci_write(controller, EHCI_CMD, enabled ? value | command : value & ~command);
        rc = ehci_wait(controller, EHCI_STATUS, status, enabled ? status : 0, 100);
    }
    if (rc) __atomic_store_n(&controller->failed, true, __ATOMIC_RELEASE);
    return rc;
}

/** @brief Inspect all issued qTDs so an early STALL cannot wait for an unreachable tail. */
static int transfer_complete(struct ehci_dma *dma, unsigned count, bool control,
                              bool *complete, uint32_t *actual, uint8_t *toggle)
{
    *complete = false; *actual = 0;
    for (unsigned i = 0; i < count; ++i) {
        uint32_t token = dma->descriptors[i].token;
        bool failed=(token & (EHCI_TD_HALTED | EHCI_TD_ERRORS))!=0;
        if ((token & EHCI_TD_ACTIVE) && !failed) return 0;
        uint32_t requested = dma->descriptors[i].reserved[0];
        uint32_t remaining = token >> 16 & 0x7fffu;
        if (remaining > requested) return -EIO;
        if (!control || (i && i + 1 < count)) *actual += requested - remaining;
        *toggle = token >> 31;
        /* A halted qTD may have already completed packets. BOT uses that
         * exact extent to validate CSW residue and interpret short sense. */
        if (failed) return token & EHCI_TD_ERRORS ? -EIO : -EPIPE;
        if (remaining) {
            if (!control) { *complete = true; return 0; }
            /* A short control data qTD skips to the status stage. */
            i = count - 2;
        }
    }
    *complete = true;
    return 0;
}

/** @brief Submit one prepared async chain and stop the schedule before reuse/copy. */
static int submit(struct usb_device *device, unsigned count, bool control,
                    uint32_t *actual, uint8_t *toggle)
{
    struct ehci_controller *controller = device->host_context;
    struct ehci_dma *dma = controller->dma;
    for (unsigned i = 0; i < count; ++i) {
        dma->descriptors[i].reserved[0] = dma->descriptors[i].token >> 16 & 0x7fffu;
        dma->descriptors[i].next = i + 1 < count ? ehci_bus(controller, &dma->descriptors[i+1]) : EHCI_TERMINATE;
        if (control && i && i + 1 < count)
            dma->descriptors[i].alternate = ehci_bus(controller, &dma->descriptors[count-1]);
    }
    dma->transfer.next = ehci_bus(controller, dma->descriptors);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    int rc = ehci_schedule_enable(controller, false, true);
    struct ehci_timer timer;
    bool complete = false;
    if (!rc) {
        rc=ehci_timer_start(&timer);
    }
    if (!rc) {
        for (unsigned spin = 0; ; ++spin) {
            if (__atomic_load_n(&device->disconnected, __ATOMIC_ACQUIRE)) { rc = -ENODEV; break; }
            rc = transfer_complete(dma, count, control, &complete, actual, toggle);
            if (rc || complete) break;
            if (ehci_timer_expired(&timer,5000000u) || spin == 50000000u) { rc = -ETIMEDOUT; break; }
            __asm__ volatile("pause" ::: "memory");
        }
    }
    int stopped = ehci_schedule_enable(controller, false, false);
    if (stopped) return stopped;
    dma->transfer.next = EHCI_TERMINATE;
    dma->transfer.token = EHCI_TD_HALTED | (dma->transfer.token & 1u);
    return rc;
}

/** @brief Acquire a task transaction without waiting behind another CPU's USB command. */
static int acquire(struct usb_device *device)
{
    struct ehci_controller *controller = device->host_context;
    if (__atomic_load_n(&device->disconnected, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&controller->failed, __ATOMIC_ACQUIRE)) return -ENODEV;
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&controller->busy, &expected, 1, false,
        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED) ? 0 : -EAGAIN;
}

/** @brief Release the host gate after the complete class transaction has finished. */
static void release(struct usb_device *device)
{ __atomic_store_n(&((struct ehci_controller *)device->host_context)->busy, 0, __ATOMIC_RELEASE); }

static int control_transfer(struct usb_device *device, const uint8_t setup[8],
                             void *data, uint16_t length, uint32_t *actual);

/** @brief Find the high-speed, single-TT hub selected for a split route. */
static int tt_route(struct usb_device *device, struct ehci_device **out)
{
    *out=NULL;
    if(device->speed==USB_SPEED_HIGH)return 0;
    struct ehci_controller *controller=device->host_context;
    if(!device->hub_address || !device->hub_port)return -EOPNOTSUPP;
    for(struct ehci_device *hub=controller->devices;hub;hub=hub->next) {
        if(hub->usb.address!=device->hub_address || hub->usb.speed!=USB_SPEED_HIGH)continue;
        if(__atomic_load_n(&hub->usb.disconnected,__ATOMIC_ACQUIRE) ||
           !hub->hub_ports || device->hub_port>hub->hub_ports)return -ENODEV;
        *out=hub;return 0;
    }
    return -ENODEV;
}

/** @brief Drain saved nonperiodic TT buffer entries before accepting another split. */
static int tt_drain(struct ehci_device *hub)
{
    struct ehci_controller *controller=hub->usb.host_context;
    if(controller->failed || __atomic_load_n(&hub->usb.disconnected,__ATOMIC_ACQUIRE))return -ENODEV;
    while(hub->tt_pending) {
        uint8_t bit=hub->tt_pending&1u ? 1u : 2u;
        uint16_t value=hub->tt_value|(bit==2 ? 0x8000u : 0);
        /* Alternate setting zero selects the single-TT organization. */
        uint8_t setup[8]={0x23,8,(uint8_t)value,(uint8_t)(value>>8),1,0,0,0};
        uint32_t actual=0;
        int rc=control_transfer(&hub->usb,setup,NULL,0,&actual);
        if(rc || actual)return rc ? rc : -EPROTO;
        hub->tt_pending&=(uint8_t)~bit;
    }
    return 0;
}

/** @brief Resolve and, if needed, recover the TT before submitting a split. */
static int tt_prepare(struct usb_device *device, struct ehci_device **hub)
{
    int rc=tt_route(device,hub);
    if(rc || !*hub)return rc;
    rc=tt_drain(*hub);
    return rc ? (rc==-ENODEV ? rc : -EIO) : 0;
}

/** @brief Retire an interrupted split on the real hub before allowing reuse. */
static int tt_finish(struct usb_device *device, struct ehci_device *hub, int result,
                     uint8_t endpoint, bool control, bool input)
{
    /* A pure halted qTD with no error bits is a completed STALL response;
     * no split remains in the TT, and some deployed hubs mishandle clearing it. */
    if(!hub || !result || result==-EPIPE)return result;
    hub->tt_value=(endpoint&15u)|((uint16_t)device->address<<4)|
        (control ? 0u : 2u<<11);
    hub->tt_pending=control ? 3u : input ? 2u : 1u;
    if(((struct ehci_controller *)device->host_context)->failed)return result;
    int rc=tt_drain(hub);
    return rc ? (rc==-ENODEV ? rc : -EIO) : result;
}

/** @brief Run a bounded control request through exclusively owned bounce DMA. */
static int control_transfer(struct usb_device *device, const uint8_t setup[8],
                             void *data, uint16_t length, uint32_t *actual)
{
    struct ehci_controller *controller = device->host_context;
    if (!setup || !actual || length > EHCI_MAX_CONFIG || (length && !data) ||
        (setup[6] | (uint16_t)setup[7] << 8) != length) return -EINVAL;
    *actual = 0;
    if (__atomic_load_n(&device->disconnected, __ATOMIC_ACQUIRE) || controller->failed) return -ENODEV;
    struct ehci_device *tt=NULL;
    int rc=tt_prepare(device,&tt);if(rc)return rc;
    rc = ehci_schedule_enable(controller, false, false); if (rc) return rc;
    struct ehci_dma *dma = controller->dma;
    ehci_qh_prepare(&dma->transfer, device, 0, device->ep0_max_packet, true);
    dma->transfer.link = ehci_bus(controller, &dma->head) | EHCI_LINK_QH;
    __builtin_memcpy(dma->setup, setup, 8);
    rc = ehci_qtd_prepare(controller, &dma->descriptors[0], EHCI_PID_SETUP, 0, dma->setup, 8);
    unsigned count = 1;
    if (!rc && length) {
        if (!(setup[0] & 0x80u)) __builtin_memcpy(dma->data, data, length);
        rc = ehci_qtd_prepare(controller, &dma->descriptors[count++],
            setup[0] & 0x80u ? EHCI_PID_IN : EHCI_PID_OUT, 1, dma->data, length);
    }
    if (!rc) rc = ehci_qtd_prepare(controller, &dma->descriptors[count++],
        setup[0] & 0x80u ? EHCI_PID_OUT : EHCI_PID_IN, 1, NULL, 0);
    uint8_t toggle = 0; bool submitted=false;
    if (!rc) { submitted=true;rc = submit(device, count, true, actual, &toggle); }
    if (!controller->failed && (setup[0] & 0x80u) && *actual)
        __builtin_memcpy(data, dma->data, *actual);
    return submitted ? tt_finish(device,tt,rc,0,true,(setup[0]&0x80u)!=0) : rc;
}

/** @brief Transfer a bounded bulk extent, preserving endpoint toggles and short INs. */
static int bulk_transfer(struct usb_device *device, struct usb_endpoint *endpoint,
                          void *data, uint32_t length, uint32_t *actual)
{
    struct ehci_controller *controller = device->host_context;
    if (!endpoint || !actual || length > EHCI_TRANSFER_BYTES || (length && !data) ||
        !(endpoint->address & 15u) || !endpoint->max_packet) return -EINVAL;
    *actual = 0;
    if (__atomic_load_n(&device->disconnected, __ATOMIC_ACQUIRE) || controller->failed) return -ENODEV;
    struct ehci_device *tt=NULL;
    int rc=tt_prepare(device,&tt);if(rc)return rc;
    rc = ehci_schedule_enable(controller, false, false); if (rc) return rc;
    struct ehci_dma *dma = controller->dma;
    bool in = (endpoint->address & 0x80u) != 0;
    ehci_qh_prepare(&dma->transfer, device, endpoint->address, endpoint->max_packet, false);
    if (!in && device->speed==USB_SPEED_HIGH) dma->transfer.token=endpoint->ping & 1u;
    dma->transfer.link = ehci_bus(controller, &dma->head) | EHCI_LINK_QH;
    if (!in && length) __builtin_memcpy(dma->data, data, length);
    uint32_t offset = 0; unsigned count = 0; uint8_t toggle = endpoint->toggle;
    do {
        uint32_t bytes = length - offset; if (bytes > 16384) bytes = 16384;
        rc = ehci_qtd_prepare(controller, &dma->descriptors[count++],
            in ? EHCI_PID_IN : EHCI_PID_OUT, toggle, dma->data + offset, bytes);
        if (rc) return rc;
        toggle ^= ((bytes + endpoint->max_packet - 1u) / endpoint->max_packet) & 1u;
        offset += bytes;
    } while (offset < length);
    rc = submit(device, count, false, actual, &toggle);
    if (!controller->failed && in && *actual) __builtin_memcpy(data, dma->data, *actual);
    if (!rc) {
        endpoint->toggle = toggle;
        endpoint->ping = dma->transfer.token & 1u;
        if (!in && *actual != length) rc=-EIO;
    }
    return tt_finish(device,tt,rc,endpoint->address,false,in);
}

/** @brief Clear ENDPOINT_HALT and reset DATA0 only after the control request succeeds. */
static int clear_halt(struct usb_device *device, struct usb_endpoint *endpoint)
{
    uint8_t setup[8] = {2, 1, 0, 0, endpoint->address, 0, 0, 0};
    uint32_t actual; int rc = control_transfer(device, setup, NULL, 0, &actual);
    if (!rc) { endpoint->toggle = 0; endpoint->ping = 0; }
    return rc;
}

const struct usb_device_operations ehci_device_operations = {
    .acquire = acquire, .release = release, .control = control_transfer,
    .bulk = bulk_transfer, .clear_halt = clear_halt,
};
