/* SPDX-License-Identifier: Apache-2.0 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief Verify both endpoints report a snapshot without consuming bytes.
 * @param pair Live pipe endpoints. @param expected Expected unread count.
 */
static void available(const int pair[2], int expected)
{
    for (unsigned i = 0; i < 2; ++i) {
        int count = -1;
        assert(ioctl(pair[i], FIONREAD, &count) == 0 && count == expected);
        assert(ioctl(pair[i], FIONREAD, &count) == 0 && count == expected);
    }
}

/** @brief Exercise the Linux pipe/FIFO ioctl ABI on the host or target kernel.
 * @return Zero only after byte counts, errors and EOF behavior match Linux.
 */
int main(void)
{
    int pair[2];
    char bytes[4096], output[4096];
    memset(bytes, 0x6b, sizeof(bytes));
    assert(pipe2(pair, O_NONBLOCK | O_CLOEXEC) == 0);
    available(pair, 0);
    assert(write(pair[1], bytes, sizeof(bytes)) == sizeof(bytes));
    available(pair, sizeof(bytes));
    assert(read(pair[0], output, 3000) == 3000);
    available(pair, 1096);
    errno = 0;
    assert(ioctl(pair[0], FIONREAD, (void *)0) == -1 && errno == EFAULT);
    void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    assert(page != MAP_FAILED && mprotect(page, 4096, PROT_READ) == 0);
    errno = 0;
    assert(ioctl(pair[1], FIONREAD, page) == -1 && errno == EFAULT);
    assert(munmap(page, 4096) == 0);
    available(pair, 1096);
    assert(write(pair[1], bytes, 3000) == 3000);
    available(pair, 4096);
    assert(close(pair[1]) == 0);
    int count = -1;
    assert(ioctl(pair[0], FIONREAD, &count) == 0 && count == 4096);
    assert(read(pair[0], output, sizeof(output)) == sizeof(output));
    assert(memcmp(bytes, output, sizeof(bytes)) == 0);
    assert(ioctl(pair[0], FIONREAD, &count) == 0 && count == 0);
    assert(read(pair[0], output, 1) == 0);
    errno = 0;
    assert(ioctl(pair[0], 0x7fff1234, &count) == -1 && errno == ENOTTY);
    assert(close(pair[0]) == 0);
    char path[] = "/tmp/pipe-fionread-XXXXXX";
    int temporary = mkstemp(path);
    assert(temporary >= 0 && close(temporary) == 0 && unlink(path) == 0);
    assert(mkfifo(path, 0600) == 0);
    int fifo = open(path, O_RDWR | O_NONBLOCK);
    assert(fifo >= 0 && unlink(path) == 0);
    assert(write(fifo, bytes, 37) == 37);
    assert(ioctl(fifo, FIONREAD, &count) == 0 && count == 37);
    assert(read(fifo, output, sizeof(output)) == 37 && !memcmp(bytes, output, 37));
    assert(ioctl(fifo, FIONREAD, &count) == 0 && count == 0 && close(fifo) == 0);
    puts("PASS pipe FIONREAD: both endpoints, partial reads, bad pointers, EOF and named FIFO");
    return 0;
}
