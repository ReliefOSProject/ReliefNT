/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_UAPI_LINUX_WAIT_H
#define RELIEFOS_UAPI_LINUX_WAIT_H

/* Native waitid selectors, prefixed so this header can coexist with libc. */
#define LINUX_P_ALL   0
#define LINUX_P_PID   1
#define LINUX_P_PGID  2
#define LINUX_P_PIDFD 3

#define LINUX_WNOHANG     0x00000001u
#define LINUX_WSTOPPED    0x00000002u
#define LINUX_WEXITED     0x00000004u
#define LINUX_WCONTINUED  0x00000008u
#define LINUX_WNOWAIT     0x01000000u
#define LINUX_WNOTHREAD   0x20000000u
#define LINUX_WALL        0x40000000u
#define LINUX_WCLONE      0x80000000u

#endif
