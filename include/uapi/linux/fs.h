#ifndef RELIEFOS_UAPI_LINUX_FS_H
#define RELIEFOS_UAPI_LINUX_FS_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Linux block-device ioctls supported by the ReliefOS block layer. */
#define BLKROSET      _IO(0x12, 93)
#define BLKROGET      _IO(0x12, 94)
#define BLKRRPART     _IO(0x12, 95)
#define BLKGETSIZE    _IO(0x12, 96)
#define BLKSSZGET     _IO(0x12, 104)
#define BLKGETSIZE64  _IOR(0x12, 114, __u64)

#endif
