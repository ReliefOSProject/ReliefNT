#ifndef RELIEFOS_UAPI_LINUX_UIO_H
#define RELIEFOS_UAPI_LINUX_UIO_H

#include <linux/types.h>

#define UIO_MAXIOV 1024
#define RWF_HIPRI 0x00000001u

struct iovec {
    void *iov_base;
    uint64_t iov_len;
};

#endif
