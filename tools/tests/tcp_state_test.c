#include <reliefnt/console.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "modules/network.h"
#include "../../kernel/syscall/ipc/socket.c"

static uint64_t now_ms;
static uint8_t sent_frame[2048];
static uint8_t sent_frames[128][2048];
static uint32_t sent_lengths[128];
static uint32_t sent_length;
static uint32_t sent_count;
static int send_error;
static int allocation_failure;
static int buffers;
static bool advance_poll_clock;
static struct task current_task;
static unsigned wait_queue_adds;
static unsigned wait_queue_removes;
static unsigned wait_queue_wakes;
static unsigned sleep_deadlines;
static unsigned network_poll_count;
void *kernel_malloc(size_t size)
{
    void *p = allocation_failure ? NULL : malloc(size);
    if (p) ++buffers;
    return p;
}
void kernel_free(void *p) { if (p) { --buffers; free(p); } }
uint64_t time_uptime_ms(void) { return now_ms; }
const uint8_t *e1000_mac(void) { static const uint8_t mac[6] = {2, 3, 4, 5, 6, 7}; return mac; }
void e1000_get_info(struct e1000_info *info) { *info = (struct e1000_info){.present=1, .active=1}; }
int e1000_send(const void *data, uint32_t length)
{
    assert(length <= sizeof(sent_frame));
    __builtin_memcpy(sent_frame, data, length);
    sent_length = length;
    uint32_t index = sent_count % 128u;
    __builtin_memcpy(sent_frames[index], data, length);
    sent_lengths[index] = length;
    ++sent_count;
    return send_error;
}
int e1000_poll(void *frame, uint32_t capacity, uint32_t *length)
{ (void)frame; (void)capacity; ++network_poll_count; *length = 0; if (advance_poll_clock) ++now_ms; return 0; }
void net_udp_input(uint32_t source, uint32_t destination, uint16_t source_port,
                   uint16_t destination_port, const void *data, uint32_t length, int ifindex)
{ (void)ifindex; (void)source; (void)destination; (void)source_port; (void)destination_port; (void)data; (void)length; }
void net_packet_input(const void *frame, uint32_t length) { (void)frame; (void)length; }
void console_printf(const char *format, ...) { (void)format; }

void console_log(enum console_log_level level, const char *format, ...)
{
    (void)level; (void)format;
}
bool user_range_ok(uint64_t address, uint64_t length)
{ return address != 0 && length != 0; }
bool user_range_writable(uint64_t address, uint64_t length)
{ return user_range_ok(address, length); }

struct task *sched_current_task(void) { return &current_task; }
uint64_t time_ticks(void) { return now_ms * RELIEFNT_TICK_HZ / 1000u; }
void sched_sleep_current_until(uint64_t tick) { (void)tick; ++sleep_deadlines; }
void sched_block_current(void) { assert(!"unexpected infinite wait in timeout test"); }
int kernel_wait_queue_add(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; assert(task == &current_task); ++wait_queue_adds; return 0; }
void kernel_wait_queue_remove(struct kernel_wait_queue *queue, struct task *task)
{ (void)queue; (void)task; ++wait_queue_removes; }
uint32_t kernel_wait_queue_wake_all(struct kernel_wait_queue *queue)
{ (void)queue; ++wait_queue_wakes; return 0; }
void kernel_wait_queue_init(struct kernel_wait_queue *queue)
{ __builtin_memset(queue, 0, sizeof(*queue)); }
void kernel_poll_readiness_notify(void) { }
int task_packet_send(struct task_file *file, const void *buffer, uint32_t length,
                     uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -LINUX_EOPNOTSUPP; }
int task_udp_send(struct task_file *file, const void *buffer, uint32_t length,
                  uint32_t flags, uint64_t address, uint32_t address_length)
{ (void)file; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -LINUX_EOPNOTSUPP; }
int task_packet_recv(struct task_file *file, void *buffer, uint32_t length,
                     uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -LINUX_EOPNOTSUPP; }
int task_udp_recv(struct task_file *file, void *buffer, uint32_t length,
                  uint32_t flags, uint64_t address, uint64_t address_length)
{ (void)file; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -LINUX_EOPNOTSUPP; }

static void test_tcp_peek(struct net_socket *s)
{
    uint32_t saved_head=s->rx_head,saved_length=s->rx_len;
    bool saved_fin=s->fin_received;
    s->rx_head=NET_SOCKET_RX_CAP-3;s->rx_len=8;s->fin_received=false;
    const unsigned char text[]="HTTP/1.0";
    for(unsigned n=0;n<8;++n)s->rx[(s->rx_head+n)%NET_SOCKET_RX_CAP]=text[n];
    struct task_file file={.flags=TASK_FILE_FLAG_SOCKET_INET,.aux=s->handle};
    unsigned before=wait_queue_adds,sent_before=sent_count;
    unsigned char data[16]={0};
    for(unsigned repeat=0;repeat<2;++repeat) {
        assert(task_inet_recv(&file,data,8,MSG_PEEK|MSG_DONTWAIT)==8);
        assert(!__builtin_memcmp(data,text,8) && s->rx_head==NET_SOCKET_RX_CAP-3 && s->rx_len==8);
        assert(sent_count==sent_before && !(file.flags&RELIEFOS_O_NONBLOCK));
    }
    assert(task_inet_recv(&file,data,8,MSG_TRUNC)==-LINUX_EOPNOTSUPP && s->rx_len==8);
    assert(task_inet_read(&file,data,8)==8 && !__builtin_memcmp(data,text,8) && !s->rx_len);
    assert(task_inet_recv(&file,data,8,MSG_PEEK|MSG_DONTWAIT)==-RELIEFOS_EAGAIN);
    assert(wait_queue_adds==before && !(file.flags&RELIEFOS_O_NONBLOCK));
    s->fin_received=true;
    assert(task_inet_recv(&file,data,8,MSG_PEEK|MSG_DONTWAIT)==0);
    assert(task_inet_recv(&file,NULL,0,MSG_PEEK)==0);
    s->rx_head=saved_head;s->rx_len=saved_length;s->fin_received=saved_fin;
}

static void test_small_reads(struct net_socket *s)
{
    s->remote_seq += NET_SOCKET_RX_CAP - s->rx_len;
    s->rx_len = NET_SOCKET_RX_CAP;
    for (unsigned i = 0; i < NET_SOCKET_RX_CAP; ++i) s->rx[i] = i % 251;
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    assert(net_get_u16(sent_frame + 48) == 0);
    uint8_t data[NET_SOCKET_RX_CAP];
    struct reliefos_net_socket_io request = {.socket=s->handle, .buffer=data, .length=5};
    unsigned before = sent_count;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == 5);
    assert(sent_count == before); /* A TLS header must not reopen a five-byte window. */
    for (unsigned i = 0; i < 5; ++i) assert(data[i] == i);
    request.length = NET_TCP_MSS - 5;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == NET_TCP_MSS - 5);
    assert(sent_count == before + 1 && net_get_u16(sent_frame + 48) >= NET_TCP_MSS);
    for (unsigned i = 0; i < request.transferred; ++i) assert(data[i] == (i + 5) % 251);
    request.length = NET_SOCKET_RX_CAP;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == NET_SOCKET_RX_CAP - NET_TCP_MSS);
    for (unsigned i = 0; i < request.transferred; ++i) assert(data[i] == (i + NET_TCP_MSS) % 251);
    /* Fill and drain across the end of the receive storage. */
    uint8_t incoming[NET_TCP_MSS];
    for (unsigned i = 0; i < sizeof(incoming); ++i) incoming[i] = i % 251;
    for (unsigned round = 0; round < 100; ++round) {
        net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
            s->remote_seq, s->local_seq, 65535, TCP_FLAG_ACK, incoming, sizeof(incoming));
        request.length = 17;
        assert(net_socket_recv(&request, 0) == 0 && request.transferred == 17);
        assert(!__builtin_memcmp(data, incoming, 17));
        request.length = sizeof(data);
        assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(incoming) - 17);
        assert(!__builtin_memcmp(data, incoming + 17, sizeof(incoming) - 17));
    }
}

static void test_window_edges(struct net_socket *s)
{
    s->remote_seq = 0xfffffff0u;
    s->rx_window_valid = false;
    assert(!s->rx_len);
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    uint32_t end = net_get_u32(sent_frame + 42) + net_get_u16(sent_frame + 48);
    uint8_t input[20] = {0};
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
        s->remote_seq, s->local_seq, 65535, TCP_FLAG_ACK, input, sizeof(input));
    assert(s->remote_seq == 4);
    uint32_t next_end = net_get_u32(sent_frame + 42) + net_get_u16(sent_frame + 48);
    assert((int32_t)(next_end - end) >= 0);
    assert(net_get_u16(sent_frame + 48) <= NET_SOCKET_RX_CAP - s->rx_len);
    s->remote_seq += NET_SOCKET_RX_CAP - s->rx_len;
    s->rx_len = NET_SOCKET_RX_CAP;
    assert(net_send_tcp_to_mac(s->dst_mac, s->local_ip, s->remote_ip,
        s->local_port, s->remote_port, s->local_seq, s->remote_seq, TCP_FLAG_ACK, 0, 0) == 0);
    assert(net_get_u16(sent_frame + 48) == 0);
    uint8_t data[NET_TCP_MSS];
    struct reliefos_net_socket_io request = {.socket=s->handle, .buffer=data, .length=sizeof(data)};
    send_error = -1;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(data));
    assert(net_socket_receive_window(s) == 0); /* Failed TX must not publish state. */
    send_error = 0;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == sizeof(data));
    assert(net_get_u16(sent_frame + 48) == 2 * sizeof(data));
}

static void test_nonblocking_tcp_send_without_ack(void)
{
    struct net_socket *socket = net_socket_alloc(7, 1000);
    struct task_file file = {0};
    uint8_t payload[70000];
    unsigned sent_before;
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51001;
    socket->remote_port = 8000;
    socket->local_seq = 9000;
    socket->acked_seq = socket->local_seq;
    socket->remote_seq = 12000;
    socket->peer_window = 65535;
    socket->peer_window_valid = true;
    socket->tx_cwnd = 3u * NET_TCP_MSS;
    file.flags = TASK_FILE_FLAG_SOCKET_INET | RELIEFOS_O_NONBLOCK;
    file.aux = (uint32_t)socket->handle;
    for (unsigned i = 0; i < sizeof(payload); ++i) payload[i] = (uint8_t)(i % 251u);

    advance_poll_clock = false;
    sent_before = sent_count;
    int written = task_inet_write(&file, payload, sizeof(payload));
    assert(written > 0 && (uint32_t)written < sizeof(payload));
    assert(written == NET_SOCKET_TX_CAP);
    assert(sent_count == sent_before + 3);
    for (unsigned i = 0; i < 3; ++i) {
        uint8_t *frame = sent_frames[(sent_before + i) % 128u];
        assert(sent_lengths[(sent_before + i) % 128u] == 14u + 20u + 20u + NET_TCP_MSS);
        assert(net_get_u32(frame + 38) == 9000u + i * NET_TCP_MSS);
        assert(!__builtin_memcmp(frame + 54, payload + i * NET_TCP_MSS, NET_TCP_MSS));
    }
    assert(socket->tx_sent == 3u * NET_TCP_MSS);

    assert(task_inet_write(&file, payload, 1) == -LINUX_EAGAIN);
    uint32_t first_seq = 9000;
    now_ms += NET_TCP_INITIAL_RTO_MS;
    net_poll_packets();
    assert(sent_count == sent_before + 4);
    assert(net_get_u32(sent_frame + 38) == first_seq);
    assert(net_get_u16(sent_frame + 16) - 40u == NET_TCP_MSS);
    assert(!__builtin_memcmp(sent_frame + 54, payload, NET_TCP_MSS));

    uint16_t partial_ack = 100;
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
                          socket->local_port, socket->remote_seq,
                          first_seq + partial_ack, 65535, TCP_FLAG_ACK, NULL, 0);
    assert(socket->acked_seq == first_seq + partial_ack);
    assert(socket->tx_sent == 3u * NET_TCP_MSS - partial_ack);
    assert(socket->tx_len == NET_SOCKET_TX_CAP - partial_ack);
    net_poll_packets();
    assert(sent_count == sent_before + 4);

    now_ms += NET_TCP_INITIAL_RTO_MS;
    net_poll_packets();
    assert(sent_count == sent_before + 5);
    assert(net_get_u32(sent_frame + 38) == first_seq + partial_ack);
    assert(net_get_u16(sent_frame + 16) - 40u == NET_TCP_MSS);
    assert(!__builtin_memcmp(sent_frame + 54, payload + partial_ack,
                             NET_TCP_MSS));
    now_ms += NET_TCP_INITIAL_RTO_MS * 2u - 1u;
    net_poll_packets();
    assert(sent_count == sent_before + 5);
    ++now_ms;
    net_poll_packets();
    assert(sent_count == sent_before + 6);
    assert(net_get_u32(sent_frame + 38) == first_seq + partial_ack);
    assert(net_get_u16(sent_frame + 16) - 40u == NET_TCP_MSS);

    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
                          socket->local_port, socket->remote_seq,
                          socket->local_seq, 65535, TCP_FLAG_ACK, NULL, 0);
    net_poll_packets();
    assert(socket->tx_sent == 2u * NET_TCP_MSS);
    uint32_t queue_room = NET_SOCKET_TX_CAP - socket->tx_len;
    assert(task_inet_write(&file, payload, NET_SOCKET_TX_CAP) == (int)queue_room);
    assert(socket->tx_len == NET_SOCKET_TX_CAP);
    file.flags &= ~RELIEFOS_O_NONBLOCK;
    assert(!(net_socket_poll_fd(socket->handle, 0, POLLOUT) & POLLOUT));
    unsigned adds_before = wait_queue_adds;
    unsigned sleeps_before = sleep_deadlines;
    assert(task_inet_write(&file, payload, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(wait_queue_adds == adds_before + 1 && sleep_deadlines == sleeps_before + 1);
    uint32_t ack = socket->acked_seq + 1u;
    unsigned wakes_before = wait_queue_wakes;
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
                          socket->local_port, socket->remote_seq,
                          ack, 65535, TCP_FLAG_ACK, NULL, 0);
    assert(wait_queue_wakes > wakes_before);
    assert(socket->tx_len == NET_SOCKET_TX_CAP - 1u);
    assert(net_socket_poll_fd(socket->handle, 0, POLLOUT) & POLLOUT);
    assert(task_inet_write(&file, payload, 1) == 1);
    assert(wait_queue_removes > 0);
    net_socket_clear(socket);
    advance_poll_clock = false;
}

static void test_tcp_peer_window_and_persist(void)
{
    struct net_socket *socket = net_socket_alloc(8, 1000);
    struct reliefos_net_socket_io request = {0};
    uint8_t payload[5000];
    unsigned before;
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51002;
    socket->remote_port = 8001;
    socket->local_seq = socket->acked_seq = 12000;
    socket->remote_seq = 22000;
    socket->peer_window = 2000;
    socket->peer_window_valid = true;
    socket->tx_cwnd = 3u * NET_TCP_MSS;
    __builtin_memset(payload, 0x5a, sizeof(payload));
    request.socket = socket->handle;
    request.buffer = payload;
    request.length = sizeof(payload);
    before = sent_count;
    assert(net_socket_send(&request, 0) == 0 && request.transferred == sizeof(payload));
    assert(sent_count == before + 2);
    assert(socket->tx_sent == 2000);
    assert(net_get_u32(sent_frames[before % 128u] + 38) == 12000);
    assert(net_get_u32(sent_frames[(before + 1u) % 128u] + 38) ==
           12000u + NET_TCP_MSS);
    assert(net_get_u16(sent_frames[(before + 1u) % 128u] + 16) - 40u ==
           2000u - NET_TCP_MSS);
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->acked_seq + 100u,
        0, TCP_FLAG_ACK, NULL, 0);
    net_poll_packets();
    assert(socket->peer_window == 0 && socket->tx_sent == 1900);

    net_socket_clear(socket);
    socket = net_socket_alloc(8, 1000);
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51003;
    socket->remote_port = 8002;
    socket->local_seq = socket->acked_seq = 33000;
    socket->remote_seq = 44000;
    socket->peer_window = 0;
    socket->peer_window_valid = true;
    socket->tx_cwnd = 3u * NET_TCP_MSS;
    request.socket = socket->handle;
    request.buffer = payload;
    request.length = 8;
    before = sent_count;
    assert(net_socket_send(&request, 0) == 0 && request.transferred == 8);
    assert(sent_count == before);
    now_ms += NET_TCP_INITIAL_RTO_MS - 1u;
    net_poll_packets();
    assert(sent_count == before);
    ++now_ms;
    net_poll_packets();
    assert(sent_count == before + 1);
    assert(net_get_u32(sent_frame + 38) == 33000);
    assert(net_get_u16(sent_frame + 16) - 40u == 1);
    assert(sent_frame[54] == payload[0]);
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->acked_seq,
        0, TCP_FLAG_ACK, NULL, 0);
    now_ms += NET_TCP_INITIAL_RTO_MS;
    net_poll_packets();
    assert(sent_count == before + 2);
    assert(net_get_u32(sent_frame + 38) == 33000);
    assert(net_get_u16(sent_frame + 16) - 40u == 1);
    now_ms += NET_TCP_INITIAL_RTO_MS * 2u - 1u;
    net_poll_packets();
    assert(sent_count == before + 2);
    ++now_ms;
    net_poll_packets();
    assert(sent_count == before + 3);
    assert(net_get_u32(sent_frame + 38) == 33000);
    assert(net_get_u16(sent_frame + 16) - 40u == 1);
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->acked_seq,
        4096, TCP_FLAG_ACK, NULL, 0);
    net_poll_packets();
    assert(socket->peer_window == 4096 && socket->tx_sent == 8);
    assert(sent_count == before + 4);
    assert(net_get_u32(sent_frame + 38) == 33001);
    assert(net_get_u16(sent_frame + 16) - 40u == 7);
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->local_seq,
        4096, TCP_FLAG_ACK, NULL, 0);
    assert(socket->tx_sent == 0 && socket->tx_len == 0);
    net_socket_clear(socket);
}

static void test_tcp_timeouts_and_read_errors(void)
{
    struct net_socket *socket = net_socket_alloc(9, 1000);
    struct task_file file = {0};
    struct linux_timeval timeout = {.tv_sec = 0, .tv_usec = 25000};
    struct linux_timeval observed = {0};
    socklen_t option_length = sizeof(timeout);
    uint8_t byte = 0;
    assert(socket);
    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51004;
    socket->remote_port = 8003;
    socket->local_seq = socket->acked_seq = 55000;
    socket->remote_seq = 66000;
    socket->peer_window_valid = true;
    socket->peer_window = 65535;
    file.flags = TASK_FILE_FLAG_SOCKET | TASK_FILE_FLAG_SOCKET_INET | RELIEFOS_O_RDWR;
    file.aux = (uint32_t)socket->handle;

    assert(net_socket_get_timeout_ticks(socket->handle, true) == UINT64_MAX);
    assert(net_socket_get_timeout_ticks(socket->handle, false) == UINT64_MAX);
    assert(inet_socket_option(true, SOL_SOCKET, SO_SNDTIMEO,
                              (uintptr_t)&timeout, sizeof(timeout),
                              socket->handle) == 0);
    assert(net_socket_get_timeout_ticks(socket->handle, true) == 3);
    assert(inet_socket_option(false, SOL_SOCKET, SO_SNDTIMEO,
                              (uintptr_t)&observed, (uintptr_t)&option_length,
                              socket->handle) == 0);
    assert(observed.tv_sec == 0 && observed.tv_usec == 30000);
    timeout = (struct linux_timeval){0, 0};
    assert(inet_socket_option(true, SOL_SOCKET, SO_SNDTIMEO_NEW,
                              (uintptr_t)&timeout, sizeof(timeout),
                              socket->handle) == 0);
    assert(net_socket_get_timeout_ticks(socket->handle, true) == UINT64_MAX);

    socket->tx_len = NET_SOCKET_TX_CAP;
    timeout = (struct linux_timeval){0, 30000};
    assert(inet_socket_option(true, SOL_SOCKET, SO_SNDTIMEO,
                              (uintptr_t)&timeout, sizeof(timeout),
                              socket->handle) == 0);
    unsigned sleeps_before = sleep_deadlines;
    assert(net_socket_wait_writable(socket->handle) == 1);
    assert(sleep_deadlines == sleeps_before + 1);
    now_ms += 30;
    assert(net_socket_wait_writable(socket->handle) == -LINUX_EAGAIN);
    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;
    socket->tx_len = 0;

    timeout = (struct linux_timeval){0, 30000};
    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
                              (uintptr_t)&timeout, sizeof(timeout),
                              socket->handle) == 0);
    file.flags &= ~RELIEFOS_O_NONBLOCK;
    sleeps_before = sleep_deadlines;
    assert(task_inet_read(&file, &byte, 1) == KERNEL_SYSCALL_BLOCKED);
    assert(sleep_deadlines == sleeps_before + 1);
    now_ms += 30;
    assert(task_inet_read(&file, &byte, 1) == -LINUX_EAGAIN);
    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;

    socket->state = RELIEFOS_NET_TCP_CLOSED;
    socket->status = RELIEFOS_NET_STATUS_TCP_RESET;
    socket->error = LINUX_ECONNRESET;
    assert(task_inet_read(&file, &byte, 1) == -LINUX_ECONNRESET);
    net_socket_clear(socket);
}

static void test_negative_socket_timeout_seconds_clamp_to_immediate(void)
{
    struct net_socket *inet = net_socket_alloc(35, 1000);
    struct kernel_wait_queue unix_wait = {0};
    struct task_file unix_file = {0};
    struct unix_socket unix = {
        .receive_timeout = UINT64_MAX,
        .send_timeout = UINT64_MAX,
    };
    struct linux_timeval timeout = {.tv_sec = -1, .tv_usec = 500000};
    struct linux_timeval observed = {0};
    socklen_t option_length = sizeof(observed);
    unsigned sleeps_before;
    assert(inet);
    inet->state = RELIEFOS_NET_TCP_ESTABLISHED;
    inet->local_ip = 0x0a25000f;
    inet->remote_ip = 0x0a250002;
    inet->local_port = 51015;
    inet->remote_port = 8014;
    inet->local_seq = inet->acked_seq = 94000;
    inet->remote_seq = 104000;

    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout), inet->handle) == 0);
    assert(net_socket_get_timeout_ticks(inet->handle, false) == 0);
    assert(inet_socket_option(false, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&observed, (uintptr_t)&option_length, inet->handle) == 0);
    assert(observed.tv_sec == 0 && observed.tv_usec == 0);

    timeout = (struct linux_timeval){-1, 500000};
    assert(unix_socket_option(&unix, true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout)) == 0);
    assert(unix.receive_timeout == 0);
    option_length = sizeof(observed);
    observed = (struct linux_timeval){1, 1};
    assert(unix_socket_option(&unix, false, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&observed, (uintptr_t)&option_length) == 0);
    assert(observed.tv_sec == 0 && observed.tv_usec == 0);

    timeout = (struct linux_timeval){0, -1};
    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout), inet->handle) == -LINUX_EDOM);
    assert(unix_socket_option(&unix, true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout)) == -LINUX_EDOM);
    timeout = (struct linux_timeval){0, 1000000};
    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout), inet->handle) == -LINUX_EDOM);
    assert(unix_socket_option(&unix, true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout)) == -LINUX_EDOM);

    /* The all-zero form remains infinite even though the negative form is
     * exposed by getsockopt as the same (0, 0) timeval. */
    timeout = (struct linux_timeval){0, 0};
    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout), inet->handle) == 0);
    assert(net_socket_get_timeout_ticks(inet->handle, false) == UINT64_MAX);
    assert(unix_socket_option(&unix, true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout)) == 0);
    assert(unix.receive_timeout == UINT64_MAX);

    timeout = (struct linux_timeval){-1, 0};
    assert(inet_socket_option(true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout), inet->handle) == 0);
    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;
    sleeps_before = sleep_deadlines;
    assert(net_socket_wait_readable(inet->handle) == -LINUX_EAGAIN);
    assert(current_task.socket_io_timed && sleep_deadlines == sleeps_before);

    assert(unix_socket_option(&unix, true, SOL_SOCKET, SO_RCVTIMEO,
        (uintptr_t)&timeout, sizeof(timeout)) == 0);
    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;
    assert(unix_block(&unix, &unix_file, &unix_wait, true, 0) == -LINUX_EAGAIN);
    assert(current_task.socket_io_timed && sleep_deadlines == sleeps_before);

    current_task.socket_io_deadline = 0;
    current_task.socket_io_timed = false;
    net_socket_clear(inet);
}

static void test_socket_registry_growth_and_legacy_snapshot(void)
{
    struct net_socket *sockets[24];
    struct reliefos_net_connection_info snapshot[RELIEFOS_NET_SOCKET_MAX];
    struct reliefos_net_connection_list request = {
        .capacity = RELIEFOS_NET_SOCKET_MAX,
        .entries = snapshot,
    };
    struct task viewer = {.role = RELIEFOS_AUTH_ROLE_ADMIN};
    for (unsigned i = 0; i < 24; ++i) {
        sockets[i] = net_socket_alloc(100u + i, 1000u);
        assert(sockets[i]);
        for (unsigned j = 0; j < i; ++j) {
            assert(sockets[i] != sockets[j]);
            assert(sockets[i]->handle != sockets[j]->handle);
        }
        net_socket_pin_fd(sockets[i]->handle);
    }
    assert(net_connections(&request, &viewer) == 0);
    assert(request.count == 24);
    assert(snapshot[0].socket == sockets[0]->handle);
    assert(snapshot[RELIEFOS_NET_SOCKET_MAX - 1].socket ==
           sockets[RELIEFOS_NET_SOCKET_MAX - 1]->handle);
    for (unsigned i = 0; i < 24; ++i) net_socket_release_fd(sockets[i]->handle);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(buffers == 0);
}

static void test_tcp_release_then_owner_exit_drains_queued_bytes(void)
{
    const uint32_t owner_pid = 27;
    uint8_t payload[NET_TCP_MSS * 4u + 37u];
    struct net_socket *socket = net_socket_alloc(owner_pid, 1000);
    struct task_file file = {0};
    struct reliefos_net_socket_io request = {0};
    uint32_t initial_seq = 71000;
    uint32_t length = sizeof(payload);
    uint32_t verified = 0;
    unsigned sent_before;
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51010;
    socket->remote_port = 8009;
    socket->local_seq = socket->acked_seq = initial_seq;
    socket->remote_seq = 81000;
    socket->peer_window = 65535;
    socket->peer_window_valid = true;
    socket->tx_cwnd = 3u * NET_TCP_MSS;
    net_socket_pin_fd(socket->handle);
    file.flags = TASK_FILE_FLAG_SOCKET_INET | RELIEFOS_O_NONBLOCK;
    file.aux = (uint32_t)socket->handle;
    for (uint32_t i = 0; i < length; ++i) payload[i] = (uint8_t)(i % 251u);

    sent_before = sent_count;
    assert(task_inet_write(&file, payload, length) == (int)length);
    assert(socket->tx_len == length && socket->tx_sent > 0 &&
           socket->tx_sent < socket->tx_len);
    net_socket_release_fd(socket->handle);
    assert(!socket->fd_owned && socket->fin_pending);
    net_close_owner_sockets(owner_pid);
    assert(socket->state == RELIEFOS_NET_TCP_ESTABLISHED);
    assert(socket->tx_len == length && socket->tx_sent > 0);

    unsigned maintenance_sent = sent_count;
    for (unsigned retries = 0; socket->tx_len && retries < 16; ++retries) {
        uint32_t ack;
        assert(socket->tx_sent > 0);
        ack = socket->acked_seq + socket->tx_sent;
        net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
            socket->local_port, socket->remote_seq, ack, 65535,
            TCP_FLAG_ACK, NULL, 0);
        /* Simulate a timer boundary after process exit. No descriptor or
         * socket syscall is made to advance the accepted queue or FIN. */
        now_ms += 10;
        net_tcp_maintenance();
        assert(sent_count > maintenance_sent);
        maintenance_sent = sent_count;
    }
    assert(socket->tx_len == 0 && socket->tx_sent == 0);
    assert(socket->acked_seq == initial_seq + length);
    assert(socket->tx_bytes == length);
    assert(socket->fin_sent && socket->state == RELIEFOS_NET_TCP_TIME_WAIT);
    net_close_owner_sockets(owner_pid);
    assert(socket->state == RELIEFOS_NET_TCP_TIME_WAIT);

    for (unsigned i = sent_before; i < sent_count; ++i) {
        uint32_t index = i % 128u;
        uint8_t *frame = sent_frames[index];
        uint32_t frame_payload = net_get_u16(frame + 16) - 40u;
        if (frame[47] & TCP_FLAG_FIN) {
            assert(verified == length);
            continue;
        }
        assert(frame_payload > 0 && verified + frame_payload <= length);
        assert(net_get_u32(frame + 38) == initial_seq + verified);
        assert(!__builtin_memcmp(frame + 54, payload + verified, frame_payload));
        verified += frame_payload;
    }
    assert(verified == length);
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->local_seq, 65535,
        TCP_FLAG_ACK, NULL, 0);
    net_tcp_service_sockets();
    assert(socket->fin_acked);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(buffers == 0);
}

static void test_tcp_fin_retransmit_ack_and_half_close(void)
{
    struct net_socket *socket = net_socket_alloc(31, 1000);
    struct reliefos_net_socket_io request = {0};
    uint8_t received[8] = {0};
    uint8_t peer_data[] = {0x41, 0x42, 0x43};
    unsigned before;
    uint32_t fin_sequence;
    int32_t handle;
    assert(socket);
    handle = socket->handle;
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51011;
    socket->remote_port = 8010;
    socket->local_seq = socket->acked_seq = 91000;
    socket->remote_seq = 101000;
    socket->peer_window = 65535;
    socket->peer_window_valid = true;
    net_socket_pin_fd(socket->handle);

    /* A write-side half-close still permits the peer's response stream. */
    assert(net_socket_shutdown_fd(socket->handle, SHUT_WR) == 0);
    assert(socket->fin_pending && socket->fin_sent);
    assert(socket->state == RELIEFOS_NET_TCP_TIME_WAIT);
    fin_sequence = socket->local_seq - 1u;
    assert(net_tcp_maintenance_pending());
    assert(!(net_socket_poll_fd(socket->handle, 0, POLLIN) & POLLHUP));

    /* Dropping the original FIN must retransmit the same sequence byte. */
    before = sent_count;
    now_ms += NET_TCP_INITIAL_RTO_MS - 1u;
    net_tcp_service_sockets();
    assert(sent_count == before);
    ++now_ms;
    net_tcp_service_sockets();
    assert(sent_count == before + 1u);
    assert(sent_frame[47] & TCP_FLAG_FIN);
    assert(net_get_u32(sent_frame + 38) == fin_sequence);

    /* A duplicate ACK that omits FIN must leave it outstanding for RTO. */
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, fin_sequence, 65535,
        TCP_FLAG_ACK, NULL, 0);
    assert(net_tcp_maintenance_pending());
    now_ms += NET_TCP_INITIAL_RTO_MS * 2u - 1u;
    net_tcp_service_sockets();
    assert(sent_count == before + 1u);
    ++now_ms;
    net_tcp_service_sockets();
    assert(sent_count == before + 2u);
    assert((sent_frame[47] & TCP_FLAG_FIN) &&
           net_get_u32(sent_frame + 38) == fin_sequence);

    /* An ACK-only segment may complete the FIN without inventing EOF. */
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, fin_sequence + 1u, 65535,
        TCP_FLAG_ACK, NULL, 0);
    assert(socket->fin_acked && socket->rx_len == 0);
    net_tcp_service_sockets();
    assert(!net_tcp_maintenance_pending());
    assert(!(net_socket_poll_fd(socket->handle, 0, POLLHUP) & POLLHUP));

    /* The read half remains active after the local FIN is acknowledged. */
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, fin_sequence + 1u, 65535,
        TCP_FLAG_ACK, peer_data, sizeof(peer_data));
    assert(socket->rx_len == sizeof(peer_data));
    assert(socket->fin_received == 0);
    assert(net_socket_poll_fd(socket->handle, 0, POLLIN) & POLLIN);
    assert(!(net_socket_poll_fd(socket->handle, 0, POLLIN) & POLLHUP));
    request.socket = socket->handle;
    request.buffer = received;
    request.length = sizeof(received);
    assert(net_socket_recv(&request, 0) == 0);
    assert(request.transferred == sizeof(peer_data));
    assert(!__builtin_memcmp(received, peer_data, sizeof(peer_data)));
    assert(!(net_socket_poll_fd(socket->handle, 0, POLLIN) & POLLHUP));

    /* A peer FIN is the only event that turns the read side into EOF. */
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, fin_sequence + 1u, 65535,
        TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    assert(socket->fin_received && socket->state == RELIEFOS_NET_TCP_TIME_WAIT);
    assert((net_socket_poll_fd(socket->handle, 0, POLLIN) &
            (POLLIN | POLLHUP)) == (POLLIN | POLLHUP));
    request.transferred = 99;
    request.length = 1;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == 0);

    /* Close ownership and age the hold while FIN ACK was already received. */
    net_socket_release_fd(socket->handle);
    assert(!socket->fd_owned);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(!net_socket_find(handle, 0, 1));
    assert(buffers == 0);
}

static void test_tcp_peer_half_close_allows_reply(void)
{
    struct net_socket *socket = net_socket_alloc(34, 1000);
    struct reliefos_net_socket_io request = {0};
    uint8_t reply[] = {0x51, 0x52, 0x53, 0x54};
    uint8_t received = 0;
    int32_t handle;
    uint32_t reply_sequence;
    assert(socket);
    handle = socket->handle;
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51014;
    socket->remote_port = 8013;
    socket->local_seq = socket->acked_seq = 93000;
    socket->remote_seq = 103000;
    socket->peer_window = 65535;
    socket->peer_window_valid = true;
    net_socket_pin_fd(handle);

    /* Peer SHUT_WR gives EOF on reads while leaving our write side open. */
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->local_seq, 65535,
        TCP_FLAG_FIN | TCP_FLAG_ACK, NULL, 0);
    assert(socket->fin_received && socket->state == RELIEFOS_NET_TCP_TIME_WAIT);
    request.socket = handle;
    request.buffer = &received;
    request.length = 1;
    assert(net_socket_recv(&request, 0) == 0 && request.transferred == 0);
    short ready = net_socket_poll_fd(handle, 0,
                                     POLLIN | POLLOUT | POLLRDHUP);
    assert((ready & (POLLIN | POLLOUT | POLLRDHUP)) ==
           (POLLIN | POLLOUT | POLLRDHUP));
    assert(!(ready & POLLHUP));

    request.buffer = reply;
    request.length = sizeof(reply);
    reply_sequence = socket->local_seq;
    assert(net_socket_send(&request, 0) == 0);
    assert(request.status == RELIEFOS_NET_STATUS_OK);
    assert(request.transferred == sizeof(reply));
    now_ms += 10;
    net_tcp_maintenance();
    assert(socket->tx_sent == sizeof(reply));
    assert(net_get_u32(sent_frame + 38) == reply_sequence);
    assert(net_get_u16(sent_frame + 16) - 40u == sizeof(reply));
    assert(!__builtin_memcmp(sent_frame + 54, reply, sizeof(reply)));

    /* Our own SHUT_WR queues a FIN after the accepted reply bytes. */
    assert(net_socket_shutdown_fd(handle, SHUT_WR) == 0);
    assert(socket->shutdown_write && socket->fin_pending && !socket->fin_sent);
    ready = net_socket_poll_fd(handle, 0, POLLIN | POLLOUT | POLLRDHUP);
    assert((ready & (POLLIN | POLLOUT | POLLRDHUP | POLLHUP)) ==
           (POLLIN | POLLOUT | POLLRDHUP | POLLHUP));
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->local_seq, 65535,
        TCP_FLAG_ACK, NULL, 0);
    assert(socket->tx_len == 0 && socket->tx_sent == 0);
    now_ms += 10;
    net_tcp_maintenance();
    assert(socket->fin_sent && !socket->fin_acked);
    assert(sent_frame[47] & TCP_FLAG_FIN);
    assert(net_get_u32(sent_frame + 38) == reply_sequence + sizeof(reply));

    /* The FIN remains active until acknowledged, then owner exit can collect it. */
    assert(net_tcp_maintenance_pending());
    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq, socket->local_seq, 65535,
        TCP_FLAG_ACK, NULL, 0);
    assert(socket->fin_acked);
    now_ms += 10;
    net_tcp_maintenance();
    assert(!net_tcp_maintenance_pending());
    net_socket_release_fd(handle);
    net_close_owner_sockets(34);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(!net_socket_find(handle, 0, 1));
    assert(buffers == 0);
}

static void test_tcp_unacknowledged_fin_is_not_reclaimed(void)
{
    struct net_socket *socket = net_socket_alloc(32, 1000);
    int32_t handle;
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51012;
    socket->remote_port = 8011;
    socket->local_seq = socket->acked_seq = 92000;
    socket->remote_seq = 102000;
    socket->peer_window = 65535;
    socket->peer_window_valid = true;
    assert(net_socket_shutdown_fd(socket->handle, SHUT_WR) == 0);
    assert(socket->fin_sent && socket->tx_sent == 0);
    handle = socket->handle;
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(socket->used && net_socket_find(handle, 0, 1) == socket);
    assert(net_tcp_maintenance_pending());
    /* FIN retry exhaustion is an explicit protocol failure and may retire it. */
    socket->tx_retries = NET_TCP_MAX_RETRANSMITS;
    socket->tx_rto_ms = NET_TCP_MAX_RTO_MS;
    socket->tx_retransmit_deadline = now_ms;
    net_tcp_service_sockets();
    assert(socket->state == RELIEFOS_NET_TCP_CLOSED);
    assert(socket->error == LINUX_ETIMEDOUT);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1u;
    net_socket_gc();
    assert(buffers == 0);
}

static void test_tcp_out_of_window_segment_cannot_ack_data(void)
{
    struct net_socket *socket = net_socket_alloc(33, 1000);
    uint32_t original_seq;
    uint32_t original_ack;
    uint32_t original_window;
    assert(socket);
    socket->state = RELIEFOS_NET_TCP_ESTABLISHED;
    socket->local_ip = 0x0a25000f;
    socket->remote_ip = 0x0a250002;
    socket->local_port = 51013;
    socket->remote_port = 8012;
    socket->local_seq = 30008;
    socket->acked_seq = 30000;
    socket->remote_seq = 40000;
    socket->peer_window = 2345;
    socket->peer_window_valid = true;
    socket->tx_len = 8;
    socket->tx_sent = 8;
    socket->tx_retransmit_deadline = now_ms + NET_TCP_INITIAL_RTO_MS;
    net_socket_work_schedule(socket);
    original_seq = socket->local_seq;
    original_ack = socket->acked_seq;
    original_window = socket->peer_window;

    net_socket_handle_tcp(socket->remote_ip, socket->remote_port,
        socket->local_port, socket->remote_seq + NET_SOCKET_RX_CAP + 1u,
        socket->local_seq, 0, TCP_FLAG_ACK, NULL, 0);

    assert(socket->local_seq == original_seq);
    assert(socket->acked_seq == original_ack);
    assert(socket->tx_len == 8 && socket->tx_sent == 8);
    assert(socket->peer_window == original_window);
    net_socket_clear(socket);
}

int main(void)
{
    struct net_socket *s = net_socket_alloc(7, 1000);
    bool interface_was_up = net_interface_up;
    assert(s);
    net_interface_up = true;
    net_tcp_maintenance();
    assert(network_poll_count == 0); /* No work means no periodic NIC polling. */
    net_interface_up = interface_was_up;
    /* An open descriptor must survive idle GC and table pressure. */
    net_socket_pin_fd(s->handle);
    int handle = s->handle;
    now_ms = NET_SOCKET_CLOSE_HOLD_MS + 1;
    net_socket_gc();
    assert(net_socket_find(handle, 0, 1) == s);
    s->state = RELIEFOS_NET_TCP_ESTABLISHED;
    s->local_ip = 0x0a25000f; s->remote_ip = 0x0a250002;
    s->local_port = 51000; s->remote_port = 8000;
    s->remote_seq = 500; s->local_seq = s->acked_seq = 100;
    s->rx_len = NET_SOCKET_RX_CAP - 2;
    const uint8_t bytes[] = {1, 2, 3, 4};
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
                          500, 100, 65535, TCP_FLAG_ACK, bytes, sizeof(bytes));
    assert(s->rx_len == NET_SOCKET_RX_CAP);
    assert(s->remote_seq == 502);
    assert(sent_length >= 54 && net_get_u16(sent_frame + 48) == 0);
    s->rx_len = 0;
    s->rx_window_valid = false; /* The fixture manually drained the receive ring. */
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
                          500, 100, 65535, TCP_FLAG_ACK, bytes, sizeof(bytes));
    assert(s->rx_len == 2 && s->remote_seq == 504 && s->rx[0] == 3 && s->rx[1] == 4);
    test_tcp_peek(s);
    test_small_reads(s);
    test_window_edges(s);
    net_close_owner_sockets(7);
    assert(s->state == RELIEFOS_NET_TCP_ESTABLISHED);
    net_socket_release_fd(handle);
    assert(!s->fd_owned);
    assert(s->fin_sent && !s->fin_acked);
    net_socket_handle_tcp(s->remote_ip, s->remote_port, s->local_port,
                          s->remote_seq, s->local_seq, 65535,
                          TCP_FLAG_ACK, NULL, 0);
    net_tcp_service_sockets();
    assert(s->fin_acked);
    struct net_dhcp_offer lease = {.yiaddr=0x0a25000f, .subnet_mask=0xffffff00,
        .router_ip=0x0a250002, .dns_ip=0x0a250003, .server_ip=0x0a250002, .lease_seconds=10};
    net_apply_dhcp_offer(&lease);
    struct reliefos_net_config config;
    now_ms += 3000;
    net_get_config(&config);
    assert(config.lease_seconds == 7 && config.dns_ip == lease.dns_ip);
    now_ms += 7000;
    net_get_config(&config);
    assert(!config.local_ip && !config.dns_ip && config.source == RELIEFOS_NET_CONFIG_SOURCE_NONE);
    now_ms += NET_SOCKET_CLOSE_HOLD_MS + 1;
    net_socket_gc();
    assert(buffers == 0);
    allocation_failure = 1;
    assert(net_socket_alloc(7, 1000) == NULL && buffers == 0);
    allocation_failure = 0;
    s = net_socket_alloc(7, 1000);
    assert(s && buffers == 3);
    net_socket_clear(s);
    assert(buffers == 0);
    test_nonblocking_tcp_send_without_ack();
    test_tcp_peer_window_and_persist();
    test_tcp_timeouts_and_read_errors();
    test_negative_socket_timeout_seconds_clamp_to_immediate();
    test_socket_registry_growth_and_legacy_snapshot();
    test_tcp_release_then_owner_exit_drains_queued_bytes();
    test_tcp_fin_retransmit_ack_and_half_close();
    test_tcp_peer_half_close_allows_reply();
    test_tcp_unacknowledged_fin_is_not_reclaimed();
    test_tcp_out_of_window_segment_cannot_ack_data();
    puts("[tcp-state] PASS socket lifetime, queued TCP, congestion/window flow, retransmit and partial ACK");
    return 0;
}

/* Real counted UNIX address parsing runs in this fixture. Only filesystem
 * endpoint resolution is outside its scope and must abort if ever reached;
 * actual inode/DAC/bind cases live in mount_backend_test and the native probe. */
int syscall_unix_path(struct vfs_namespace *ns,struct task *task,const char *name,
                          bool create,struct vfs_path *out)
{ (void)ns;(void)task;(void)name;(void)create;(void)out;abort(); }
