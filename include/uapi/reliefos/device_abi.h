#ifndef RELIEFOS_UAPI_DEVICE_ABI_H
#define RELIEFOS_UAPI_DEVICE_ABI_H
/*
 * Device wire ABI between ReliefNT and userland. Userland wrappers live in
 * <reliefos/device.h>.
 * UAPI only: nothing here may include a non-UAPI header.
 */

#include <stdint.h>


#define RELIEFOS_DEVICE_MAX 24U
#define RELIEFOS_DEVICE_NAME_LEN 32U
#define RELIEFOS_DEVICE_STATUS_LEN 32U
#define RELIEFOS_DEVICE_DETAIL_LEN 96U

#define RELIEFOS_DEVICE_CLASS_SYSTEM 1U
#define RELIEFOS_DEVICE_CLASS_INPUT 2U
#define RELIEFOS_DEVICE_CLASS_DISPLAY 3U
#define RELIEFOS_DEVICE_CLASS_STORAGE 4U
#define RELIEFOS_DEVICE_CLASS_SERIAL 5U
#define RELIEFOS_DEVICE_CLASS_NETWORK 6U
#define RELIEFOS_DEVICE_CLASS_AUDIO 7U

#define RELIEFOS_DEVICE_FLAG_PRESENT 0x00000001U
#define RELIEFOS_DEVICE_FLAG_ACTIVE 0x00000002U
#define RELIEFOS_DEVICE_FLAG_BOOT 0x00000004U
#define RELIEFOS_DEVICE_FLAG_REMOVABLE 0x00000008U

/* Canonical devfs paths. The historical fd 3 control descriptor no longer
 * exists; applications must open the device node they intend to use. */
#define RELIEFOS_DEV_NULL "/dev/null"
#define RELIEFOS_DEV_ZERO "/dev/zero"
#define RELIEFOS_DEV_FULL "/dev/full"
#define RELIEFOS_DEV_RANDOM "/dev/random"
#define RELIEFOS_DEV_URANDOM "/dev/urandom"
#define RELIEFOS_DEV_TTY "/dev/tty"
#define RELIEFOS_DEV_CONSOLE "/dev/console"
#define RELIEFOS_DEV_PTMX "/dev/ptmx"
#define RELIEFOS_DEV_FB0 "/dev/fb0"
#define RELIEFOS_DEV_INPUT_EVENT0 "/dev/input/event0"
#define RELIEFOS_DEV_INPUT_EVENT1 "/dev/input/event1"
/* Query uint64 display invalidation generation on a fixed VT descriptor. */
#define RELIEFOS_VT_GETGENERATION 0x800856f0UL

/* evdev extension: uint32_t input, 0 for raw events, 1-6 for events that
 * originated while that VT was graphical. The setting belongs to the open
 * file description and applies to read and poll. */
#define RELIEFOS_EVIOCSVT 0x400445f0UL
/* Linux OSS PCM playback device. */
#define RELIEFOS_DEV_DSP "/dev/dsp"
#define RELIEFOS_DEV_SERIAL0 "/dev/serial0"
#define RELIEFOS_DEV_SDA "/dev/sda"
#define RELIEFOS_DEV_NVME0N1 "/dev/nvme0n1"
/* Reserved legacy path. */
#define RELIEFOS_DEV_GPU "/dev/gpu"
#define RELIEFOS_DEV_SHM0 "/dev/shm0"

struct reliefos_device_info {
    uint32_t id;
    uint32_t device_class;
    uint32_t flags;
    uint32_t reserved;
    uint64_t value0;
    uint64_t value1;
    char name[RELIEFOS_DEVICE_NAME_LEN];
    char status[RELIEFOS_DEVICE_STATUS_LEN];
    char detail[RELIEFOS_DEVICE_DETAIL_LEN];
};

struct reliefos_device_list {
    uint32_t capacity;
    uint32_t count;
    struct reliefos_device_info *devices;
};

#endif /* RELIEFOS_UAPI_DEVICE_ABI_H */
