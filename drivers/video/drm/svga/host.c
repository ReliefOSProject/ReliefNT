/* SPDX-License-Identifier: MIT */
/*
 * Copyright 2016 VMware, Inc., Palo Alto, CA., USA
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDERS, AUTHORS AND/OR ITS SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "host.h"
#include <reliefnt/heap.h>
#include <reliefnt/paging.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/time.h>

#define VMWARE_INFO_LEAF 0x40000000u
#define VMWARE_FEATURES_LEAF 0x40000010u
#define VMWARE_MAGIC 0x564d5868u
#define VMWARE_PORT 0x5658u
#define VMWARE_GET_VERSION 10u
#define VMWARE_MESSAGE 30u
#define VMWARE_MKS_GUEST_STATS 85u
#define VMWARE_RPCI_COOKIE 0xc9435052u

#define MESSAGE_SUCCESS 1u
#define MESSAGE_HAS_REPLY 2u
#define MESSAGE_CHECKPOINT 16u
#define MESSAGE_RETRIES 3u
#define MESSAGE_MAX_SEND SVGA_HOST_PAGE_SIZE
#define MESSAGE_MAX_REPLY 65536u
#define MESSAGE_TIMEOUT_US 500000u

enum message_operation {
    MESSAGE_OPEN, MESSAGE_SEND_SIZE, MESSAGE_SEND_WORD, MESSAGE_RECEIVE_SIZE,
    MESSAGE_RECEIVE_WORD, MESSAGE_RECEIVE_STATUS, MESSAGE_CLOSE,
};

struct host_channel {
    uint32_t id, cookie_high, cookie_low;
};

struct host_regs {
    uint64_t ax, bx, cx, dx, si, di;
};

static enum { HOST_NONE, HOST_PORT, HOST_VMCALL, HOST_VMMCALL } host_transport;

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

static struct host_regs host_call(uint32_t command, uint64_t argument, uint16_t channel,
                                  uint32_t cookie_high, uint32_t cookie_low)
{
    struct host_regs regs = {
        .ax = VMWARE_MAGIC, .bx = argument, .cx = command,
        .dx = (uint32_t)channel << 16 | 2u, .si = cookie_high, .di = cookie_low,
    };
    switch (host_transport) {
    case HOST_VMCALL:
        __asm__ volatile("vmcall" : "+a"(regs.ax), "+b"(regs.bx), "+c"(regs.cx), "+d"(regs.dx),
                         "+S"(regs.si), "+D"(regs.di) :: "cc", "memory");
        break;
    case HOST_VMMCALL:
        __asm__ volatile("vmmcall" : "+a"(regs.ax), "+b"(regs.bx), "+c"(regs.cx), "+d"(regs.dx),
                         "+S"(regs.si), "+D"(regs.di) :: "cc", "memory");
        break;
    case HOST_PORT:
        regs.dx = (regs.dx & ~UINT64_C(0xffff)) | VMWARE_PORT;
        __asm__ volatile("inl %%dx, %%eax" : "+a"(regs.ax), "+b"(regs.bx), "+c"(regs.cx),
                         "+d"(regs.dx), "+S"(regs.si), "+D"(regs.di) :: "cc", "memory");
        break;
    default:
        regs.ax = UINT32_MAX;
        break;
    }
    return regs;
}

void svga_host_init(void)
{
    host_transport = HOST_NONE;
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & (1u << 31)))
        return;
    cpuid(VMWARE_INFO_LEAF, &a, &b, &c, &d);
    const uint32_t vendor[] = {b, c, d};
    if (__builtin_memcmp(vendor, "VMwareVMware", sizeof(vendor)))
        return;

    host_transport = HOST_PORT;
    if (a >= VMWARE_FEATURES_LEAF) {
        cpuid(VMWARE_FEATURES_LEAF, &a, &b, &c, &d);
        if ((c & 3u) == 2u)
            host_transport = HOST_VMCALL;
        else if ((c & 3u) == 1u)
            host_transport = HOST_VMMCALL;
    }
    const struct host_regs version = host_call(VMWARE_GET_VERSION, 0, 0, 0, 0);
    if ((uint32_t)version.ax == UINT32_MAX || (uint32_t)version.bx != VMWARE_MAGIC)
        host_transport = HOST_NONE;
}

void svga_host_fini(void)
{
    host_transport = HOST_NONE;
}

bool svga_host_available(void)
{
    return host_transport != HOST_NONE;
}

void svga_host_stats(enum svga_host_stats_op op, uint64_t ppn)
{
    if (svga_host_available())
        (void)host_call((uint32_t)op << 16 | VMWARE_MKS_GUEST_STATS, ppn, 0, 0, 0);
}

static struct host_regs message_call(enum message_operation op, uint32_t argument,
                                     const struct host_channel *channel)
{
    return host_call((uint32_t)op << 16 | VMWARE_MESSAGE, argument, channel->id,
                     channel->cookie_high, channel->cookie_low);
}

int svga_host_copy_string(struct task *task, uint64_t address, char *str, uint32_t capacity)
{
    uint32_t offset = 0;

    while (offset < capacity) {
        if (address < RELIEFNT_USER_BASE || address >= RELIEFNT_USER_TOP)
            return -EFAULT;
        uint32_t count = SVGA_HOST_PAGE_SIZE - (address & (SVGA_HOST_PAGE_SIZE - 1));
        if (count > capacity - offset)
            count = capacity - offset;
#ifdef CONFIG_SWAP
        int ret = drm_user_import(task, str + offset, address, count);
#else
        int ret = user_copy_from_task(task, str + offset, address, count);
#endif
        if (ret)
            return ret;
        for (uint32_t i = 0; i < count; ++i)
            if (!str[offset + i])
                return offset + i;
        offset += count;
        address += count;
    }
    return capacity;
}

static uint16_t message_status(struct host_regs result)
{
    return (uint32_t)result.cx >> 16;
}

/* Low-bandwidth RPC keeps the kernel addresses in the guest. The caller owns
 * the channel until CLOSE. Only checkpoint status authorizes a retry. */
static int transfer_message(const struct host_channel *channel, uint8_t *bytes, uint32_t length,
                             enum message_operation op, uint64_t start)
{
    for (uint32_t offset = 0; offset < length;) {
        if (time_work_counter_us() - start >= MESSAGE_TIMEOUT_US)
            return -ETIMEDOUT;
        uint32_t count = length - offset < 4 ? length - offset : 4;
        uint32_t word = MESSAGE_SUCCESS;
        if (op == MESSAGE_SEND_WORD) {
            word = 0;
            __builtin_memcpy(&word, bytes + offset, count);
        }
        const struct host_regs result = message_call(op, word, channel);
        uint16_t status = message_status(result);
        if (!(status & MESSAGE_SUCCESS))
            return status & MESSAGE_CHECKPOINT ? -EAGAIN : -EIO;
        if (op == MESSAGE_RECEIVE_WORD) {
            word = (uint32_t)result.bx;
            __builtin_memcpy(bytes + offset, &word, count);
        }
        offset += count;
    }
    return 0;
}

static int send_message(const struct host_channel *channel, char *text, uint32_t length,
                        uint64_t start)
{
    /* transfer_message reads the source on SEND_WORD. */
    for (unsigned attempt = 0; attempt < MESSAGE_RETRIES; ++attempt) {
        if (time_work_counter_us() - start >= MESSAGE_TIMEOUT_US)
            return -ETIMEDOUT;
        if (!(message_status(message_call(MESSAGE_SEND_SIZE, length, channel)) & MESSAGE_SUCCESS))
            return -EIO;
        int ret = transfer_message(channel, (uint8_t *)text, length, MESSAGE_SEND_WORD, start);
        if (ret != -EAGAIN)
            return ret;
    }
    return -EIO;
}

static int receive_message(struct task *task, struct drm_vmw_msg_arg *arg,
                           const struct host_channel *channel, uint64_t start)
{
    for (unsigned attempt = 0; attempt < MESSAGE_RETRIES; ++attempt) {
        if (time_work_counter_us() - start >= MESSAGE_TIMEOUT_US)
            return -ETIMEDOUT;
        const struct host_regs size = message_call(MESSAGE_RECEIVE_SIZE, 0, channel);
        uint16_t status = message_status(size);
        if (!(status & MESSAGE_SUCCESS))
            return -EIO;
        if (!(status & MESSAGE_HAS_REPLY))
            return 0;
        uint32_t length = (uint32_t)size.bx;
        if (length > MESSAGE_MAX_REPLY)
            return -E2BIG;
        uint8_t *reply = length ? kernel_malloc(length) : NULL;
        if (length && !reply)
            return -ENOMEM;
        int ret = transfer_message(channel, reply, length, MESSAGE_RECEIVE_WORD, start);
        if (!ret) {
            status = message_status(message_call(MESSAGE_RECEIVE_STATUS, MESSAGE_SUCCESS, channel));
            if (!(status & MESSAGE_SUCCESS))
                ret = status & MESSAGE_CHECKPOINT ? -EAGAIN : -EIO;
        }
        if (!ret && length) {
            /* A delivered message is never replayed after usercopy. */
#ifdef CONFIG_SWAP
            if (drm_user_prepare(task, arg->receive, length, true))
                ret = -EFAULT;
#endif
            if (!ret && user_copy_to_task(task, arg->receive, reply, length))
                ret = -EFAULT;
            if (!ret)
                arg->receive_len = length;
        }
        kernel_free(reply);
        if (ret != -EAGAIN)
            return ret;
    }
    return -EIO;
}

int svga_host_message(struct task *task, struct drm_vmw_msg_arg *arg)
{
    if (!svga_host_available())
        return -ENODEV;
    char *message = kernel_malloc(MESSAGE_MAX_SEND);
    if (!message)
        return -ENOMEM;
    int count = svga_host_copy_string(task, arg->send, message, MESSAGE_MAX_SEND);
    if (count < 0 || (uint32_t)count == MESSAGE_MAX_SEND) {
        kernel_free(message);
        return count < 0 ? count : -EINVAL;
    }

    const uint64_t start = time_work_counter_us();
    struct host_channel channel = {0};
    const struct host_regs opened = message_call(MESSAGE_OPEN, VMWARE_RPCI_COOKIE, &channel);
    if (!(message_status(opened) & MESSAGE_SUCCESS)) {
        kernel_free(message);
        return -EIO;
    }
    channel.id = (uint32_t)opened.dx >> 16;
    channel.cookie_high = (uint32_t)opened.si;
    channel.cookie_low = (uint32_t)opened.di;
    int ret = send_message(&channel, message, count, start);
    kernel_free(message);
    if (!ret && !arg->send_only)
        ret = receive_message(task, arg, &channel, start);
    (void)message_call(MESSAGE_CLOSE, 0, &channel);
    return ret;
}
