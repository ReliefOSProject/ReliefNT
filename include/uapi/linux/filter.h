/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI__LINUX_FILTER_H__
#define _UAPI__LINUX_FILTER_H__

#include <linux/types.h>
#include <linux/bpf_common.h>

#define BPF_MAJOR_VERSION 1
#define BPF_MINOR_VERSION 1
#define BPF_MEMWORDS 16
#define BPF_RVAL(code) ((code) & 0x18)
#define BPF_A        0x10
#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX      0x00
#define BPF_TXA      0x80

struct sock_filter {
	__u16 code;
	__u8 jt;
	__u8 jf;
	__u32 k;
};

struct sock_fprog {
	unsigned short len;
	struct sock_filter *filter;
};

#ifndef BPF_STMT
#define BPF_STMT(code, k) { (unsigned short)(code), 0, 0, k }
#endif
#ifndef BPF_JUMP
#define BPF_JUMP(code, k, jt, jf) { (unsigned short)(code), jt, jf, k }
#endif

#endif
