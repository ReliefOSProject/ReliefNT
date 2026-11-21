/* SPDX-License-Identifier: Apache-2.0 */
#include "services.h"
#include "drm_internal.h"
#include <reliefnt/graphics_service.h>
#include <reliefnt/moduleparam.h>

#ifdef CONFIG_DRM_VIRTIO_GPU
extern const struct drm_driver vg_driver;
#endif
#ifdef CONFIG_DRM_VMWARE_SVGA
extern const struct drm_driver svga_driver;
#endif

static const struct drm_driver *const drivers[] = {
#ifdef CONFIG_DRM_VIRTIO_GPU
    &vg_driver,
#endif
#ifdef CONFIG_DRM_VMWARE_SVGA
    &svga_driver,
#endif
    NULL,
};

static const struct drm_driver *primary_driver(void)
{
    struct drm_device *device = drm_device_get();
    return device ? device->driver : NULL;
}

static uint32_t process_retirements(uint32_t budget)
{
    const struct drm_driver *driver = primary_driver();
    return driver && driver->retire ? driver->retire(budget) : 0;
}

static int connector_info(uint32_t index, struct graphics_connector_info *out)
{
    const struct drm_driver *driver = primary_driver();
    return driver && driver->connector_info ? driver->connector_info(index, out) : -ENODEV;
}

static void panic_prepare(void)
{
    const struct drm_driver *driver = primary_driver();
    if (driver && driver->panic_prepare)
        driver->panic_prepare();
}

static void panic_damage(uint32_t x, uint32_t y, uint32_t width, uint32_t height)
{
    const struct drm_driver *driver = primary_driver();
    if (driver && driver->panic_damage)
        driver->panic_damage(x, y, width, height);
}

static void panic_present(void)
{
    const struct drm_driver *driver = primary_driver();
    if (driver && driver->panic_present)
        driver->panic_present();
}

/* Readonly boot parameter, parsed identically in y and m builds. */
static bool faildiag;
module_param(faildiag, bool, 0400);
MODULE_PARM_DESC(faildiag, "Enable bounded DRM failure diagnostics at initialization");

static const struct graphics_service_operations drm_core_operations = {
    .is_registered = drm_device_is_registered, .render_node_ready = drm_device_render_node_ready,
    .pci_identity = drm_device_pci_identity, .kms_scanout_active = drm_device_kms_scanout_active,
    .kms_vt_release = drm_device_kms_vt_release, .process_retirements = process_retirements,
    .panic_prepare = panic_prepare, .panic_damage = panic_damage, .panic_present = panic_present,
    .connector_info = connector_info,
};
static const struct kernel_service_descriptor drm_core_provider = {
    .operations = &drm_core_operations, .owner = THIS_MODULE,
};

static void stop_primary(void)
{
    const struct drm_driver *driver = primary_driver();
    if (driver) {
        if (driver->kms_vt_release)
            driver->kms_vt_release();
        driver->stop();
        drm_device_unregister();
    }
}

int drm_core_services_start(void)
{
    for (unsigned i = 0; drivers[i]; ++i) {
        struct pci_device device;
        int ret = drivers[i]->start(&device);
        if (ret == -ENODEV)
            continue;
        if (ret)
            return ret;
        ret = drm_device_register(&device, drivers[i]);
        if (ret) {
            drivers[i]->stop();
            return ret;
        }
        break;
    }

    drm_faildiag_configure(faildiag ? "drm.faildiag=1" : NULL);
    int ret = graphics_service_register(&drm_core_provider);
    if (ret)
        stop_primary();
    return ret;
}

void drm_core_services_stop(void)
{
    (void)graphics_service_unregister(&drm_core_provider);
    stop_primary();
}
