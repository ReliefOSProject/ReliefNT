#ifndef RELIEFNT_NET_NET_C_INTERNAL_H
#define RELIEFNT_NET_NET_C_INTERNAL_H

/*
 * ReliefOS kernel networking: provides the network and socket service layer.
 * Handles interfaces, packet transport, TCP state, and user-facing requests.
 */
#include <reliefnt/console.h>
#include <reliefnt/e1000.h>
#include <reliefnt/heap.h>
#include <reliefnt/net.h>
#include <reliefnt/net_packet.h>
#include <reliefnt/net_udp.h>
#include <linux/errno.h>
#include <linux/if.h>
#include <linux/capability.h>
#include <reliefnt/usercopy.h>
#include <reliefnt/sched.h>
#include <linux/poll.h>
#include <reliefnt/storage.h>
#include <reliefnt/time.h>
#include <reliefos/layout.h>

#include <reliefos/device_abi.h>

#define ETH_TYPE_IPV4 0x0800u
#define ETH_TYPE_ARP 0x0806u
#define ARP_HTYPE_ETHERNET 0x0001u
#define ARP_OPER_REQUEST 0x0001u
#define ARP_OPER_REPLY 0x0002u
#define IPV4_PROTO_ICMP 1u
#define IPV4_PROTO_TCP 6u
#define IPV4_PROTO_UDP 17u
#define ICMP_ECHO_REPLY 0u
#define ICMP_ECHO_REQUEST 8u
#define TCP_FLAG_FIN 0x01u
#define TCP_FLAG_SYN 0x02u
#define TCP_FLAG_RST 0x04u
#define TCP_FLAG_PSH 0x08u
#define TCP_FLAG_ACK 0x10u
#define NET_FRAME_MAX 1536u
#define NET_ICMP_PAYLOAD_LEN 16u
#define NET_DHCP_PACKET_MAX 548u
#define NET_DHCP_PACKET_LEN 300u
#define NET_DNS_PACKET_MAX 512u
#define NET_NTP_PACKET_LEN 48u
#define NET_HTTP_REQUEST_MAX 640u
#define NET_DHCP_CLIENT_PORT 68u
#define NET_DHCP_SERVER_PORT 67u
#define NET_DNS_PORT 53u
#define NET_NTP_PORT 123u
#define NET_HTTP_PORT 80u
#define NET_DHCP_MAGIC 0x63825363u
#define NET_DHCP_DISCOVER 1u
#define NET_DHCP_OFFER 2u
#define NET_DHCP_REQUEST 3u
#define NET_DHCP_ACK 5u
#define NET_DHCP_NAK 6u
#define NET_BOOT_DHCP_ATTEMPTS 3u
#define NET_BOOT_DHCP_TIMEOUT_MS 4000u
#define NET_TCP_MSS 1460u
#define NET_TCP_SYN_RETRANSMIT_MS 500u
#define NET_TCP_DATA_RETRANSMIT_MS 750u
#define NET_ARP_CACHE_SIZE 8u
#define NET_HTTP_DEFAULT_TIMEOUT_MS 8000u
#define NET_HTTP_MAX_TIMEOUT_MS 10000u
/* The full unscaled TCP window; scaling is not negotiated yet. */
#define NET_SOCKET_RX_CAP 65535u
#ifndef NET_TCP_TRACE
#define NET_TCP_TRACE 0
#endif
#define NET_SOCKET_CLOSE_HOLD_MS 10000u
#define NET_SOCKET_DEFAULT_TIMEOUT_MS 5000u
#define NET_NETWORK_CONFIG_PATH RELIEFOS_PATH_NETWORK_CONF
#define NET_NETWORK_CONFIG_BACKUP_PATH RELIEFOS_PATH_NETWORK_BAK
#define NET_NETWORK_CONFIG_MAX 256u
#define NET_NTP_UNIX_EPOCH_OFFSET 2208988800ULL

struct net_arp_wait {
    uint32_t ip;
    uint8_t mac[6];
    uint32_t done;
};

struct net_ping_wait {
    uint32_t target_ip;
    uint16_t ident;
    uint16_t sequence;
    uint32_t done;
};

struct net_udp_wait {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint8_t *payload;
    uint32_t capacity;
    uint32_t length;
    uint32_t done;
};

struct net_tcp_wait {
    uint32_t src_ip;
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t remote_seq;
    uint32_t acked_seq;
    uint8_t *payload;
    uint32_t capacity;
    uint32_t length;
    uint32_t flags;
    uint32_t reset;
    uint32_t fin;
    uint32_t overflow;
    uint32_t changed;
};

struct net_dhcp_offer {
    uint32_t msg_type;
    uint32_t yiaddr;
    uint32_t subnet_mask;
    uint32_t router_ip;
    uint32_t dns_ip;
    uint32_t server_ip;
    uint32_t lease_seconds;
};

struct net_arp_cache_entry {
    uint32_t ip;
    uint8_t mac[6];
};

struct net_socket {
    uint32_t used;
    bool fd_owned;
    bool shutdown_read;
    bool shutdown_write;
    int error;
    uint64_t syn_deadline;
    uint64_t syn_retransmit;
    int32_t handle;
    uint32_t owner_pid;
    uint32_t owner_uid;
    uint32_t state;
    uint32_t status;
    uint32_t local_ip;
    uint32_t remote_ip;
    uint16_t local_port;
    uint16_t remote_port;
    uint32_t local_seq;
    uint32_t remote_seq;
    uint32_t acked_seq;
    uint32_t rx_len;
    uint32_t rx_head;
    uint32_t rx_window_ack;
    uint32_t rx_window;
    bool rx_window_valid;
    uint32_t tx_bytes;
    uint32_t rx_bytes;
    uint32_t created_ms;
    uint32_t changed_ms;
    uint32_t fin_received;
    uint8_t dst_mac[6];
    uint8_t *rx;
};







































































































/* Remaining space already advertised to the peer, across sequence wrap. */




















































































































/* The descriptor table owns one reference per open file description. PID
 * filtering belongs to the management API, not inherited Linux descriptors. */


extern const uint8_t net_broadcast_mac[6];
extern uint32_t net_sequence;
extern uint16_t net_ipv4_id;
extern struct reliefos_net_config net_config;
extern bool net_interface_up;
extern struct net_socket net_sockets[RELIEFOS_NET_SOCKET_MAX];

/**
 * Net cpu relax.
 */
void net_cpu_relax(void);

/**
 * Net memcpy.
 * @param dst Value supplied by the caller.
 * @param src Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
void net_memcpy(void *dst, const void *src, uint32_t len);

/**
 * Net memzero.
 * @param dst Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
void net_memzero(void *dst, uint32_t len);

/**
 * Net strlen.
 * @param text NUL-terminated text supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t net_strlen(const char *text, uint32_t cap);

/**
 * Net mac is zero.
 * @param mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_mac_is_zero(const uint8_t mac[6]);

/**
 * Net memeq.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int net_memeq(const uint8_t *a, const uint8_t *b, uint32_t len);

/**
 * Net text eq len.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int net_text_eq_len(const char *a, const char *b, uint32_t len);

/**
 * Net load dns policy.
 */
void net_load_dns_policy(void);

/**
 * Net get u16.
 * @param p Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_get_u16(const uint8_t *p);

/**
 * Net get u32.
 * @param p Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_get_u32(const uint8_t *p);

/**
 * Net put u16.
 * @param p Value supplied by the caller.
 * @param value Value supplied by the caller.
 */
void net_put_u16(uint8_t *p, uint16_t value);

/**
 * Net put u32.
 * @param p Value supplied by the caller.
 * @param value Value supplied by the caller.
 */
void net_put_u32(uint8_t *p, uint32_t value);

/**
 * Net tcp seq after or equal.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_tcp_seq_after_or_equal(uint32_t a, uint32_t b);

/**
 * Net checksum.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint16_t net_checksum(const uint8_t *data, uint32_t len);

/**
 * Net checksum partial.
 * @param sum Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t net_checksum_partial(uint32_t sum, const uint8_t *data,
                                     uint32_t len);

/**
 * Net checksum finish.
 * @param sum Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_checksum_finish(uint32_t sum);

/**
 * Net tcp checksum.
 * @param src_ip Value supplied by the caller.
 * @param dst_ip Value supplied by the caller.
 * @param tcp Value supplied by the caller.
 * @param tcp_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_tcp_checksum(uint32_t src_ip, uint32_t dst_ip,
                                 const uint8_t *tcp, uint16_t tcp_len);

/**
 * Net append char.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param ch Value supplied by the caller.
 */
void net_append_char(char *dst, uint32_t *pos, uint32_t cap, char ch);

/**
 * Net append text.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param text NUL-terminated text supplied by the caller.
 */
void net_append_text(char *dst, uint32_t *pos, uint32_t cap,
                            const char *text);

/**
 * Net append u32.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param value Value supplied by the caller.
 */
void net_append_u32(char *dst, uint32_t *pos, uint32_t cap,
                           uint32_t value);

/**
 * Net timeout expired.
 * @param start_ms Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param spins Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_timeout_expired(uint64_t start_ms, uint32_t timeout_ms,
                               uint32_t spins);

/**
 * Net arp cache clear.
 */
void net_arp_cache_clear(void);

/**
 * Net update config flags.
 */
void net_update_config_flags(void);

/**
 * Net set static fallback.
 */
void net_set_static_fallback(void);

/**
 * Net apply dhcp offer.
 * @param offer Value supplied by the caller.
 */
void net_apply_dhcp_offer(const struct net_dhcp_offer *offer);

/**
 * Net write eth.
 * @param frame Value supplied by the caller.
 * @param dst_mac Value supplied by the caller.
 * @param type Value supplied by the caller.
 */
void net_write_eth(uint8_t *frame, const uint8_t *dst_mac,
                          uint16_t type);

/**
 * Net route arp ip.
 * @param target_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_route_arp_ip(uint32_t target_ip);

/**
 * Net frame for us.
 * @param frame Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_frame_for_us(const uint8_t *frame);

/**
 * Net send udp to mac.
 * @param dst_mac Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param dst_ip Value supplied by the caller.
 * @param src_port Value supplied by the caller.
 * @param dst_port Value supplied by the caller.
 * @param payload Value supplied by the caller.
 * @param payload_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_send_udp_to_mac(const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip, uint16_t src_port,
                               uint16_t dst_port, const uint8_t *payload,
                               uint32_t payload_len);

uint32_t net_socket_receive_window(const struct net_socket *socket);

uint32_t net_socket_select_window(const struct net_socket *socket);

int net_send_tcp_to_mac(const uint8_t *dst_mac, uint32_t src_ip,
                               uint32_t dst_ip, uint16_t src_port,
                               uint16_t dst_port, uint32_t seq,
                               uint32_t ack, uint8_t flags,
                               const uint8_t *payload,
                               uint32_t payload_len);

/**
 * Net handle arp.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param arp_wait Value supplied by the caller.
 */
void net_handle_arp(const uint8_t *frame, uint32_t len,
                           struct net_arp_wait *arp_wait);

/**
 * Net handle udp.
 * @param ip Value supplied by the caller.
 * @param total_len Value supplied by the caller.
 * @param ihl Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 */
void net_handle_udp(const uint8_t *ip, uint32_t total_len,
                           uint32_t ihl, uint32_t src_ip,
                           struct net_udp_wait *udp_wait);

/**
 * Net handle tcp.
 * @param ip Value supplied by the caller.
 * @param total_len Value supplied by the caller.
 * @param ihl Value supplied by the caller.
 * @param src_ip Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
void net_handle_tcp(const uint8_t *ip, uint32_t total_len,
                           uint32_t ihl, uint32_t src_ip,
                           struct net_tcp_wait *tcp_wait);

/**
 * Net handle ipv4.
 * @param frame Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
void net_handle_ipv4(const uint8_t *frame, uint32_t len,
                            struct net_ping_wait *ping_wait,
                            struct net_udp_wait *udp_wait,
                            struct net_tcp_wait *tcp_wait);

/**
 * Net poll once.
 * @param arp_wait Value supplied by the caller.
 * @param ping_wait Value supplied by the caller.
 * @param udp_wait Value supplied by the caller.
 * @param tcp_wait Value supplied by the caller.
 */
void net_poll_once(struct net_arp_wait *arp_wait,
                          struct net_ping_wait *ping_wait,
                          struct net_udp_wait *udp_wait,
                          struct net_tcp_wait *tcp_wait);

/**
 * Net resolve mac.
 * @param ip Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param out_mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_resolve_mac(uint32_t ip, uint32_t timeout_ms, uint8_t *out_mac);

/**
 * Net socket touch.
 * @param socket Value supplied by the caller.
 */
void net_socket_touch(struct net_socket *socket);

/**
 * Net socket mark closed.
 * @param socket Value supplied by the caller.
 * @param status Output storage updated by the function.
 */
void net_socket_mark_closed(struct net_socket *socket, uint32_t status);

/**
 * Net socket trace tls.
 * @param socket Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_trace_tls(const struct net_socket *socket);

/**
 * Net ensure ipv4 config.
 * @param timeout_ms Value supplied by the caller.
 * @param require_dns Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_ensure_ipv4_config(uint32_t timeout_ms, int require_dns);

/**
 * Net init.
 */
void net_init(void);

/**
 * Net is ready.
 * @return The value or status produced by the operation.
 */
int net_is_ready(void);

void net_poll_packets(void);

/**
 * @brief Send raw IPv4 using the configured route and Ethernet transport.
 * @param destination Host-order destination IPv4 address.
 * @param source Host-order bound source, zero selects the interface address.
 * @param protocol IPv4 protocol used when constructing a header.
 * @param header_included True when data already contains an IPv4 header.
 * @param data Borrowed validated message payload under the execution lock.
 * @param length Payload bytes.
 * @return Bytes accepted or negative errno; no fragmentation is implemented.
 */
int net_ipv4_send_raw(uint32_t destination, uint32_t source, uint8_t protocol,
                      bool header_included, const void *data, uint32_t length);

int net_ipv4_send_udp(uint32_t source, uint32_t destination, uint16_t source_port,
                       uint16_t destination_port, const void *data, uint32_t length);

/**
 * Net get config.
 * @param config Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_get_config(struct reliefos_net_config *config);

/**
 * Net set dns policy.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_set_dns_policy(struct reliefos_net_dns_policy *request);

/**
 * Net dhcp renew.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_dhcp_renew(struct reliefos_net_dhcp *request);

/**
 * Net ping.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_ping(struct reliefos_net_ping *request);

/**
 * Net dns resolve.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_dns_resolve(struct reliefos_net_dns *request);

/**
 * Net ntp sync.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_ntp_sync(struct reliefos_time_sync *request);

/**
 * Net parse ipv4 literal.
 * @param text NUL-terminated text supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @param out_ip Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_parse_ipv4_literal(const char *text, uint32_t len,
                                  uint32_t *out_ip);

/**
 * Net http resolve hosts.
 * @param host Value supplied by the caller.
 * @param host_len Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param out_ips Value supplied by the caller.
 * @param out_count Value supplied by the caller.
 * @param out_status Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_http_resolve_hosts(const char *host, uint32_t host_len,
                                  uint32_t timeout_ms, uint32_t *out_ips,
                                  uint32_t *out_count,
                                  uint32_t *out_status);

/**
 * Net socket open.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @param owner_uid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_open(struct reliefos_net_socket_open *request, uint32_t owner_pid,
                    uint32_t owner_uid);

/**
 * Net socket connect.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_connect(struct reliefos_net_socket_connect *request,
                       uint32_t owner_pid);

/**
 * Net socket send.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_send(struct reliefos_net_socket_io *request, uint32_t owner_pid);

/**
 * Net socket recv.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_recv(struct reliefos_net_socket_io *request, uint32_t owner_pid);

/**
 * Net socket close.
 * @param request Value supplied by the caller.
 * @param owner_pid Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_socket_close(struct reliefos_net_socket_close *request,
                     uint32_t owner_pid);

/**
 * Net connections.
 * @param request Value supplied by the caller.
 * @param viewer Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_connections(struct reliefos_net_connection_list *request,
                    const struct task *viewer);

/**
 * Net close owner sockets.
 * @param owner_pid Value supplied by the caller.
 */
short net_socket_poll_fd(int32_t handle, uint32_t owner_pid, short events);

void net_socket_pin_fd(int32_t handle);

void net_socket_release_fd(int32_t handle);

int net_socket_error(int32_t handle, bool clear);

int net_socket_available(int32_t handle);

int net_socket_connect_fd(int32_t handle, uint32_t ip, uint16_t port, bool nonblock);

int net_socket_shutdown_fd(int32_t handle, int how);

int net_socket_address(int32_t handle, uint32_t owner_pid,
                       uint32_t *local_ip, uint16_t *local_port,
                       uint32_t *remote_ip, uint16_t *remote_port);

void net_close_owner_sockets(uint32_t owner_pid);

/**
 * Net driver detached.
 */
void net_driver_detached(void);

/**
 * Net http get.
 * @param request Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_http_get(struct reliefos_net_http_get *request);

/**
 * Net device info.
 * @param flags Identifier or flags controlling the operation.
 * @param mac_value Value supplied by the caller.
 * @param local_ip Value supplied by the caller.
 */
void net_device_info(uint32_t *flags, uint64_t *mac_value, uint32_t *local_ip);

/** @brief Apply/query eth0 through the Linux native interface ioctl ABI.
 * Only the real Ethernet interface and the supported default route are exposed.
 * Unsupported routing/features return an error rather than retaining inert state.
 */
int net_interface_ioctl(uint32_t request, uint64_t address);

/** @brief Report the real administrative and hardware state of eth0. */
bool net_interface_ready(void);

#endif
