#ifndef RELIEFNT_NET_PACKET_H
#define RELIEFNT_NET_PACKET_H
#include <reliefnt/types.h>
struct task_file;
#define TASK_FILE_KIND_PACKET 3u
/** @brief Dispatch raw IPv4 and Ethernet socket operations under the execution lock.
 * @param number Native syscall number.
 * @param a0 First native argument.
 * @param a1 Second native argument.
 * @param a2 Third native argument.
 * @param a3 Fourth native argument.
 * @param a4 Fifth native argument.
 * @param a5 Sixth native argument.
 * @return Syscall result or negative errno.
 */
int64_t syscall_packet(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5);
/** @brief Queue an ingress Ethernet frame for matching raw/packet sockets.
 * @param frame Complete Ethernet frame, borrowed during the call.
 * @param length Available frame bytes; execution lock held.
 */
void net_packet_input(const void *frame, uint32_t length);
/** @brief Send one raw/packet message from a pinned description.
 * @param file Socket description under the execution lock.
 * @param data Validated message bytes.
 * @param length Message length.
 * @param flags Linux send flags.
 * @param address Optional user destination address.
 * @param address_length Destination address size.
 * @return Bytes sent or negative errno.
 */
int task_packet_send(struct task_file *file, const void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length);
/** @brief Receive one raw/packet message, preserving datagram boundaries.
 * @param file Socket description under the execution lock.
 * @param data Validated writable destination.
 * @param length Destination capacity.
 * @param flags Linux receive flags.
 * @param address Optional user source-address destination.
 * @param address_length User socklen_t address when address is nonzero.
 * @return Byte count, negative errno, or the interruptible blocking sentinel.
 */
int task_packet_recv(struct task_file *file, void *data, uint32_t length,
                     uint32_t flags, uint64_t address, uint64_t address_length);
/** @brief Return this packet OFD's native source-address size, or zero. */
uint32_t task_packet_address_size(const struct task_file *file);
struct net_udp_rx_info;
/** @brief Receive a packet and its native source and IPv6 ancillary metadata.
 * @param file Retained OFD. @param data Kernel buffer. @param length Capacity.
 * @param flags Receive flags. @param source Optional sockaddr storage.
 * @param source_length Optional source size. @param info Optional IPv6 metadata.
 * @return Bytes, negative errno, or blocked sentinel. */
int task_packet_recv_ex(struct task_file *file,void *data,uint32_t length,uint32_t flags,
                        void *source,uint32_t *source_length,struct net_udp_rx_info *info);
#ifdef CONFIG_SWAP
/** @brief Receive to kernel buffers and capture the native source before dequeue.
 * @param file Retained packet OFD. @param data Kernel byte output.
 * @param length Capacity. @param flags Receive flags. @param source Writable
 * kernel storage for the native address. @param source_length Receives its size.
 * @return Bytes/errno or the existing blocked marker. Both outputs are owned
 * by the caller; no user page or borrowed queue pointer survives this call.
 */
int task_packet_recv_staged(struct task_file *file,void *data,uint32_t length,
                            uint32_t flags,void *source,uint32_t *source_length);
#endif
/** @brief Query raw/packet readiness.
 * @param file Pinned socket description.
 * @param events Requested poll bits.
 * @return Ready event bits.
 */
short task_packet_poll(struct task_file *file, short events);
/** @brief Release the final raw/packet description and queued messages.
 * @param file Owned description under the execution lock.
 */
void task_packet_release(struct task_file *file);
/** @brief Return the next raw/packet message length.
 * @param file Pinned socket description.
 * @return Length, zero when empty, or negative errno.
 */
int task_packet_available(struct task_file *file);
/** @brief Deliver one Ethernet frame while retaining its original ingress link.
 * @param ifindex Stable ingress interface. @param frame Borrowed frame.
 * @param length Complete Ethernet frame length. */
void net_packet_input_on(uint32_t ifindex, const void *frame, uint32_t length);


/** @brief Wake raw sockets bound to a disappearing device. @param ifindex Device. */
void net_packet_device_detached(uint32_t ifindex);

#endif
