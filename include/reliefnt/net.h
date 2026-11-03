/*
 * ReliefOS kernel networking interface: declares sockets and packet services.
 * Defines the internal contract between network drivers and syscalls.
 */
#ifndef RELIEFNT_NET_H
#define RELIEFNT_NET_H
#include <linux/in6.h>

#include <reliefos/net_abi.h>
#include <reliefos/system_abi.h>
#include <reliefnt/types.h>

struct task;

/**
 * @brief Probe and configure the network stack: load DNS policy, init the NIC, and try a DHCP lease.
 */
void net_init(void);
/**
 * @brief Return non-zero when a network interface is present and usable.
 */
int net_is_ready(void);
/**
 * @brief Check the active TCP hint and timer gate before switching stacks.
 * @return True when one bounded maintenance pass is due under the current tick.
 * @note Requires execution ownership; constant-time and does not inspect idle
 *       socket registry entries.
 */
bool net_tcp_maintenance_due(void);
/**
 * @brief Advance queued TCP work from a bounded task-context maintenance pass.
 * @return None; at most one tick's packet and socket budget is serviced.
 * @note Call with kernel execution ownership held from syscall, idle, or the
 *       scheduler-owned timer-return stack. Do not call from raw device IRQ
 *       callbacks. The helper returns in constant time when no TCP work is queued.
 */
void net_tcp_maintenance(void);
/**
 * @brief Copy the current interface configuration (IP, MAC, flags) into config; 0 on success.
 */
int net_get_config(struct reliefos_net_config *config);
void net_poll_packets(void);
/**
 * @brief Transmit a raw IPv4 message over the configured Ethernet route.
 * @param destination Host-order destination address.
 * @param source Host-order bound source, zero selects the interface address.
 * @param protocol Protocol for a generated IP header.
 * @param header_included Whether data already contains an IPv4 header.
 * @param data Validated message bytes, borrowed under the execution lock.
 * @param length Message length.
 * @return Bytes sent or negative errno, including EMSGSIZE without fragmentation.
 */
int net_ipv4_send_raw(uint32_t destination, uint32_t source, uint8_t protocol,
                      bool header_included, const void *data, uint32_t length);
int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length);
/**
 * @brief Read or change the DNS resolver mode and server; results and status are written back into request.
 */
int net_set_dns_policy(struct reliefos_net_dns_policy *request);
/**
 * @brief Renew the DHCP lease, writing the resulting config and status back into request.
 */
int net_dhcp_renew(struct reliefos_net_dhcp *request);
/**
 * @brief Send an ICMP echo request to the target and record sent/received/rtt_ms in request.
 */
int net_ping(struct reliefos_net_ping *request);
/**
 * @brief Resolve a hostname to IPv4 addresses and fill request with results and status.
 */
int net_dns_resolve(struct reliefos_net_dns *request);
/**
 * @brief Query the configured NTP server and fill request with the resulting time offset.
 */
int net_ntp_sync(struct reliefos_time_sync *request);
/**
 * @brief Fetch a URL over HTTP and fill request with the status code and response body.
 */
int net_http_get(struct reliefos_net_http_get *request);
/**
 * @brief Allocate a TCP socket and return its internal network handle in request.
 * @param request Requested family/type/protocol and output handle/status.
 * @param owner_pid Owning process identifier.
 * @param owner_uid Owning user identifier used for connection visibility.
 * @return Zero when the request was processed; inspect request->status for ABI errors.
 */
int net_socket_open(struct reliefos_net_socket_open *request, uint32_t owner_pid,
                    uint32_t owner_uid);
/**
 * @brief Resolve and connect the requested TCP socket, updating endpoint fields.
 * @param request Socket handle, hostname/address, port, and output status/endpoints.
 * @param owner_pid Required socket owner process identifier.
 * @return Zero when the request was processed; inspect request->status for the result.
 */
int net_socket_connect(struct reliefos_net_socket_connect *request, uint32_t owner_pid);
/**
 * @brief Queue as much of the request payload as fits in the TCP send buffer.
 * @param request Socket handle, borrowed payload, requested length, and output progress.
 * @param owner_pid Required socket owner process identifier.
 * @return Zero when processed; inspect request->transferred and request->status.
 * @note Accepted bytes are retained for the network pump until cumulative ACKs free them.
 */
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner_pid);
/**
 * @brief Copy available TCP stream bytes into the request buffer or report EOF/status.
 * @param request Socket handle, writable buffer, requested length, and output progress.
 * @param owner_pid Required socket owner process identifier.
 * @return Zero when processed; inspect request->transferred and request->status.
 */
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner_pid);
/**
 * @brief Copy TCP receive bytes while preserving the unread ring and window.
 * @param request Borrowed handle, writable length-byte buffer and output status/progress.
 * @param owner_pid Optional owner filter, zero for the caller's independently held socket.
 * @return Zero when processed, negative internal validation error otherwise.
 * Caller owns execution and socket lifetime. EOF/reset/timeout use the same
 * status mapping as net_socket_recv; no code or buffer ownership is transferred.
 */
int net_socket_peek(struct reliefos_net_socket_io *request, uint32_t owner_pid);
/**
 * @brief Begin closing a TCP socket after queued stream data has drained.
 * @param request Socket handle and output status.
 * @param owner_pid Required socket owner process identifier.
 * @return Zero when the request was processed; inspect request->status.
 * @note FIN transmission and completion are serviced asynchronously by the network pump.
 */
int net_socket_close(struct reliefos_net_socket_close *request, uint32_t owner_pid);
/**
 * @brief Count visible TCP sockets and fill up to request->capacity snapshot entries.
 * @param request Capacity, optional fixed-layout entry array, and output live count.
 * @param viewer Task whose role/user identity controls visibility.
 * @return Zero, or a negative error for an invalid request.
 * @note The live count may exceed the legacy UAPI entry capacity.
 */
int net_connections(struct reliefos_net_connection_list *request, const struct task *viewer);
/**
 * @brief Poll TCP readiness for an AF_INET file description.
 * @param handle Internal socket handle.
 * @param owner_pid Optional owner filter, or zero to match any process.
 * @param events Requested poll bits.
 * @return Ready bits, or POLLNVAL when the handle is unknown.
 */
short net_socket_poll_fd(int32_t handle, uint32_t owner_pid, short events);
/**
 * @brief Retain an internal socket while an AF_INET file description owns it.
 * @param handle Internal socket handle.
 */
void net_socket_pin_fd(int32_t handle);
/**
 * @brief Release an AF_INET file-description reference and permit later collection.
 * @param handle Internal socket handle.
 */
void net_socket_release_fd(int32_t handle);
/**
 * @brief Park a blocking stream writer until transmit queue space is available.
 * @param handle Internal socket handle.
 * @return One while parked, zero when it should retry, or a negative errno.
 * The socket's SO_SNDTIMEO value controls the wait; an unset timeout is infinite.
 */
int net_socket_wait_writable(int32_t handle);
/**
 * @brief Park a blocking stream reader until data, EOF, or an error is available.
 * @param handle Internal socket handle.
 * @return One while parked, zero when it should retry, or a negative errno.
 * The socket's SO_RCVTIMEO value controls the wait; an unset timeout is infinite.
 */
int net_socket_wait_readable(int32_t handle);
#ifdef CONFIG_SWAP
/** @brief Remove only this socket's readiness registration before page-in wait.
 * @param handle Pinned TCP socket handle. @param task Borrowed waiting task.
 * Execution ownership protects lookup; unrelated provider waits are untouched.
 */
void net_socket_cancel_wait(int32_t handle, struct task *task);
#endif
/**
 * @brief Read the configured TCP send or receive timeout in scheduler ticks.
 * @param handle Internal socket handle.
 * @param send Select SO_SNDTIMEO when true, SO_RCVTIMEO otherwise.
 * @return Timeout ticks, or UINT64_MAX when no timeout is configured.
 */
uint64_t net_socket_get_timeout_ticks(int32_t handle, bool send);
/**
 * @brief Set a TCP send or receive timeout in scheduler ticks.
 * @param handle Internal socket handle.
 * @param send Select SO_SNDTIMEO when true, SO_RCVTIMEO otherwise.
 * @param ticks Timeout ticks, or UINT64_MAX to disable the timeout.
 * @return Zero on success or a negative errno.
 */
int net_socket_set_timeout_ticks(int32_t handle, bool send, uint64_t ticks);
/**
 * @brief Read and optionally consume a pending socket error.
 * @param handle Internal socket handle.
 * @param clear Whether to clear the retained error after reading it.
 * @return Positive Linux errno, zero when none is pending, or negative EBADF.
 */
int net_socket_error(int32_t handle, bool clear);
/**
 * @brief Return the number of bytes currently queued for TCP receive.
 * @param handle Internal socket handle.
 * @return Queued byte count, or negative EBADF.
 */
int net_socket_available(int32_t handle);
/**
 * @brief Connect a descriptor socket to a numeric IPv4 endpoint.
 * @param handle Internal socket handle.
 * @param ip Host-order IPv4 address.
 * @param port Peer TCP port.
 * @param nonblock Whether an incomplete handshake should return EINPROGRESS.
 * @return Zero or a negative Linux errno.
 */
int net_socket_connect_fd(int32_t handle, uint32_t ip, uint16_t port, bool nonblock);
/** @brief Assign the Linux socket family to a newly allocated stream endpoint.
 * @param handle Internal handle. @param ipv6 True for AF_INET6. @return Zero or errno. */
int net_socket_set_ipv6(int32_t handle,bool ipv6);
/** @brief Query the native family of a TCP endpoint.
 * @param handle Internal handle. @return AF_INET/AF_INET6 or negative errno. */
int net_socket_family(int32_t handle);
/** @brief Bind a native or mapped stream endpoint with cross-family conflict checks.
 * @param handle Socket. @param address Network-order native address. @return Zero or errno. */
int net_socket_bind6_fd(int32_t handle,const struct sockaddr_in6 *address);
/** @brief Connect a TCP endpoint to a native or mapped IPv6 address.
 * @param handle Socket. @param address Peer native address. @param nonblock Nonblocking OFD.
 * @return Zero, errno, or internal blocked marker. */
int net_socket_connect6_fd(int32_t handle,const struct sockaddr_in6 *address,bool nonblock);
/** @brief Copy the local or peer native IPv6 endpoint.
 * @param handle Socket. @param peer Select peer. @param out Caller-owned output.
 * @return Zero, ENOTCONN, or negative errno. */
int net_socket_address6(int32_t handle,bool peer,struct sockaddr_in6 *out);
/** @brief Configure a bound TCP endpoint for passive connections.
 * @param handle Socket. @param backlog Maximum queued children. @return Zero or errno. */
int net_socket_listen_fd(int32_t handle,int backlog);
/** @brief Find an established child without consuming a listener queue entry.
 * @param handle Listener. @return Positive child handle or negative errno. */
int net_socket_accept_peek(int32_t handle);
/** @brief Transfer an established child from its listener to a newly allocated OFD.
 * @param handle Listener. @param child Child handle. @return Zero or errno. */
int net_socket_accept_commit(int32_t handle,int32_t child);
/** @brief Get or update an implemented IPv6 stream option.
 * @param handle Socket. @param set True to update. @param option IPv6 option.
 * @param value Input/output integer. @return Zero or negative errno. */
int net_socket_ipv6_option(int32_t handle,bool set,uint32_t option,int *value);
/** @brief Query whether a socket is accepting new connections.
 * @param handle Socket. @return One, zero, or negative errno. */
int net_socket_is_listening(int32_t handle);
/** @brief Read or set TCP address reuse before binding.
 * @param handle Socket. @param set Select update. @param value Input/output flag.
 * @return Zero or negative errno. */
int net_socket_reuse(int32_t handle,bool set,int *value);
struct text_stream;
/** @brief Apply a validated quoted IPv6 TCP error to the matching tuple.
 * @param source Original local address. @param destination Original peer.
 * @param source_port Local port. @param destination_port Peer port.
 * @param error Positive errno. @param mtu Reported path MTU or zero. */
void net_tcp6_error(const struct in6_addr *source,const struct in6_addr *destination,
                    uint16_t source_port,uint16_t destination_port,int error,uint32_t mtu);
/** @brief Render the live native IPv6 TCP registry.
 * @param stream Caller-owned proc output. */
void net_tcp6_proc_emit(struct text_stream *stream);
/**
 * @brief Shut down a TCP socket direction and queue a FIN after pending writes.
 * @param handle Internal socket handle.
 * @param how SHUT_RD, SHUT_WR, or SHUT_RDWR.
 * @return Zero or a negative Linux errno.
 */
int net_socket_shutdown_fd(int32_t handle, int how);
/**
 * @brief Fill local/remote IPv4 endpoints for getsockname/getpeername.
 */
int net_socket_address(int32_t handle, uint32_t owner_pid,
                       uint32_t *local_ip, uint16_t *local_port,
                       uint32_t *remote_ip, uint16_t *remote_port);
/**
 * @brief Retire sockets owned by a process that no longer has descriptors.
 * @param owner_pid Process identifier whose unowned network state is retired.
 * @return None; accepted TCP data and an initiated FIN continue asynchronously
 *         until the network pump drains or fails the connection.
 * @note Idle unconnected sockets are closed immediately; orderly-close state
 *       remains registered through the diagnostic hold interval.
 */
void net_close_owner_sockets(uint32_t owner_pid);
/**
 * @brief Drop network state after the NIC driver is removed so callers stop using stale sockets.
 */
void net_driver_detached(void);
/**
 * @brief Report the NIC's presence/active flags, 48-bit MAC, and current local IPv4 address.
 */
void net_device_info(uint32_t *flags, uint64_t *mac_value, uint32_t *local_ip);

/** @brief Execute a native Linux interface/route ioctl with checked user memory. */
int net_interface_ioctl(uint32_t request, uint64_t address);

/** @brief Report whether eth0 is administratively up and ready. */
bool net_interface_ready(void);

/** @brief Report a registered Ethernet device independently of carrier state.
 * @return True while a retained provider publishes a present PCI device;
 * false before registration or after detach. Caller owns network execution.
 */
bool net_interface_present(void);

/** @brief Report the administrative IFF_UP setting independently of carrier.
 * @return Current eth0 administrative flag; caller owns network execution.
 */
bool net_interface_admin_up(void);

#endif
