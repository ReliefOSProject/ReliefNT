/* Read-only Linux sysfs inventory. Attributes describe measured hardware and
 * actual kernel objects; no writable control files or DRM ioctls are implied. */
#include <reliefos/fs_abi.h>
#include <reliefnt/framebuffer.h>
#include <reliefnt/inventory.h>
#include <reliefnt/pci.h>
#include <reliefnt/platform.h>
#include <reliefnt/smp.h>
#include <reliefnt/storage.h>
#include <reliefnt/text_stream.h>

#define SYSFS_PCI_MAX 256u
static struct pci_device pci_devices[SYSFS_PCI_MAX];
static uint32_t pci_count;
static int pci_initialized;
static int pci_error;
static const char *dmi_fields[] = {"bios_vendor",   "bios_version",    "bios_date",      "sys_vendor",
                                   "product_name",  "product_version", "product_serial", "product_uuid",
                                   "product_sku",   "product_family",  "board_vendor",   "board_name",
                                   "board_version", "board_serial",    "chassis_vendor"};
enum attribute {
    A_DIR,
    A_LINK,
    A_DMI,
    A_CPUSET,
    A_CPU_ONLINE,
    A_PACKAGE,
    A_CORE,
    A_SIBLINGS,
    A_PACKAGE_LIST,
    A_PCI_VENDOR,
    A_PCI_DEVICE,
    A_PCI_CLASS,
    A_PCI_REV,
    A_PCI_SUBVENDOR,
    A_PCI_SUBDEVICE,
    A_PCI_ALIAS,
    A_PCI_CONFIG,
    A_FB_NAME,
    A_FB_SIZE,
    A_FB_BPP,
    A_FB_STRIDE,
    A_FB_MODES,
    A_CONNECTED,
    A_ENABLED,
    A_PCI_UEVENT,
    A_BLOCK_NUMBER,
    A_BLOCK_TEXT,
    A_BLOCK_FS_TEXT
};
struct sys_node {
    const char *path;
    enum attribute attr;
    uint32_t index;
    const char *target;
    uint64_t value;
};
typedef int (*sys_visit)(const struct sys_node *, void *);
/** @brief Cache PCI identities once, under the caller's kernel execution lock. */
static int sys_pci_init(void)
{
    if (pci_initialized)
        return pci_error;
    pci_initialized = 1;
    for (uint32_t bus = 0; bus < 256; ++bus)
        for (uint32_t slot = 0; slot < 32; ++slot) {
            struct pci_device device;
            if (pci_read_device(bus, slot, 0, &device) < 0)
                continue;
            uint32_t functions = (pci_config_read32(bus, slot, 0, 0x0c) & 0x00800000) ? 8 : 1;
            for (uint32_t fn = 0; fn < functions; ++fn) {
                if (pci_read_device(bus, slot, fn, &device) < 0)
                    continue;
                if (pci_count == SYSFS_PCI_MAX)
                    return pci_error = -28;
                pci_devices[pci_count++] = device;
            }
        }
    return 0;
}
/** @brief Build a bounded path from a prefix, optional decimal index and suffix. */
static void sys_path(char out[RELIEFOS_FS_PATH_LEN], const char *prefix, int index, const char *suffix)
{
    struct text_stream s = {.buffer = out, .capacity = RELIEFOS_FS_PATH_LEN - 1};
    text_string(&s, prefix);
    if (index >= 0)
        text_unsigned(&s, (uint32_t)index);
    text_string(&s, suffix);
    out[s.written] = 0;
}
/** @brief Format the Linux domain:bus:slot.function identity of an enumerated device. */
static void sys_pci_path(char out[RELIEFOS_FS_PATH_LEN], const char *prefix, uint32_t index,
                         const char *suffix)
{
    const struct pci_device *p = &pci_devices[index];
    struct text_stream s = {.buffer = out, .capacity = RELIEFOS_FS_PATH_LEN - 1};
    text_string(&s, prefix);
    text_string(&s, "0000:");
    text_hex(&s, p->bus, 2);
    text_string(&s, ":");
    text_hex(&s, p->slot, 2);
    text_string(&s, ".");
    text_hex(&s, p->function, 1);
    text_string(&s, suffix);
    out[s.written] = 0;
}
/** @brief Visit one generated node, propagating an early match or visitor error. */
static int sys_emit(sys_visit visit, void *ctx, const char *path, enum attribute attr, uint32_t index,
                    const char *target)
{
    struct sys_node n = {.path=path, .attr=attr, .index=index, .target=target};
    return visit(&n, ctx);
}
static int sys_block_name(char *out, uint32_t disk, int32_t part)
{
    return storage_disk_device_name(disk, part, out, RELIEFOS_FS_PATH_LEN);
}

/* Filesystem metadata is deliberately exposed through read-only sysfs instead
 * of relaxing raw block-device reads for unprivileged processes.  The offsets
 * match the Linux ext2/ext3/ext4 superblock ABI. */
static uint16_t sys_fs_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static uint32_t sys_fs_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void sys_fs_uuid(char out[37], const uint8_t *uuid)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t at = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            out[at++] = '-';
        out[at++] = hex[uuid[i] >> 4];
        out[at++] = hex[uuid[i] & 15];
    }
    out[at] = 0;
}

static int sys_block_fs_info(uint32_t disk, int32_t part, char type[8], char version[8],
                             char uuid[37], char label[17])
{
    uint8_t super[1024];
    uint32_t got = 0;
    int ret = storage_disk_block_read(disk, part, 1024, super, sizeof(super), &got);
    if (ret < 0 || got < 1024 || sys_fs_le16(super + 56) != 0xef53)
        return 0;
    uint32_t compat = sys_fs_le32(super + 92);
    uint32_t incompat = sys_fs_le32(super + 96);
    const char *name = (incompat & 0x40u) ? "ext4" : (compat & 0x4u) ? "ext3" : "ext2";
    __builtin_memcpy(type, name, __builtin_strlen(name) + 1);
    __builtin_memcpy(version, "1.0", 4);
    bool nonzero = false;
    for (uint32_t i = 0; i < 16; ++i)
        nonzero |= super[104 + i] != 0;
    if (nonzero)
        sys_fs_uuid(uuid, super + 104);
    else
        uuid[0] = 0;
    uint32_t n = 0;
    while (n < 16 && super[120 + n]) {
        uint8_t c = super[120 + n];
        label[n++] = c >= 32 && c <= 126 ? (char)c : '?';
    }
    label[n] = 0;
    return 1;
}

/* Use the same registered disks/GPT extents and dev_t encoding as devfs. */
static int sys_blocks(sys_visit visit, void *ctx)
{
    struct reliefos_install_disk disks[RELIEFOS_INSTALL_MAX_DISKS];
    uint32_t count = 0;
    int ret = storage_install_list_disks(disks, RELIEFOS_INSTALL_MAX_DISKS, &count);
    if (ret < 0) return ret;
    if (count > RELIEFOS_INSTALL_MAX_DISKS) return -5;
    for (uint32_t i = 0; i < count; ++i) {
        char diskpath[RELIEFOS_FS_PATH_LEN];
        char diskname[RELIEFOS_FS_PATH_LEN];
        if (sys_block_name(diskname, disks[i].id, -1) < 0) return -2;
        sys_path(diskpath, "/sys/devices/platform/leonos-block/", -1, diskname + 5);
        for (int32_t part = -1; part < (int32_t)RELIEFOS_DISK_MAX_PARTITIONS; ++part) {
            uint64_t start = 0, sectors = disks[i].sector_count;
            if (part >= 0) {
                ret = storage_disk_block_info(disks[i].id, part, &start, &sectors);
                if (ret == -2 || ret == -22) continue; /* No GPT entry/table. */
                if (ret < 0) return ret;
            }
            char name[RELIEFOS_FS_PATH_LEN], base[RELIEFOS_FS_PATH_LEN];
            char path[RELIEFOS_FS_PATH_LEN], target[RELIEFOS_FS_PATH_LEN], number[64], event[256];
            if (sys_block_name(name, disks[i].id, part) < 0) return -2;
            const char *devname = name + 5; /* Skip the /dev/ prefix. */
            sys_path(base, diskpath, -1, part < 0 ? "" : "/");
            if (part >= 0) {
                sys_path(path, base, -1, devname);
                sys_path(base, path, -1, "");
            }
            uint32_t minor = STORAGE_BLOCK_MINOR(STORAGE_BLOCK_VOLUME_ID(disks[i].id, part));
            struct text_stream s = {.buffer=number, .capacity=sizeof(number)-1};
            text_unsigned(&s, STORAGE_BLOCK_MAJOR); text_string(&s, ":"); text_unsigned(&s, minor);
            number[s.written] = 0;
            s = (struct text_stream){.buffer=event, .capacity=sizeof(event)-1};
            text_string(&s, "MAJOR="); text_unsigned(&s, STORAGE_BLOCK_MAJOR);
            text_string(&s, "\nMINOR="); text_unsigned(&s, minor);
            text_string(&s, "\nDEVNAME="); text_string(&s, devname);
            text_string(&s, "\nDEVTYPE="); text_string(&s, part < 0 ? "disk" : "partition");
            if (part >= 0) { text_string(&s, "\nPARTN="); text_unsigned(&s, part + 1); }
            event[s.written] = 0;
            const char *suffix[] = {"", "/holders", "/slaves", "/dev", "/size", "/uevent",
                                   "/start", "/partition", "/queue", "/queue/logical_block_size",
                                   "/fs_type", "/fs_version", "/fs_uuid", "/fs_label"};
            const enum attribute kind[] = {A_DIR,A_DIR,A_DIR,A_BLOCK_TEXT,A_BLOCK_NUMBER,A_BLOCK_TEXT,
                                           A_BLOCK_NUMBER,A_BLOCK_NUMBER,A_DIR,A_BLOCK_NUMBER,
                                           A_BLOCK_FS_TEXT,A_BLOCK_FS_TEXT,A_BLOCK_FS_TEXT,A_BLOCK_FS_TEXT};
            char fs_type[8], fs_version[8], fs_uuid[37], fs_label[17];
            int has_fs = sys_block_fs_info(disks[i].id, part, fs_type, fs_version, fs_uuid, fs_label);
            for (uint32_t j=0; j<sizeof(kind)/sizeof(kind[0]); ++j) {
                if ((part < 0 && (j == 6 || j == 7)) || (part >= 0 && (j == 8 || j == 9))) continue;
                if (j >= 10 && !has_fs) continue;
                sys_path(path, base, -1, suffix[j]);
                const char *fs_target = j == 10 ? fs_type : j == 11 ? fs_version :
                                        j == 12 ? fs_uuid : fs_label;
                struct sys_node n = {.path=path,.attr=kind[j],
                    .target=j >= 10 ? fs_target : j == 3 ? number : event,
                    .value=j == 4 ? sectors * (disks[i].sector_size / 512u) :
                           j == 6 ? start * (disks[i].sector_size / 512u) :
                           j == 7 ? (uint32_t)part + 1 : disks[i].sector_size};
                if ((ret = visit(&n, ctx))) return ret;
            }
            sys_path(target, "../../", -1, base + 5);
            sys_path(path, "/sys/dev/block/", -1, number);
            if ((ret=sys_emit(visit,ctx,path,A_LINK,0,target))) return ret;
            sys_path(path, "/sys/class/block/", -1, devname);
            if ((ret=sys_emit(visit,ctx,path,A_LINK,0,target))) return ret;
            if (part < 0) {
                sys_path(target, "../", -1, base + 5);
                sys_path(path, "/sys/block/", -1, devname);
                if ((ret=sys_emit(visit,ctx,path,A_LINK,0,target))) return ret;
            }
        }
    }
    return 0;
}
/** @brief Enumerate the same namespace used by lookup/readlink/readdir. */
static int sys_walk(sys_visit visit, void *ctx)
{
    static const char *dirs[] = {"/sys",
                                 "/sys/dev",
                                 "/sys/dev/block",
                                 "/sys/block",
                                 "/sys/class/block",
                                 "/sys/devices",
                                 "/sys/devices/platform",
                                 "/sys/devices/platform/leonos-block",
                                 "/sys/devices/system",
                                 "/sys/devices/system/cpu",
                                 "/sys/devices/virtual",
                                 "/sys/devices/virtual/dmi",
                                 "/sys/devices/virtual/dmi/id",
                                 "/sys/devices/pci0000:00",
                                 "/sys/bus",
                                 "/sys/bus/pci",
                                 "/sys/bus/pci/devices",
                                 "/sys/class",
                                 "/sys/class/dmi",
                                 "/sys/class/graphics",
                                 "/sys/class/drm"};
    int ret;
    for (uint32_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        if ((ret = sys_emit(visit, ctx, dirs[i], A_DIR, 0, 0)))
            return ret;
    if ((ret = sys_emit(visit, ctx, "/sys/class/dmi/id", A_LINK, 0, "../../devices/virtual/dmi/id")))
        return ret;
    const struct platform_dmi_info *dmi = platform_dmi();
    char path[RELIEFOS_FS_PATH_LEN], target[RELIEFOS_FS_PATH_LEN];
    for (uint32_t i = 0; i < PLATFORM_DMI_FIELDS; ++i)
        if (dmi->values[i][0]) {
            sys_path(path, "/sys/devices/virtual/dmi/id/", -1, dmi_fields[i]);
            if ((ret = sys_emit(visit, ctx, path, A_DMI, i, 0)))
                return ret;
        }
    const char *sets[] = {"online", "offline", "present", "possible"};
    for (uint32_t i = 0; i < 4; ++i) {
        sys_path(path, "/sys/devices/system/cpu/", -1, sets[i]);
        if ((ret = sys_emit(visit, ctx, path, A_CPUSET, i, 0)))
            return ret;
    }
    for (uint32_t cpu = 0; cpu < smp_cpu_count(); ++cpu) {
        const char *suffixes[] = {"",
                                  "/online",
                                  "/topology",
                                  "/topology/physical_package_id",
                                  "/topology/core_id",
                                  "/topology/thread_siblings_list",
                                  "/topology/core_cpus_list",
                                  "/topology/core_siblings_list",
                                  "/topology/package_cpus_list"};
        const enum attribute attrs[] = {A_DIR,      A_CPU_ONLINE, A_DIR,          A_PACKAGE,     A_CORE,
                                        A_SIBLINGS, A_SIBLINGS,   A_PACKAGE_LIST, A_PACKAGE_LIST};
        for (uint32_t j = 0; j < sizeof(attrs) / sizeof(attrs[0]); ++j) {
            if (j >= 3 && !cpu_inventory_get(cpu))
                continue;
            sys_path(path, "/sys/devices/system/cpu/cpu", cpu, suffixes[j]);
            if ((ret = sys_emit(visit, ctx, path, attrs[j], cpu, 0)))
                return ret;
        }
    }
    ret = sys_pci_init();
    if (ret < 0)
        return ret;
    static const char *attrs[] = {"",
                                  "/vendor",
                                  "/device",
                                  "/class",
                                  "/revision",
                                  "/subsystem_vendor",
                                  "/subsystem_device",
                                  "/modalias",
                                  "/config",
                                  "/uevent"};
    static const enum attribute kinds[] = {A_DIR,        A_PCI_VENDOR,    A_PCI_DEVICE,    A_PCI_CLASS,
                                           A_PCI_REV,    A_PCI_SUBVENDOR, A_PCI_SUBDEVICE, A_PCI_ALIAS,
                                           A_PCI_CONFIG, A_PCI_UEVENT};
    for (uint32_t i = 0; i < pci_count; ++i) {
        sys_pci_path(path, "/sys/bus/pci/devices/", i, "");
        sys_pci_path(target, "../../../devices/pci0000:00/", i, "");
        if ((ret = sys_emit(visit, ctx, path, A_LINK, i, target)))
            return ret;
        for (uint32_t j = 0; j < sizeof(kinds) / sizeof(kinds[0]); ++j) {
            sys_pci_path(path, "/sys/devices/pci0000:00/", i, attrs[j]);
            if ((ret = sys_emit(visit, ctx, path, kinds[j], i, 0)))
                return ret;
        }
    }
    const struct framebuffer *fb = framebuffer_get();
    if (fb && fb->available) {
        /* A firmware/native scanout is a real display object. Expose its one
         * known current mode; never fabricate EDID, refresh or physical size. */
        const char *paths[] = {"/sys/devices/virtual/graphics",
                               "/sys/devices/virtual/graphics/fb0",
                               "/sys/devices/virtual/graphics/fb0/name",
                               "/sys/devices/virtual/graphics/fb0/virtual_size",
                               "/sys/devices/virtual/graphics/fb0/bits_per_pixel",
                               "/sys/devices/virtual/graphics/fb0/stride",
                               "/sys/devices/virtual/drm",
                               "/sys/devices/virtual/drm/card0",
                               "/sys/devices/virtual/drm/card0/card0-Unknown-1",
                               "/sys/devices/virtual/drm/card0/card0-Unknown-1/status",
                               "/sys/devices/virtual/drm/card0/card0-Unknown-1/enabled",
                               "/sys/devices/virtual/drm/card0/card0-Unknown-1/modes"};
        const enum attribute kinds2[] = {A_DIR, A_DIR, A_FB_NAME, A_FB_SIZE,   A_FB_BPP,  A_FB_STRIDE,
                                         A_DIR, A_DIR, A_DIR,     A_CONNECTED, A_ENABLED, A_FB_MODES};
        for (uint32_t i = 0; i < sizeof(kinds2) / sizeof(kinds2[0]); ++i)
            if ((ret = sys_emit(visit, ctx, paths[i], kinds2[i], 0, 0)))
                return ret;
        if ((ret = sys_emit(visit, ctx, "/sys/class/graphics/fb0", A_LINK, 0,
                            "../../devices/virtual/graphics/fb0")))
            return ret;
        /* The standard Linux PCI graphics mapping: fbdev helpers resolve the
         * framebuffer through its owning display controller. */
        for (uint32_t i = 0; i < pci_count; ++i) {
            if (pci_devices[i].class_code != 0x03) continue;
            sys_pci_path(path, "/sys/devices/pci0000:00/", i, "/graphics");
            if ((ret = sys_emit(visit, ctx, path, A_DIR, i, 0))) return ret;
            sys_pci_path(path, "/sys/devices/pci0000:00/", i, "/graphics/fb0");
            if ((ret = sys_emit(visit, ctx, path, A_LINK, 0, "../../../virtual/graphics/fb0")))
                return ret;
            sys_pci_path(path, "/sys/bus/pci/devices/", i, "/graphics");
            if ((ret = sys_emit(visit, ctx, path, A_DIR, i, 0))) return ret;
            sys_pci_path(path, "/sys/bus/pci/devices/", i, "/graphics/fb0");
            if ((ret = sys_emit(visit, ctx, path, A_LINK, 0, "../../../../virtual/graphics/fb0")))
                return ret;
            sys_pci_path(path, "/sys/bus/pci/devices/", i, "/graphics:fb0");
            if ((ret = sys_emit(visit, ctx, path, A_LINK, 0, "../../../../virtual/graphics/fb0")))
                return ret;
            sys_pci_path(path, "../../devices/pci0000:00/", i, "");
            if ((ret = sys_emit(visit, ctx, "/sys/class/graphics/fb0/device", A_LINK, i, path)))
                return ret;
            if ((ret = sys_emit(visit, ctx, "/sys/class/graphics/fb0/device/subsystem", A_LINK, 0,
                                "../../../../bus/pci")))
                return ret;
            break;
        }
        if ((ret = sys_emit(visit, ctx, "/sys/class/drm/card0", A_LINK, 0,
                            "../../devices/virtual/drm/card0")))
            return ret;
        if ((ret = sys_emit(visit, ctx, "/sys/class/drm/card0-Unknown-1", A_LINK, 0,
                            "../../devices/virtual/drm/card0/card0-Unknown-1")))
            return ret;
    }
    return sys_blocks(visit, ctx);
}
/** @brief Read a PCI config register for a known node. */
static uint32_t sys_pci_config(const struct sys_node *n, uint8_t offset)
{
    const struct pci_device *p = &pci_devices[n->index];
    return pci_config_read32(p->bus, p->slot, p->function, offset);
}
/** @brief Read subsystem IDs from the correct PCI header or bridge SSVID capability. */
static uint32_t sys_pci_subsystem(const struct sys_node *n)
{
    uint32_t header = (sys_pci_config(n, 0x0c) >> 16) & 0x7f;
    if (header == 0)
        return sys_pci_config(n, 0x2c);
    if (header == 2)
        return sys_pci_config(n, 0x40);
    if (header == 1 && (sys_pci_config(n, 4) & 0x00100000)) {
        uint8_t next = sys_pci_config(n, 0x34) & 0xfc;
        for (unsigned count = 0; next >= 0x40 && next <= 0xf8 && count < 48; ++count) {
            uint32_t cap = sys_pci_config(n, next);
            if ((cap & 255) == 0x0d)
                return sys_pci_config(n, next + 4);
            next = (cap >> 8) & 0xfc;
        }
    }
    return 0;
}
/** @brief Generate one attribute from real hardware/kernel state; no fixed whole-file buffer. */
static void sys_value(const struct sys_node *n, struct text_stream *s)
{
    const struct cpu_inventory *cpu = cpu_inventory_get(n->index);
    const struct pci_device *pci = &pci_devices[n->index < SYSFS_PCI_MAX ? n->index : 0];
    const struct framebuffer *fb = framebuffer_get();
    switch (n->attr) {
    case A_DMI:
        text_string(s, platform_dmi()->values[n->index]);
        break;
    case A_CPUSET: {
        bool first = true;
        for (uint32_t i = 0; i < smp_cpu_count(); ++i) {
            if (n->index < 2 && smp_cpu_online(i) != (n->index == 0))
                continue;
            if (!first)
                text_string(s, ",");
            text_unsigned(s, i);
            first = false;
        }
        break;
    }
    case A_CPU_ONLINE:
        text_unsigned(s, smp_cpu_online(n->index));
        break;
    case A_PACKAGE:
        text_unsigned(s, cpu->package_id);
        break;
    case A_CORE:
        text_unsigned(s, cpu->core_id);
        break;
    case A_SIBLINGS:
    case A_PACKAGE_LIST: {
        bool first = true;
        for (uint32_t i = 0; i < smp_cpu_count(); ++i) {
            const struct cpu_inventory *other = cpu_inventory_get(i);
            if (!other || !smp_cpu_online(i) || cpu->package_id != other->package_id ||
                (n->attr == A_SIBLINGS && cpu->core_id != other->core_id))
                continue;
            if (!first)
                text_string(s, ",");
            text_unsigned(s, i);
            first = false;
        }
        break;
    }
    case A_PCI_VENDOR:
        text_string(s, "0x");
        text_hex(s, pci->vendor_id, 4);
        break;
    case A_PCI_DEVICE:
        text_string(s, "0x");
        text_hex(s, pci->device_id, 4);
        break;
    case A_PCI_CLASS:
        text_string(s, "0x");
        text_hex(s, (pci->class_code << 16) | (pci->subclass << 8) | pci->prog_if, 6);
        break;
    case A_PCI_REV:
        text_string(s, "0x");
        text_hex(s, pci->revision, 2);
        break;
    case A_PCI_SUBVENDOR:
    case A_PCI_SUBDEVICE: {
        uint32_t v = sys_pci_subsystem(n);
        text_string(s, "0x");
        text_hex(s, n->attr == A_PCI_SUBVENDOR ? v & 65535 : v >> 16, 4);
        break;
    }
    case A_PCI_ALIAS: {
        uint32_t sub = sys_pci_subsystem(n);
        text_string(s, "pci:v");
        text_hex(s, pci->vendor_id, 8);
        text_string(s, "d");
        text_hex(s, pci->device_id, 8);
        text_string(s, "sv");
        text_hex(s, sub & 65535, 8);
        text_string(s, "sd");
        text_hex(s, sub >> 16, 8);
        text_string(s, "bc");
        text_hex(s, pci->class_code, 2);
        text_string(s, "sc");
        text_hex(s, pci->subclass, 2);
        text_string(s, "i");
        text_hex(s, pci->prog_if, 2);
        break;
    }
    case A_PCI_CONFIG:
        for (uint32_t i = 0; i < 256; i += 4) {
            uint32_t value = sys_pci_config(n, (uint8_t)i);
            text_bytes(s, &value, 4);
        }
        return;
    case A_PCI_UEVENT:
        text_string(s, "PCI_CLASS=");
        text_hex(s, (pci->class_code << 16) | (pci->subclass << 8) | pci->prog_if, 6);
        text_string(s, "\nPCI_ID=");
        text_hex(s, pci->vendor_id, 4);
        text_string(s, ":");
        text_hex(s, pci->device_id, 4);
        break;
    case A_FB_NAME:
        text_string(s, fb->backend == FRAMEBUFFER_BACKEND_VMWARE_SVGA ? "vmware-svga"
                       : fb->backend == FRAMEBUFFER_BACKEND_BOCHS_VBE ? "bochs-vbe"
                                                                      : "firmware-framebuffer");
        break;
    case A_FB_SIZE:
        text_unsigned(s, fb->width);
        text_string(s, ",");
        text_unsigned(s, fb->height);
        break;
    case A_FB_BPP:
        text_unsigned(s, fb->bpp);
        break;
    case A_FB_STRIDE:
        text_unsigned(s, fb->pitch);
        break;
    case A_FB_MODES:
        text_unsigned(s, fb->width);
        text_string(s, "x");
        text_unsigned(s, fb->height);
        break;
    case A_CONNECTED:
        text_string(s, "connected");
        break;
    case A_ENABLED:
        text_string(s, "enabled");
        break;
    case A_BLOCK_NUMBER:
        text_unsigned(s, n->value);
        break;
    case A_BLOCK_TEXT:
        text_string(s, n->target);
        break;
    case A_BLOCK_FS_TEXT:
        text_string(s, n->target);
        break;
    default:
        return;
    }
    text_string(s, "\n");
}
struct sys_request {
    const char *path;
    struct storage_node *node;
    struct text_stream stream;
    bool read, link;
};
/** @brief Match a pathname and perform lookup/read/readlink on that exact object. */
static int sys_match(const struct sys_node *n, void *context)
{
    struct sys_request *r = context;
    if (__builtin_strcmp(n->path, r->path))
        return 0;
    uint32_t type = n->attr == A_DIR    ? RELIEFOS_FS_TYPE_DIR
                    : n->attr == A_LINK ? RELIEFOS_FS_TYPE_SYMLINK
                                        : RELIEFOS_FS_TYPE_FILE;
    if (r->node)
        *r->node = (struct storage_node){.type = type,
                                         .flags = STORAGE_NODE_FLAG_PROC | STORAGE_NODE_FLAG_SYSFS,
                                         .size = n->attr == A_PCI_CONFIG       ? 256
                                                 : type == RELIEFOS_FS_TYPE_FILE ? 4096
                                                 : n->attr == A_LINK ? __builtin_strlen(n->target)
                                                                     : 0};
    if (r->link) {
        if (n->attr != A_LINK)
            return -22;
        text_string(&r->stream, n->target);
    } else if (r->read) {
        if (n->attr == A_DIR)
            return -21;
        if (n->attr == A_LINK)
            return -22;
        sys_value(n, &r->stream);
    }
    return 1;
}
/** @brief Resolve a generated sysfs node without following its final link. */
int sysfs_lookup(const char *path, struct storage_node *out)
{
    struct sys_request r = {.path = path, .node = out};
    int ret = sys_walk(sys_match, &r);
    return ret == 1 ? 0 : ret < 0 ? ret : -2;
}
/** @brief Read a bounded attribute range; caller holds the kernel execution lock. */
int sysfs_read(const char *path, uint64_t offset, void *buffer, uint32_t length, uint32_t *out_read)
{
    if (out_read)
        *out_read = 0;
    if (!buffer && length)
        return -22;
    struct sys_request r = {
        .path = path, .read = true, .stream = {.offset = offset, .buffer = buffer, .capacity = length}};
    int ret = sys_walk(sys_match, &r);
    if (out_read && ret == 1)
        *out_read = r.stream.written;
    return ret == 1 ? 0 : ret < 0 ? ret : -2;
}
/** @brief Return truncated symlink bytes, without terminating NUL. */
int sysfs_readlink(const char *path, char *buffer, uint32_t capacity)
{
    if (!buffer || !capacity)
        return -22;
    struct sys_request r = {
        .path = path, .link = true, .stream = {.buffer = buffer, .capacity = capacity}};
    int ret = sys_walk(sys_match, &r);
    return ret == 1 ? (int)r.stream.written : ret < 0 ? ret : -2;
}
struct sys_directory {
    const char *path;
    uint32_t length;
    uint64_t skip;
    struct reliefos_dir_entry *entry;
};
/** @brief Select one immediate child, excluding grandchildren and the parent itself. */
static int sys_child(const struct sys_node *n, void *context)
{
    struct sys_directory *d = context;
    if (__builtin_strncmp(n->path, d->path, d->length) || n->path[d->length] != '/')
        return 0;
    const char *name = n->path + d->length + 1;
    if (!*name)
        return 0;
    for (const char *p = name; *p; ++p)
        if (*p == '/')
            return 0;
    if (d->skip) {
        --d->skip;
        return 0;
    }
    *d->entry = (struct reliefos_dir_entry){.type = n->attr == A_DIR    ? RELIEFOS_FS_TYPE_DIR
                                                  : n->attr == A_LINK ? RELIEFOS_FS_TYPE_SYMLINK
                                                                      : RELIEFOS_FS_TYPE_FILE};
    __builtin_memcpy(d->entry->name, name, __builtin_strlen(name) + 1);
    return 1;
}
/** @brief Enumerate exactly the same immediate children visible to lookup. */
int sysfs_readdir(const char *path, uint64_t *offset, struct reliefos_dir_entry *entry)
{
    if (!path || !offset || !entry)
        return -22;
    struct storage_node node;
    int ret = sysfs_lookup(path, &node);
    if (ret < 0)
        return ret;
    if (node.type != RELIEFOS_FS_TYPE_DIR)
        return -20;
    struct sys_directory d = {path, (uint32_t)__builtin_strlen(path), *offset, entry};
    ret = sys_walk(sys_child, &d);
    if (ret == 1)
        ++*offset;
    return ret;
}
