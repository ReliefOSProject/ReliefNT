/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief Read the native integer TCP flag from a live descriptor.
 * @param fd TCP descriptor. @param expected Normalized flag value.
 */
static void check_flag(int fd, int expected)
{
    int value = -1; socklen_t size = sizeof(value);
    assert(!getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &size));
    assert(size == sizeof(value) && value == expected);
}

/** @brief Exercise native flag ABI and listener inheritance on one IP family.
 * @param family AF_INET or AF_INET6; only loopback traffic is generated.
 */
static void probe(int family)
{
    int listener = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int client = socket(family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    assert(listener >= 0 && client >= 0);
    check_flag(listener, 0); check_flag(client, 0);
    int value = -9;
    assert(!setsockopt(listener, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
    check_flag(listener, 1); check_flag(client, 0);
    socklen_t size = sizeof(value);
    errno = 0;
    assert(setsockopt(client, IPPROTO_TCP, TCP_NODELAY, NULL, sizeof(value)) == -1 && errno == EFAULT);
    errno = 0;
    assert(setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value) - 1) == -1 && errno == EINVAL);
    errno = 0;
    assert(getsockopt(client, IPPROTO_TCP, TCP_NODELAY, &value, NULL) == -1 && errno == EFAULT);
    errno = 0;
    assert(getsockopt(client, IPPROTO_TCP, TCP_NODELAY, NULL, &size) == -1 && errno == EFAULT);
    size = 0;
    assert(!getsockopt(client, IPPROTO_TCP, TCP_NODELAY, NULL, &size) && !size);
    unsigned char truncated[2] = {0xff, 0xa5}; size = 1;
    assert(!getsockopt(client, IPPROTO_TCP, TCP_NODELAY, truncated, &size));
    assert(size == 1 && truncated[0] == 0 && truncated[1] == 0xa5);
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED && !mprotect(page, 4096, PROT_READ));
    size = sizeof(value); errno = 0;
    assert(getsockopt(client, IPPROTO_TCP, TCP_NODELAY, page, &size) == -1 && errno == EFAULT);
    assert(!munmap(page, 4096));
    value = 1;
    errno = 0;
    assert(setsockopt(client, IPPROTO_TCP, 0x7fff, &value, sizeof(value)) == -1 && errno == ENOPROTOOPT);
    size = sizeof(value); errno = 0;
    assert(getsockopt(client, IPPROTO_TCP, 0x7fff, &value, &size) == -1 && errno == ENOPROTOOPT);
    check_flag(client, 0);
    assert(!setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
    check_flag(client, 1);
    struct sockaddr_storage address = {0}; socklen_t address_size;
    if (family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)&address;
        a->sin_family = family; a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address_size = sizeof(*a);
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)&address;
        a->sin6_family = family; a->sin6_addr = in6addr_loopback;
        address_size = sizeof(*a);
    }
    assert(!bind(listener, (struct sockaddr *)&address, address_size));
    assert(!getsockname(listener, (struct sockaddr *)&address, &address_size));
    assert(!listen(listener, 1));
    assert(!connect(client, (struct sockaddr *)&address, address_size));
    int accepted = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    assert(accepted >= 0);
    check_flag(accepted, 1);
    value = 0;
    assert(!setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &value, sizeof(value)));
    check_flag(accepted, 0); check_flag(listener, 1); check_flag(client, 1);
    assert(write(client, "a", 1) == 1 && write(client, "b", 1) == 1);
    char output[2];
    for (size_t received = 0; received < sizeof(output); ) {
        ssize_t count = recv(accepted, output + received, sizeof(output) - received, 0);
        assert(count > 0 && (size_t)count <= sizeof(output) - received);
        received += (size_t)count;
    }
    assert(!memcmp(output, "ab", 2));
    assert(!close(accepted) && !close(client) && !close(listener));
}

/** @brief Compare the TCP_NODELAY user ABI on Linux and ReliefNT.
 * @return Zero after IPv4/IPv6 option and loopback checks pass.
 */
int main(void)
{
    probe(AF_INET);
    probe(AF_INET6);
    puts("PASS TCP_NODELAY: IPv4/IPv6, normalized state, pointer/length errors and listener inheritance");
    return 0;
}
