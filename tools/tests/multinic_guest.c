/* SPDX-License-Identifier: Apache-2.0 */
/* Native Linux socket probe for independently configured network interfaces. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static const char *device;

static void require(int condition, const char *operation)
{
    if (!condition) {
        fprintf(stderr, "MULTINIC_FAIL device=%s operation=%s errno=%d (%s)\n",
                device, operation, errno, strerror(errno));
        exit(1);
    }
}

static int connect_peer(int family, int type, const char *peer, unsigned port)
{
    int fd = socket(family, type, 0);
    require(fd >= 0, "socket");
    require(setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, device, strlen(device) + 1) == 0,
            "bind socket to interface");
    char bound[IFNAMSIZ] = {0};
    socklen_t size = sizeof(bound);
    require(getsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, bound, &size) == 0 &&
            size == strlen(device) + 1 && strcmp(bound, device) == 0,
            "read bound interface");
    struct timeval timeout = {8, 0};
    require(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0,
            "receive timeout");
    if (type == SOCK_STREAM)
        require(setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0,
                "send timeout");
    struct sockaddr_storage storage = {0};
    if (family == AF_INET) {
        struct sockaddr_in *address = (void *)&storage;
        address->sin_family = AF_INET;
        address->sin_port = htons((uint16_t)port);
        require(inet_pton(family, peer, &address->sin_addr) == 1, "parse IPv4 peer");
        size = sizeof(*address);
    } else {
        struct sockaddr_in6 *address = (void *)&storage;
        address->sin6_family = AF_INET6;
        address->sin6_port = htons((uint16_t)port);
        require(inet_pton(family, peer, &address->sin6_addr) == 1, "parse IPv6 peer");
        size = sizeof(*address);
    }
    require(connect(fd, (void *)&storage, size) == 0, "connect echo peer");
    if (family == AF_INET) {
        struct ifreq request = {0};
        snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", device);
        require(ioctl(fd, SIOCGIFADDR, &request) == 0, "query interface IPv4");
        struct sockaddr_in local;
        socklen_t local_size = sizeof(local);
        require(getsockname(fd, (void *)&local, &local_size) == 0 &&
                local.sin_addr.s_addr == ((struct sockaddr_in *)&request.ifr_addr)->sin_addr.s_addr,
                "TCP source belongs to selected device");
    }
    return fd;
}

static void transfer(int fd, unsigned char *data, size_t size, int sending)
{
    while (size) {
        ssize_t count = sending ? send(fd, data, size, 0) : recv(fd, data, size, 0);
        if (count < 0 && errno == EINTR) continue;
        require(count > 0, sending ? "TCP send" : "TCP receive");
        data += count;
        size -= (size_t)count;
    }
}

static void check_stream(int family, const char *peer, unsigned port, int hold)
{
    int fd = connect_peer(family, SOCK_STREAM, peer, port);
    unsigned char sent[4096], received[4096];
    unsigned rounds = hold ? 50 : 3;
    for (unsigned round = 0; round < rounds; ++round) {
        for (unsigned i = 0; i < sizeof(sent); ++i)
            sent[i] = (unsigned char)(i * 37 + round * 19 + (unsigned)family);
        transfer(fd, sent, sizeof(sent), 1);
        transfer(fd, received, sizeof(received), 0);
        require(memcmp(sent, received, sizeof(sent)) == 0, "TCP payload integrity");
        if (hold && round == 0) {
            FILE *marker = fopen("/run/multinic-hold-ready", "w");
            require(marker != NULL && fclose(marker) == 0, "publish active stream marker");
        }
        if (hold) {
            struct timespec delay = {0, 100000000};
            while (nanosleep(&delay, &delay) < 0 && errno == EINTR) {}
        }
    }
    require(close(fd) == 0, "close TCP socket");
}

static void check_datagrams(int family, const char *peer, unsigned port)
{
    int fd = connect_peer(family, SOCK_DGRAM, peer, port);
    unsigned char sent[333], received[334];
    for (unsigned round = 0; round < 4; ++round) {
        for (unsigned i = 0; i < sizeof(sent); ++i)
            sent[i] = (unsigned char)(i * 17 + round * 23 + (unsigned)family);
        require(send(fd, sent, sizeof(sent), 0) == sizeof(sent), "UDP send");
        require(recv(fd, received, sizeof(received), 0) == sizeof(sent), "UDP receive size");
        require(memcmp(sent, received, sizeof(sent)) == 0, "UDP payload integrity");
    }
    require(close(fd) == 0, "close UDP socket");
}

int main(int argc, char **argv)
{
    device = argc > 1 ? argv[1] : "?";
    require(argc == 5 || argc == 6, "arguments device IPv4 IPv6 port [hold]");
    char *end;
    unsigned long number = strtoul(argv[4], &end, 10);
    require(argv[4][0] && !*end && number > 0 && number <= 65535, "echo port");
    require(strlen(device) < IFNAMSIZ && if_nametoindex(device) > 0, "interface identity");
    for (unsigned version = 0; version < 2; ++version) {
        int family = version ? AF_INET6 : AF_INET;
        check_stream(family, argv[version + 2], (unsigned)number, argc == 6 && !version);
        check_datagrams(family, argv[version + 2], (unsigned)number);
        printf("MULTINIC_PASS device=%s family=%u\n", device, version ? 6 : 4);
        fflush(stdout);
    }
    return 0;
}
