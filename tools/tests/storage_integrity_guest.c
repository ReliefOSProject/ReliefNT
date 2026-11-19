/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define INITIAL_SIZE (2u * 1024u * 1024u + 1537u)
#define CAPACITY (INITIAL_SIZE + 131072u)
#define IO_SIZE 131072u

/** @brief Report a failed probe operation without hiding its errno.
 * @param operation Static operation name. @return Nonzero failure status.
 */
static int fail(const char *operation)
{ fprintf(stderr, "STORAGE_INTEGRITY_FAIL %s errno=%d\n", operation, errno); return 1; }

/** @brief Transfer exactly the requested range using retryable POSIX calls.
 * @param fd Owned descriptor. @param bytes Writable buffer for reads.
 * @param length Available bytes. @param offset Absolute file position.
 * @param writing Select pwrite. @return Zero or diagnosed short/error status.
 */
static int transfer(int fd, unsigned char *bytes, size_t length, size_t offset, int writing)
{
    size_t done = 0;
    while (done < length) {
        ssize_t count = writing ? pwrite(fd, bytes + done, length - done, offset + done) :
                                  pread(fd, bytes + done, length - done, offset + done);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0 || (size_t)count > length - done) return fail("transfer");
        done += (size_t)count;
    }
    return 0;
}

/** @brief Verify all expected bytes against the independent in-memory model.
 * @param fd Borrowed descriptor. @param model Expected bytes.
 * @param length Model length. @param buffer IO_SIZE scratch bytes.
 * @param regular Whether EOF and stat size must also match.
 * @return Zero or a diagnosed data/size failure.
 */
static int verify(int fd, const unsigned char *model, size_t length,
                  unsigned char *buffer, int regular)
{
    for (size_t offset = 0; offset < length;) {
        size_t bytes = length - offset < IO_SIZE ? length - offset : IO_SIZE;
        memset(buffer, 0xa5, IO_SIZE);
        if (transfer(fd, buffer, bytes, offset, 0)) return 1;
        if (memcmp(buffer, model + offset, bytes)) {
            for (size_t i = 0; i < bytes; ++i) if (buffer[i] != model[offset + i]) {
                fprintf(stderr, "STORAGE_INTEGRITY_FAIL offset=%zu got=%u expected=%u\n",
                        offset + i, buffer[i], model[offset + i]); break;
            }
            return 1;
        }
        offset += bytes;
    }
    if (regular) {
        struct stat status;
        if (fstat(fd, &status) || status.st_size != (off_t)length) return fail("size");
        if (pread(fd, buffer, IO_SIZE, length) != 0) return fail("eof");
    }
    return 0;
}

/** @brief Exercise aligned/unaligned, sparse, overwrite and persistence paths.
 * @param argc Three arguments. @param argv file|block and disposable target.
 * @return Zero after every byte verifies, close/reopen and final fsync succeed.
 * Block targets must be disposable and at least CAPACITY bytes long. Regular
 * targets are exclusively created; no existing file is truncated or replaced.
 */
int main(int argc, char **argv)
{
    if (argc != 3 || (strcmp(argv[1], "file") && strcmp(argv[1], "block"))) return 2;
    int regular = !strcmp(argv[1], "file");
    unsigned char *model = calloc(1, CAPACITY), *buffer = malloc(IO_SIZE);
    if (!model || !buffer) return fail("allocate");
    int fd = open(argv[2], O_RDWR | (regular ? O_CREAT | O_EXCL : 0), 0600);
    if (fd < 0) return fail("open");
    size_t length = regular ? INITIAL_SIZE : CAPACITY;
    if (regular) {
        if (ftruncate(fd, length)) return fail("initial sparse size");
    } else if (verify(fd, model, length, buffer, 0)) return 1;
    static const size_t sizes[] = {1, 511, 512, 513, 4095, 4096, 4097,
                                   32768, 32769, 65535, 65536, 65537, 131071, 131072};
    uint32_t random = 0x75196ac3;
    for (unsigned round = 0; round < 70; ++round) {
        random = random * 1664525u + 1013904223u;
        size_t offset = round < 14 ? round * 65536u + (round % 3 == 0 ? 0 : 511) :
                                    random % INITIAL_SIZE;
        size_t bytes = sizes[round % (sizeof(sizes) / sizeof(sizes[0]))];
        if (bytes > CAPACITY - offset) bytes = CAPACITY - offset;
        for (size_t i = 0; i < bytes; ++i)
            buffer[i] = (unsigned char)((offset + i) * 29u ^ ((offset + i) >> 11) ^ round);
        if (transfer(fd, buffer, bytes, offset, 1)) return 1;
        memcpy(model + offset, buffer, bytes);
        if (offset + bytes > length) length = offset + bytes;
        if (round % 11 == 0 && (fsync(fd) || verify(fd, model, length, buffer, regular)))
            return fail("intermediate sync/verify");
    }
    if (regular) {
        size_t shrink = 65537;
        if (ftruncate(fd, shrink)) return fail("shrink");
        if (verify(fd, model, shrink, buffer, 1)) return 1;
        if (ftruncate(fd, INITIAL_SIZE)) return fail("regrow");
        memset(model + shrink, 0, CAPACITY - shrink);
        length = INITIAL_SIZE;
        if (verify(fd, model, length, buffer, 1)) return 1;
        /* Leave nonzero data beyond the regrown hole for independent extraction. */
        memset(buffer, 0x6d, 65537);
        size_t offset = INITIAL_SIZE - 65537;
        if (transfer(fd, buffer, 65537, offset, 1)) return 1;
        memcpy(model + offset, buffer, 65537);
    }
    if (fsync(fd) || close(fd)) return fail("final sync/close");
    fd = open(argv[2], O_RDONLY);
    if (fd < 0 || verify(fd, model, length, buffer, regular) || close(fd)) return fail("reopen");
    printf("STORAGE_INTEGRITY_PASS mode=%s path=%s bytes=%zu writes=70\n",
           argv[1], argv[2], length);
    free(buffer); free(model);
    return 0;
}
