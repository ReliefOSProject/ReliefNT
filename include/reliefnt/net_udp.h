#ifndef RELIEFNT_NET_UDP_H
#define RELIEFNT_NET_UDP_H
#include <reliefnt/types.h>
struct task_file;
#define TASK_FILE_KIND_UDP 2u
int64_t syscall_udp(uint64_t number, uint64_t a0, uint64_t a1, uint64_t a2,
                    uint64_t a3, uint64_t a4, uint64_t a5);
int task_udp_send(struct task_file *file, const void *data, uint32_t length,
                   uint32_t flags, uint64_t address, uint32_t address_length);
int task_udp_recv(struct task_file *file, void *data, uint32_t length,
                   uint32_t flags, uint64_t address, uint64_t address_length);
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
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                    uint16_t destination_port, const void *data, uint32_t length, int ifindex);
#endif
