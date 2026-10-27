/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include <reliefnt/drm.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/graphics_service.h>
#include <linux/errno.h>
static const struct graphics_service_operations drm_core_operations={
    .is_registered=drm_device_is_registered,.render_node_ready=drm_device_render_node_ready,
    .pci_identity=drm_device_pci_identity,.kms_scanout_active=drm_device_kms_scanout_active,
    .kms_vt_release=drm_device_kms_vt_release,.process_retirements=drm_vmwgfx_process_retirements,
    .cancel_task_continuation=drm_vmwgfx_cancel_task_continuation,
    .task_uninterruptible=drm_vmwgfx_task_uninterruptible,.task_signal_busy=drm_vmwgfx_task_signal_busy};
static const struct kernel_service_descriptor drm_core_provider={
    .operations=&drm_core_operations,.owner=THIS_MODULE};
/** @brief Discover the bound SVGA function and publish fully initialized DRM hooks.
 * @return Zero, including no matching framebuffer, or discovery/registry errno.
 * Init task context after core heap setup; no early graphics registration is
 * required. The immutable framebuffer identity is copied, never borrowed.
 */
int drm_core_services_start(void)
{
    struct pci_device device;int rc=framebuffer_vmware_pci_identity(&device);
    if(!rc){rc=drm_device_register(&device);if(rc)return rc;}
    else if(rc!=-ENODEV)return rc;
    drm_faildiag_configure(graphics_service_faildiag_enabled() ? "drm.faildiag=1" : NULL);
    return graphics_service_register(&drm_core_provider);
}
/** @brief Remove optional DRM hooks only after all external code users have drained.
 * Exit task context; cleanup-ledger detach may already have completed. No
 * callback or hardware operation occurs under the permanent service locks.
 */
void drm_core_services_stop(void)
{(void)graphics_service_unregister(&drm_core_provider);}
