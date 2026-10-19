#include "drm_internal.h"

#include <reliefnt/heap.h>

#define DRM_VMWARE_VENDOR 0x15adu
#define DRM_VMWARE_SVGA2  0x0405u

static struct drm_device drm_primary;

struct drm_device *drm_device_get(void)
{
    return drm_primary.registered ? &drm_primary : NULL;
}

int drm_device_register(const struct pci_device *device)
{
    if (!device || device->vendor_id != DRM_VMWARE_VENDOR ||
        device->device_id != DRM_VMWARE_SVGA2)
        return -LINUX_EINVAL;
    if (drm_primary.registered) {
        if (drm_primary.pci.bus == device->bus &&
            drm_primary.pci.slot == device->slot &&
            drm_primary.pci.function == device->function)
            return 0;
        return -LINUX_EBUSY;
    }
    drm_primary = (struct drm_device){.pci = *device, .registered = true};
    return 0;
}

bool drm_device_is_registered(void)
{
    return drm_primary.registered;
}

bool drm_device_render_node_ready(void)
{
    struct svga_info info;
    return drm_primary.registered && drm_vmwgfx_vgpu9_ready(&info);
}

int drm_device_pci_identity(struct pci_device *out)
{
    if (!out || !drm_primary.registered) return -LINUX_ENODEV;
    *out = drm_primary.pci;
    return 0;
}

struct drm_master *drm_master_create(void)
{
    struct drm_master *master = kernel_malloc(sizeof(*master));
    if (!master) return NULL;
    *master = (struct drm_master){.references = 1, .next_magic = 1};
    return master;
}

void drm_master_get(struct drm_master *master)
{
    if (master) ++master->references;
}

void drm_master_put(struct drm_master *master)
{
    if (!master || !master->references || --master->references) return;
    while (master->magics) {
        struct drm_magic_entry *entry = master->magics;
        master->magics = entry->next;
        kernel_free(entry);
    }
    kernel_free(master);
}
