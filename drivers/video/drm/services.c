/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include <reliefnt/drm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/graphics_service.h>
#include <reliefnt/moduleparam.h>
#include <linux/errno.h>
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#endif

/** @brief Dispatch bounded retirement work to the registered hardware backend.
 * @param budget Work limit. @return Retired count; task/execution context.
 */
static uint32_t process_retirements(uint32_t budget)
{
#ifdef CONFIG_DRM_VIRTIO_GPU
    if(drm_is_virtio())return vg_process_retirements(budget);
#endif
    return drm_vmwgfx_process_retirements(budget);
}
#ifdef CONFIG_DRM_VIRTIO_GPU
/** @brief Copy connector metadata for VirtIO, retaining the generic SVGA contract.
 * @param index Output number. @param out Copied destination. @return Zero or errno.
 * Task/execution context, no borrowed pointer survives provider admission.
 */
static int connector_info(uint32_t index,struct graphics_connector_info *out)
{
    if(drm_is_virtio())return vg_connector_info(index,out);
    if(index)return -ENOENT;
    const struct framebuffer *fb=framebuffer_get();
    *out=(struct graphics_connector_info){.connected=fb->available,
        .enabled=drm_device_kms_scanout_active(),.mode_count=fb->available ? 1 : 0};
    out->widths[0]=fb->width;out->heights[0]=fb->height;return 0;
}
#endif
/* A boot-only, readonly parameter uses the same descriptor in y and m builds.
 * modprobe forwards drm.faildiag from /proc/cmdline to an external drm.ko. */
static bool faildiag;
module_param(faildiag, bool, 0400);
MODULE_PARM_DESC(faildiag, "Enable bounded DRM failure diagnostics at initialization");
static const struct graphics_service_operations drm_core_operations={
    .is_registered=drm_device_is_registered,.render_node_ready=drm_device_render_node_ready,
    .pci_identity=drm_device_pci_identity,.kms_scanout_active=drm_device_kms_scanout_active,
    .kms_vt_release=drm_device_kms_vt_release,.process_retirements=process_retirements,
    .cancel_task_continuation=drm_vmwgfx_cancel_task_continuation,
    .task_uninterruptible=drm_vmwgfx_task_uninterruptible,.task_signal_busy=drm_vmwgfx_task_signal_busy,
#ifdef CONFIG_DRM_VIRTIO_GPU
    .panic_prepare=vg_panic_prepare,.panic_damage=vg_panic_damage,.panic_present=vg_panic_present,
    .connector_info=connector_info,
#endif
};
static const struct kernel_service_descriptor drm_core_provider={
    .operations=&drm_core_operations,.owner=THIS_MODULE};
/** @brief Discover the bound SVGA function and publish fully initialized DRM hooks.
 * @return Zero, including no matching framebuffer, or discovery/registry errno.
 * Init task context after core heap setup; no early graphics registration is
 * required. The immutable framebuffer identity is copied, never borrowed.
 * The shared parameter framework has applied the boot/module arguments before
 * this call. The readonly faildiag value initializes bounded logging once.
 */
int drm_core_services_start(void)
{
    struct pci_device device;
    int rc=-ENODEV;
#ifdef CONFIG_DRM_VIRTIO_GPU
    rc=vg_device_start(&device);
    if(rc && rc!=-ENODEV)return rc;
#endif
    if(rc==-ENODEV)rc=framebuffer_vmware_pci_identity(&device);
    if(!rc){rc=drm_device_register(&device);if(rc)return rc;}
    else if(rc!=-ENODEV)return rc;
    drm_faildiag_configure(faildiag ? "drm.faildiag=1" : NULL);
    rc=graphics_service_register(&drm_core_provider);
#ifdef CONFIG_DRM_VIRTIO_GPU
    if(rc)vg_device_stop();
#endif
    return rc;
}
/** @brief Remove optional DRM hooks only after all external code users have drained.
 * Exit task context; cleanup-ledger detach may already have completed. No
 * callback or hardware operation occurs under the permanent service locks.
 */
void drm_core_services_stop(void)
{
    (void)graphics_service_unregister(&drm_core_provider);
#ifdef CONFIG_DRM_VIRTIO_GPU
    vg_kms_vt_release();vg_device_stop();
#endif
}
