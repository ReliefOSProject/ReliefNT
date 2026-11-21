#include "drm_internal.h"

#include <reliefnt/heap.h>
#include <reliefnt/netlink.h>
#include <reliefnt/text_stream.h>

static struct drm_device drm_primary;

struct drm_device *drm_device_get(void)
{
    return drm_primary.registered ? &drm_primary : NULL;
}

int drm_device_register(const struct pci_device *device, const struct drm_driver *driver)
{
    if (!device || !driver) return -LINUX_EINVAL;
    if (drm_primary.registered) {
        if (drm_primary.driver == driver && drm_primary.pci.bus == device->bus &&
            drm_primary.pci.slot == device->slot &&
            drm_primary.pci.function == device->function)
            return 0;
        return -LINUX_EBUSY;
    }
    drm_primary = (struct drm_device){.pci = *device, .driver = driver, .registered = true};
    return 0;
}

/* Files, mappings and asynchronous services have drained before detach. */
void drm_device_unregister(void)
{
    drm_primary = (struct drm_device){0};
}

bool drm_device_is_registered(void)
{
    return drm_primary.registered;
}

bool drm_device_render_node_ready(void)
{
    struct drm_device *device = drm_device_get();
    return device && device->driver->render_ready && device->driver->render_ready();
}

int drm_device_pci_identity(struct pci_device *out)
{
    if (!out || !drm_primary.registered) return -LINUX_ENODEV;
    *out = drm_primary.pci;
    return 0;
}

/* The primary device's canonical DEVPATH follows the PCI sysfs layout. */
void drm_device_hotplug(const struct drm_device *device)
{
    char path[sizeof("/devices/pci0000:00/0000:00:00.0/drm/card0")];
    struct text_stream stream = {.buffer = path, .capacity = sizeof(path) - 1};
    text_string(&stream, "/devices/pci0000:00/0000:");
    text_hex(&stream, device->pci.bus, 2);
    text_string(&stream, ":");
    text_hex(&stream, device->pci.slot, 2);
    text_string(&stream, ".");
    text_hex(&stream, device->pci.function, 1);
    text_string(&stream, "/drm/card0");
    path[stream.written] = 0;
    static const char properties[] = "HOTPLUG=1\0DEVNAME=dri/card0\0DEVTYPE=drm_minor\0MAJOR=226\0MINOR=0";
    netlink_uevent_change(path, "drm", properties, sizeof(properties));
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
