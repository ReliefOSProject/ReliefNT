/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFOS_UAPI_LINUX_PIDFD_H
#define RELIEFOS_UAPI_LINUX_PIDFD_H

#include <linux/fcntl.h>

#define PIDFD_NONBLOCK LINUX_O_NONBLOCK
#define PIDFD_THREAD   LINUX_O_EXCL

#define PIDFD_SIGNAL_THREAD       (1U << 0)
#define PIDFD_SIGNAL_THREAD_GROUP (1U << 1)

#endif
