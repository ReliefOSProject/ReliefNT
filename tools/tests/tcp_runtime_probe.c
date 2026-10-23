/* Native Linux socket ABI probe; the host harness supplies a local TCP peer. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL tcp-runtime line=%d errno=%d: %s\n", __LINE__, errno, #expr); return 1; \
} } while (0)

static uint64_t milliseconds(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return 0;
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int configure_guest(int fd)
{
    struct ifreq request = {0};
    strcpy(request.ifr_name, "eth0");
    struct sockaddr_in *address = (struct sockaddr_in *)&request.ifr_addr;
    address->sin_family = AF_INET;
    CHECK(inet_pton(AF_INET, "10.0.2.15", &address->sin_addr) == 1);
    CHECK(!ioctl(fd, SIOCSIFADDR, &request));
    CHECK(inet_pton(AF_INET, "255.255.255.0", &address->sin_addr) == 1);
    CHECK(!ioctl(fd, SIOCSIFNETMASK, &request));
    request.ifr_flags = IFF_UP;
    CHECK(!ioctl(fd, SIOCSIFFLAGS, &request));
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    alarm(40);
    const char *host = argc > 1 ? argv[1] : "10.0.2.2";
    unsigned long port = argc > 2 ? strtoul(argv[2], NULL, 10) : 19876;
    CHECK(port && port <= 65535);
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    if (argc > 3 && !strcmp(argv[3], "--guest")) CHECK(!configure_guest(fd));
    struct timeval timeout;
    socklen_t size = sizeof(timeout);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, &size));
    CHECK(size == sizeof(timeout) && !timeout.tv_sec && !timeout.tv_usec);
    size = sizeof(timeout);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size));
    CHECK(!timeout.tv_sec && !timeout.tv_usec);
    CHECK(!fcntl(fd, F_SETFL, O_NONBLOCK));
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    CHECK(inet_pton(AF_INET, host, &peer.sin_addr) == 1);
    int connected = connect(fd, (struct sockaddr *)&peer, sizeof(peer));
    CHECK(!connected || (connected == -1 && errno == EINPROGRESS));
    struct pollfd poller = {.fd = fd, .events = POLLOUT};
    CHECK(poll(&poller, 1, 5000) == 1);
    int error = -1;
    size = sizeof(error);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) && !error);

    CHECK(!fcntl(fd, F_SETFL, 0));
    timeout = (struct timeval){.tv_sec = -1};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    unsigned char response[8];
    uint64_t began = milliseconds();
    errno = 0;
    CHECK(recv(fd, response, sizeof(response), 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    CHECK(milliseconds() - began < 100);
    size = sizeof(timeout);
    CHECK(!getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, &size));
    CHECK(!timeout.tv_sec && !timeout.tv_usec);
    timeout = (struct timeval){.tv_usec = 150000};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    began = milliseconds();
    errno = 0;
    CHECK(recv(fd, response, sizeof(response), 0) == -1 && (errno == EAGAIN || errno == EWOULDBLOCK));
    uint64_t elapsed = milliseconds() - began;
    CHECK(elapsed >= 100 && elapsed < 2000);
    printf("PASS tcp-runtime: default infinite timeouts, receive deadline %llu ms\n", (unsigned long long)elapsed);
    timeout = (struct timeval){0};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!fcntl(fd, F_SETFL, O_NONBLOCK));

    unsigned char data[65536];
    const size_t total = 2 * 1024 * 1024;
    size_t sent = 0;
    uint64_t expected = UINT64_C(14695981039346656037);
    began = milliseconds();
    unsigned writes = 0, retries = 0;
    while (sent < total) {
        size_t count = total - sent;
        if (count > sizeof(data)) count = sizeof(data);
        for (size_t i = 0; i < count; ++i) data[i] = (unsigned char)((sent + i) * 29 + 7);
        uint64_t call_began = milliseconds();
        ssize_t result = send(fd, data, count, MSG_NOSIGNAL);
        CHECK(milliseconds() - call_began < 1000);
        CHECK(milliseconds() - began < 30000);
        if (result < 0) {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            ++retries;
            poller.events = POLLOUT;
            CHECK(poll(&poller, 1, 2000) >= 0);
            continue;
        }
        CHECK(result > 0 && (size_t)result <= count);
        for (ssize_t i = 0; i < result; ++i) expected = (expected ^ data[i]) * UINT64_C(1099511628211);
        sent += (size_t)result;
        ++writes;
    }
    size_t received = 0;
    while (received < sizeof(response)) {
        ssize_t count = recv(fd, response + received, sizeof(response) - received, 0);
        CHECK(milliseconds() - began < 30000);
        if (count < 0) {
            CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
            poller.events = POLLIN;
            CHECK(poll(&poller, 1, 2000) >= 0);
            continue;
        }
        CHECK(count > 0);
        received += (size_t)count;
    }
    uint64_t actual = 0;
    for (size_t i = 0; i < sizeof(response); ++i) actual = (actual << 8) | response[i];
    CHECK(actual == expected);
    printf("PASS tcp-runtime: %zu bytes verified, %u writes, %u backpressure retries, %llu ms\n",
           sent, writes, retries, (unsigned long long)(milliseconds() - began));
    CHECK(!close(fd));
    /* Peer FIN closes only its sending direction. Read EOF before replying,
     * then let the external peer check every byte and our orderly FIN. */
    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(fd >= 0);
    timeout = (struct timeval){.tv_sec = 3};
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    CHECK(!setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)));
    CHECK(!connect(fd, (struct sockaddr *)&peer, sizeof(peer)));
    received = 0;
    while (received < sizeof(response)) {
        ssize_t count = recv(fd, response + received, sizeof(response) - received, 0);
        CHECK(count > 0);
        received += (size_t)count;
    }
    CHECK(!memcmp(response, "peer-fin", sizeof(response)));
    CHECK(recv(fd, response, 1, 0) == 0);
    poller = (struct pollfd){.fd = fd, .events = POLLIN | POLLOUT | POLLRDHUP};
    CHECK(poll(&poller, 1, 0) == 1);
    CHECK((poller.revents & (POLLIN | POLLOUT | POLLRDHUP)) ==
          (POLLIN | POLLOUT | POLLRDHUP) && !(poller.revents & POLLHUP));
    for (size_t i = 0; i < 8192; ++i) data[i] = (unsigned char)(i * 29 + 7);
    sent = 0;
    while (sent < 8192) {
        ssize_t count = send(fd, data + sent, 8192 - sent, MSG_NOSIGNAL);
        CHECK(count > 0);
        sent += (size_t)count;
    }
    CHECK(!shutdown(fd, SHUT_WR));
    CHECK(!close(fd));
    puts("PASS tcp-runtime: peer write-half-close permits a complete local reply");
    /* A normal final close/exit must leave accepted data owned by the network
     * stack. The parent deliberately makes no further networking calls. The
     * external peer is the delivery oracle, independently of this PASS line. */
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        int output = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        CHECK(output >= 0);
        CHECK(!connect(output, (struct sockaddr *)&peer, sizeof(peer)));
        for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)(i * 29 + 7);
        size_t done = 0;
        while (done < 65535) {
            ssize_t count = write(output, data + done, 65535 - done);
            CHECK(count > 0);
            done += (size_t)count;
        }
        CHECK(!close(output));
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    puts("TCP_CLOSED_OWNER_EXITED bytes=65535; awaiting independent peer verification");
    struct timespec grace = {.tv_sec = 1, .tv_nsec = 500000000};
    while (nanosleep(&grace, &grace)) CHECK(errno == EINTR);
    puts("PASS TCP Linux ABI runtime probe");
    return 0;
}
