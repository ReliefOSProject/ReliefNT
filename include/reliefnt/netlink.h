/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RELIEFNT_NETLINK_H
#define RELIEFNT_NETLINK_H
#include <reliefnt/types.h>
#include <linux/netlink.h>
struct task;
struct task_file;
#define TASK_FILE_KIND_NETLINK 4u
/** @brief Handle a route-netlink socket operation under execution ownership.
 * @param number Native syscall number. @param a0 First argument. @param a1 Second argument.
 * @param a2 Third argument. @param a3 Fourth argument. @param a4 Fifth argument.
 * @param a5 Sixth argument. @return Syscall result or negative errno. */
int64_t syscall_netlink(uint64_t number,uint64_t a0,uint64_t a1,uint64_t a2,
                        uint64_t a3,uint64_t a4,uint64_t a5);
/** @brief Process a kernel-bound netlink datagram from a pinned OFD.
 * @param file Retained OFD. @param data Validated bytes. @param length Byte count.
 * @param flags Linux flags. @param address Optional user destination. @param address_length
 * Destination size. @return Bytes accepted or negative errno. */
int task_netlink_send(struct task_file *file,const void *data,uint32_t length,
                      uint32_t flags,uint64_t address,uint32_t address_length);
/** @brief Receive one message after validating the optional source user buffer.
 * @param file Retained OFD. @param data Validated destination. @param length Capacity.
 * @param flags Linux flags. @param address Optional user source output. @param address_length
 * User socklen output. @return Bytes, errno or blocked marker. */
int task_netlink_recv(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                      uint64_t address,uint64_t address_length);
/** @brief Receive into caller-owned staging storage without retaining user pointers.
 * @param file Retained OFD. @param data Kernel output. @param length Capacity.
 * @param flags Linux flags. @param source Optional kernel source output.
 * @param source_length Output source length. @return Bytes, errno or blocked marker. */
int task_netlink_recv_staged(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                             void *source,uint32_t *source_length);
/** @brief Query queued datagrams and send readiness.
 * @param file Retained OFD. @param events Requested poll flags. @return Ready flags. */
short task_netlink_poll(const struct task_file *file,short events);
/** @brief Inspect the next queued datagram size.
 * @param file Retained OFD. @return Bytes, zero or negative errno. */
int task_netlink_available(const struct task_file *file);
/** @brief Retire the final OFD and its owned queue.
 * @param file Final OFD under execution ownership. */
void task_netlink_release(struct task_file *file);
/** @brief Cancel a task's netlink wait without retaining user memory.
 * @param file Retained OFD. @param task Task whose wait is being cancelled. */
void task_netlink_cancel_wait(struct task_file *file,struct task *task);
#endif
