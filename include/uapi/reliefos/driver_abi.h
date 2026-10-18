#ifndef RELIEFOS_UAPI_DRIVER_ABI_H
#define RELIEFOS_UAPI_DRIVER_ABI_H
/*
 * Driver-control wire ABI between ReliefNT and userland (/dev/driverctl).
 * Userland wrappers live in <reliefos/driver.h>; the Ring-0 module API stays
 * there too until it moves to its own domain.
 * UAPI only: nothing here may include a non-UAPI header.
 */

#include <stdint.h>


/* Native ioctl on /dev/driverctl, fixed-size reliefos_driver_control argument. */
#define RELIEFOS_DRIVER_CONTROL_IOCTL 0xc0504c64U
#define RELIEFOS_DRIVER_FILE_LEN 64U
#define RELIEFOS_DRIVER_NAME_LEN 32U
#define RELIEFOS_DRIVER_ERROR_LEN 96U

#define RELIEFOS_DRIVER_STATE_UNLOADED 0U
#define RELIEFOS_DRIVER_STATE_LOADING 1U
#define RELIEFOS_DRIVER_STATE_LOADED 2U
#define RELIEFOS_DRIVER_STATE_DISABLED 3U
#define RELIEFOS_DRIVER_STATE_FAILED 4U

/* Stable driver categories returned by struct reliefos_driver_info.kind. */
#define RELIEFOS_DRIVER_KIND_INPUT 1U
#define RELIEFOS_DRIVER_KIND_SERIAL 2U
#define RELIEFOS_DRIVER_KIND_NETWORK 3U
#define RELIEFOS_DRIVER_KIND_AUDIO 4U

#define RELIEFOS_DRIVER_FLAG_AUTOSTART 0x00000001U
#define RELIEFOS_DRIVER_FLAG_DISABLED 0x00000002U
#define RELIEFOS_DRIVER_FLAG_BUILTIN 0x00000004U

#define RELIEFOS_DRIVER_CONTROL_LOAD 1U
#define RELIEFOS_DRIVER_CONTROL_UNLOAD 2U
#define RELIEFOS_DRIVER_CONTROL_FORCE_UNLOAD 3U
#define RELIEFOS_DRIVER_CONTROL_RESCAN 4U
#define RELIEFOS_DRIVER_CONTROL_ENABLE_BOOT 5U
#define RELIEFOS_DRIVER_CONTROL_DISABLE_BOOT 6U

struct reliefos_driver_info {
    uint32_t id;
    uint32_t state;
    uint32_t kind;
    uint32_t flags;
    uint32_t abi_version;
    uint32_t version;
    uint64_t load_address;
    uint64_t image_size;
    char file[RELIEFOS_DRIVER_FILE_LEN];
    char name[RELIEFOS_DRIVER_NAME_LEN];
    char error[RELIEFOS_DRIVER_ERROR_LEN];
};

struct reliefos_driver_list {
    uint32_t capacity;
    uint32_t count;
    struct reliefos_driver_info *drivers;
};

struct reliefos_driver_control {
    uint32_t action;
    uint32_t flags;
    int32_t status;
    uint32_t reserved;
    char file[RELIEFOS_DRIVER_FILE_LEN];
};

#endif /* RELIEFOS_UAPI_DRIVER_ABI_H */
