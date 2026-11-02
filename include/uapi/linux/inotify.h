/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_UAPI_LINUX_INOTIFY_H
#define RELIEFNT_UAPI_LINUX_INOTIFY_H

#include <stdint.h>
#include <linux/ioctl.h>

/** @brief Linux inotify event header followed by an optional padded basename. */
struct inotify_event {
    int32_t wd; /**< Watch descriptor, or -1 for queue overflow. */
    uint32_t mask; /**< Delivered event and directory bits. */
    uint32_t cookie; /**< Shared rename cookie, or zero. */
    uint32_t len; /**< Padded name bytes, including NUL when nonzero. */
    char name[]; /**< Optional NUL-terminated basename and zero padding. */
};

#define IN_ACCESS         0x00000001U
#define IN_MODIFY         0x00000002U
#define IN_ATTRIB         0x00000004U
#define IN_CLOSE_WRITE    0x00000008U
#define IN_CLOSE_NOWRITE  0x00000010U
#define IN_OPEN           0x00000020U
#define IN_MOVED_FROM     0x00000040U
#define IN_MOVED_TO       0x00000080U
#define IN_CREATE         0x00000100U
#define IN_DELETE         0x00000200U
#define IN_DELETE_SELF    0x00000400U
#define IN_MOVE_SELF      0x00000800U
#define IN_UNMOUNT        0x00002000U
#define IN_Q_OVERFLOW     0x00004000U
#define IN_IGNORED        0x00008000U
#define IN_ONLYDIR        0x01000000U
#define IN_DONT_FOLLOW    0x02000000U
#define IN_EXCL_UNLINK    0x04000000U
#define IN_MASK_CREATE    0x10000000U
#define IN_MASK_ADD       0x20000000U
#define IN_ISDIR          0x40000000U
#define IN_ONESHOT        0x80000000U
#define IN_CLOSE          (IN_CLOSE_WRITE | IN_CLOSE_NOWRITE)
#define IN_MOVE           (IN_MOVED_FROM | IN_MOVED_TO)
#define IN_ALL_EVENTS     0x00000fffU
#define IN_CLOEXEC        0x00080000U
#define IN_NONBLOCK       0x00000800U

/* The ioctl number is part of the Linux UAPI; implementation is optional. */
#define INOTIFY_IOC_SETNEXTWD _IOW('I', 0, int32_t)

#endif
