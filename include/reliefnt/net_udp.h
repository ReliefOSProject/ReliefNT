#ifndef RELIEFNT_NET_UDP_H
#define RELIEFNT_NET_UDP_H
#include <reliefnt/types.h>
#include <linux/in6.h>
struct task_file;
#define TASK_FILE_KIND_UDP 2u
int64_t syscall_udp(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5);
int task_udp_send(struct task_file *file, const void *data, uint32_t length,
                   uint32_t flags, uint64_t address, uint32_t address_length);
int task_udp_recv(struct task_file *file, void *data, uint32_t length,
                   uint32_t flags, uint64_t address, uint64_t address_length);
struct net_udp_rx_info {
    struct in6_addr destination;
    uint32_t ifindex;
    int hops;
    bool pktinfo, hoplimit, old_hoplimit;
};
/** @brief Determine the socket's native source-address size.
 * @param file Pinned UDP OFD. @return Address size, or zero for invalid OFD. */
uint32_t task_udp_address_size(const struct task_file *file);
/** @brief Receive into owned buffers, including source and IPv6 packet metadata.
 * @param file Pinned UDP OFD. @param data Kernel output. @param length Capacity.
 * @param flags Receive flags. @param source Optional native sockaddr storage.
 * @param source_length Receives source size when source is present.
 * @param info Optional packet metadata. @return Bytes, errno, or blocked sentinel. */
int task_udp_recv_ex(struct task_file *file, void *data, uint32_t length, uint32_t flags,
                     void *source, uint32_t *source_length, struct net_udp_rx_info *info);
#ifdef CONFIG_SWAP
struct sockaddr_in;
/** @brief Receive into owned kernel storage and capture the source before dequeue.
 * @param file Retained UDP OFD. @param data Kernel output bytes. @param length
 * Capacity. @param flags Native receive flags. @param source Optional kernel
 * source-address output. @return Bytes/errno or the existing blocked marker.
 */
int task_udp_recv_staged(struct task_file *file,void *data,uint32_t length,
                         uint32_t flags,struct sockaddr_in *source);
#endif
short task_udp_poll(struct task_file *file, short events);
void task_udp_release(struct task_file *file);
int task_udp_available(struct task_file *file);
struct text_stream;
/** @brief Associate a validated ICMPv6 quote with matching UDP state.
 * @param source Original local. @param destination Original peer. @param source_port Local port.
 * @param destination_port Peer port. @param error Positive errno. @param mtu Reported MTU. */
void net_udp6_error(const struct in6_addr *source,const struct in6_addr *destination,
                    uint16_t source_port,uint16_t destination_port,int error,uint32_t mtu);
/** @brief Render live IPv6 UDP endpoints into a proc slice.
 * @param stream Caller-owned proc output. */
void net_udp6_proc_emit(struct text_stream *stream);
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                    uint16_t destination_port, const void *data, uint32_t length, int ifindex);

/** @brief Deliver a checked ICMPv6 error only to UDP state on the matching link.
 * @param ifindex Link or zero. @param source Local. @param destination Peer.
 * @param source_port Local port. @param destination_port Peer port.
 * @param error Positive errno. @param mtu Reported MTU. */
void net_udp6_error_on(uint32_t ifindex, const struct in6_addr *source,
                       const struct in6_addr *destination, uint16_t source_port,
                       uint16_t destination_port, int error, uint32_t mtu);
/** @brief Wake UDP sockets bound to a disappearing device. @param ifindex Device. */
void net_udp_device_detached(uint32_t ifindex);

#endif
