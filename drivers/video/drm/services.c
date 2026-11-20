/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include <reliefnt/drm.h>
#include <reliefnt/graphics_service.h>
#include <reliefnt/moduleparam.h>
#include <linux/errno.h>
#ifdef CONFIG_DRM_VIRTIO_GPU
#include "virtio/internal.h"
#endif

#ifdef CONFIG_DRM_VIRTIO_GPU
static uint32_t process_retirements(uint32_t budget)
{
    return drm_is_virtio() ? vg_process_retirements(budget) : 0;
}

static int connector_info(uint32_t index, struct graphics_connector_info *out)
{
    return drm_is_virtio() ? vg_connector_info(index, out) : -ENODEV;
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
    .kms_vt_release=drm_device_kms_vt_release,
#ifdef CONFIG_DRM_VIRTIO_GPU
    .process_retirements=process_retirements,
    .panic_prepare=vg_panic_prepare,.panic_damage=vg_panic_damage,.panic_present=vg_panic_present,
    .connector_info=connector_info,
#endif
};
static const struct kernel_service_descriptor drm_core_provider={
    .operations=&drm_core_operations,.owner=THIS_MODULE};
/** @brief Discover supported graphics hardware and publish the DRM service.
 * @return Zero when no supported device is present, or a discovery/registry error.
 * Called in init task context after heap setup and parameter parsing.
 */
int drm_core_services_start(void)
{
    int rc;
#ifdef CONFIG_DRM_VIRTIO_GPU
    struct pci_device device;
    rc = vg_device_start(&device);
    if (rc && rc != -ENODEV) return rc;
    if (!rc) {
        rc = drm_device_register(&device);
        if (rc) {
            vg_device_stop();
            return rc;
        }
    }
#endif
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
