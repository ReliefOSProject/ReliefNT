/* SPDX-License-Identifier: Apache-2.0 */
#include "internal.h"
#include <reliefnt/console.h>
#include <reliefnt/time.h>
#include <reliefnt/deferred.h>
#include <reliefnt/module.h>

struct vg_device vg;

/** @brief Stop DMA and mark outstanding work failed without releasing callbacks.
 * @param error Negative request status. @return Zero or reset errno.
 * Task/execution ownership excludes submission; IRQ dispatch drains before reset.
 */
int vg_device_fault(int error)
{
    __atomic_store_n(&vg.fault,true,__ATOMIC_RELEASE);
    int rc=vg.transport.common ? virtio_pci_reset(&vg.transport) : 0;
    if(!rc)for(struct vg_request *r=vg.requests;r;r=r->next) {
        if(!__atomic_load_n(&r->done,__ATOMIC_ACQUIRE)) {
            r->result=error;__atomic_store_n(&r->done,true,__ATOMIC_RELEASE);
        }
    }
    return rc;
}

/** @brief Retain failed-init/module code until its hardware and IRQ really stop.
 * @param context Singleton retained by the cleanup ledger. @return Zero or errno.
 */
static int quiesce(void *context)
{ (void)context;return vg_device_fault(-EIO); }

/** @brief Release queue/MMIO storage only after the module ledger's STOP succeeds.
 * @param context Singleton already reset and quiescent, consumed idempotently.
 */
static void release(void *context)
{ (void)context;vg_device_stop(); }

/** @brief Coalesce graphics work after draining completions and config events.
 * @param context Retained singleton device; IRQ context, no allocation or sleep.
 */
static void interrupt(void *context)
{
    struct vg_device *device=context;
    vg_poll();
    if (*(volatile uint32_t *)(device->transport.config) & VIRTIO_GPU_EVENT_DISPLAY)
        __atomic_store_n(&device->display_changed,true,__ATOMIC_RELEASE);
    kernel_deferred_schedule(DEFERRED_GRAPHICS);
}

/** @brief Refresh host-advertised outputs and validated EDID blocks.
 * @return Zero or errno. Task context; no unvalidated EDID is published.
 */
int vg_display_refresh(void)
{
    struct virtio_gpu_ctrl_hdr command={.type=VIRTIO_GPU_CMD_GET_DISPLAY_INFO};
    struct virtio_gpu_resp_display_info display;
    int rc=vg_command(&command,sizeof(command),&display,sizeof(display),VIRTIO_GPU_RESP_OK_DISPLAY_INFO);
    if(rc)return rc;
    vg.display=display;
    if (vg.transport.features & (1ULL << VIRTIO_GPU_F_EDID)) {
        for(unsigned i=0;i<vg.scanout_count;++i) {
            vg.edid[i]=(struct virtio_gpu_resp_edid){0};
            if(!display.pmodes[i].enabled)continue;
            struct virtio_gpu_cmd_get_edid request={.hdr.type=VIRTIO_GPU_CMD_GET_EDID,.scanout=i};
            struct virtio_gpu_resp_edid edid;
            rc=vg_command(&request,sizeof(request),&edid,sizeof(edid),VIRTIO_GPU_RESP_OK_EDID);
            if(rc || edid.size<128 || edid.size>sizeof(edid.edid) || (edid.size&127)) continue;
            static const uint8_t magic[]={0,255,255,255,255,255,255,0};
            if(__builtin_memcmp(edid.edid,magic,sizeof(magic)))continue;
            bool valid=true;
            for(unsigned block=0;block<edid.size;block+=128) {
                uint8_t checksum=0;for(unsigned j=0;j<128;++j)checksum+=edid.edid[block+j];
                if(checksum)valid=false;
            }
            if(valid)vg.edid[i]=edid;
        }
    }
    *(volatile uint32_t *)(vg.transport.config+4)=VIRTIO_GPU_EVENT_DISPLAY;
    return 0;
}

/** @brief Initialize the first modern VirtIO GPU independently of boot VGA.
 * @param out Required copied PCI identity. @return Zero or initialization errno.
 * Task context; only negotiated VirGL and EDID features are exposed.
 */
int vg_device_start(struct pci_device *out)
{
    struct pci_device pci;
    int rc=pci_find_device(0x1af4,0x1050,&pci);
    if(rc)return -ENODEV;
    rc=module_add_resource(THIS_MODULE,quiesce,release,&vg);if(rc)return rc;
    rc=virtio_pci_open(&vg.transport,&pci,(1ULL<<VIRTIO_GPU_F_VIRGL)|(1ULL<<VIRTIO_GPU_F_EDID));
    if(rc)return rc;
    if(vg.transport.mappings[3].length<sizeof(struct virtio_gpu_config)){rc=-EINVAL;goto failed;}
    virtio_pci_interrupt_prepare(&vg.transport,interrupt,&vg);
    rc=virtio_pci_queue_create(&vg.transport,&vg.queues[0],0,256);if(rc)goto failed;
    rc=virtio_pci_queue_create(&vg.transport,&vg.queues[1],1,64);if(rc)goto failed;
    rc=vg_panic_start();if(rc)goto failed;
    rc=virtio_pci_ready(&vg.transport);if(rc)goto failed;
    vg.virgl=!!(vg.transport.features&(1ULL<<VIRTIO_GPU_F_VIRGL));
    vg.scanout_count=*(volatile uint32_t *)(vg.transport.config+8);
    if(vg.scanout_count>VIRTIO_GPU_MAX_SCANOUTS)vg.scanout_count=VIRTIO_GPU_MAX_SCANOUTS;
    unsigned count=*(volatile uint32_t *)(vg.transport.config+12);
    if(count>VG_MAX_CAPSETS)count=VG_MAX_CAPSETS;
    for(unsigned i=0;vg.virgl && i<count;++i) {
        struct virtio_gpu_get_capset_info command={.hdr.type=VIRTIO_GPU_CMD_GET_CAPSET_INFO,.capset_index=i};
        struct virtio_gpu_resp_capset_info info;
        rc=vg_command(&command,sizeof(command),&info,sizeof(info),VIRTIO_GPU_RESP_OK_CAPSET_INFO);
        if(rc)goto failed;
        if((info.capset_id!=VIRTIO_GPU_CAPSET_VIRGL && info.capset_id!=VIRTIO_GPU_CAPSET_VIRGL2) ||
            !info.capset_max_size || info.capset_max_size>1048576)continue;
        vg.capsets[vg.capset_count++]=(struct vg_capset){info.capset_id,info.capset_max_version,info.capset_max_size};
    }
    if(vg.virgl && !vg.capset_count){rc=-EOPNOTSUPP;goto failed;}
    rc=vg_display_refresh();if(rc)goto failed;
    rc=vg_console_start();if(rc)goto failed;
    vg.ready=true;*out=pci;
    console_info("[virtio-gpu] ready %02x:%02x.%u outputs=%u virgl=%u capsets=%u irq=%s\n",
        pci.bus,pci.slot,pci.function,vg.scanout_count,vg.virgl,vg.capset_count,
        vg.transport.irq_mode==1 ? "msix" : vg.transport.irq_mode==2 ? "msi" : "poll");
    kernel_deferred_schedule(DEFERRED_GRAPHICS);
    return 0;
failed:
    vg_device_stop();return rc;
}

/** @brief Reset hardware before consuming any remaining ring or DMA allocation.
 * Exit/failed-init task context; unresettable devices retain all allocations.
 */
void vg_device_stop(void)
{
    if(!vg.transport.common)return;
    if(vg_device_fault(-EIO))return;
    while(vg.requests) {
        struct vg_request *r=vg.requests;r->done=true;r->result=-EIO;
        if(r->complete)r->complete(r);vg_request_free(r);
    }
    vg_console_stop();
    vg_panic_stop();
    (void)virtio_pci_queue_destroy(&vg.transport,&vg.queues[1]);
    (void)virtio_pci_queue_destroy(&vg.transport,&vg.queues[0]);
    virtio_pci_close(&vg.transport);vg=(struct vg_device){0};
}

/** @brief Retire asynchronous submissions and service coalesced display changes.
 * @param budget Work target. @return Actual requests retired; task/execution
 * context, request-specific resource callbacks never run inside an IRQ lock.
 */
uint32_t vg_process_retirements(uint32_t budget)
{
    if(!vg.ready || vg.reaping)return 0;
    vg.reaping=true;
    vg_poll();unsigned count=0;
    uint64_t now=vg_clock_us();
    bool timed_out=false;
    for(struct vg_request *r=vg.requests;r;r=r->next)
        if(!__atomic_load_n(&r->done,__ATOMIC_ACQUIRE) && now-r->submitted_us>=3000000)timed_out=true;
    if(timed_out || (vg.fault && vg.transport.live))(void)vg_device_fault(-EIO);
    while(count<budget+2) {
        struct vg_request *r=vg.requests;
        while(r && (!r->automatic || !__atomic_load_n(&r->done,__ATOMIC_ACQUIRE)))r=r->next;
        if(!r)break;
        /* A final BO put may synchronously add and consume control requests.
         * Restart the search after each callback; never retain a list cursor
         * across resource teardown or a recursively completed host command. */
        if(r->complete)r->complete(r);
        vg_request_free(r);++count;
    }
    if(!vg.fault) {
        if(__atomic_exchange_n(&vg.display_changed,false,__ATOMIC_ACQ_REL)) (void)vg_display_refresh();
        (void)vg_console_update(false);
    }
    vg.reaping=false;return count;
}
