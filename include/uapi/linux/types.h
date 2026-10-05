#ifndef RELIEFOS_UAPI_LINUX_TYPES_H
#define RELIEFOS_UAPI_LINUX_TYPES_H

/* Linux UAPI scalar types used by the supported x86_64 ABI subset.  Keep
 * these definitions independent from libc so kernel and userland consume
 * identical layouts. */
#include <stdint.h>

/*
 * This is the ReliefOS export equivalent of the Linux v6.14 UAPI scalar
 * definitions used by the audio ABI.  Keep it libc-independent so a header
 * consumer sees the same widths during headers_install and host tests.
 */

typedef int8_t __s8;
typedef uint8_t __u8;
typedef int16_t __s16;
typedef uint16_t __u16;
typedef int32_t __s32;
typedef uint32_t __u32;
typedef int64_t __s64;
typedef uint64_t __u64;

typedef __s8 s8;
typedef __u8 u8;
typedef __s16 s16;
typedef __u16 u16;
typedef __s32 s32;
typedef __u32 u32;
typedef __s64 s64;
typedef __u64 u64;

/* Sparse annotations are intentionally empty in the exported C ABI. */
#ifndef __bitwise
#define __bitwise
#endif
#ifndef __force
#define __force
#endif
#ifndef __user
#define __user
#endif
#ifndef __packed
#define __packed __attribute__((packed))
#endif

typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;
typedef __u64 __le64;
typedef __u64 __be64;

typedef __s64 __kernel_long_t;
typedef __u64 __kernel_ulong_t;
typedef __s64 __kernel_time_t;
typedef __s64 __kernel_off_t;
typedef __s32 __kernel_pid_t;
/* Canonical LP64 SysV IPC dependencies; also needed in freestanding builds
 * where the UAPI asm/posix_types.h suppresses its userspace selector. */
#include <asm/posix_types_64.h>
#include <linux/posix_types.h>
typedef __u64 __aligned_u64 __attribute__((aligned(8)));
typedef __s64 __aligned_s64 __attribute__((aligned(8)));

#define __LITTLE_ENDIAN 1234
#define __BIG_ENDIAN 4321
#define __PDP_ENDIAN 3412
#ifndef __BYTE_ORDER
#define __BYTE_ORDER __LITTLE_ENDIAN
#endif

#endif
