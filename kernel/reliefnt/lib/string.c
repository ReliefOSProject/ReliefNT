/*
 * ReliefOS freestanding string primitives: implements basic memory operations.
 * Supplies memset, memcpy, memmove, and related routines to kernel code.
 */
#include <reliefnt/types.h>

/**
 * @brief Fill len bytes at dst with the low byte of value and return dst.
 */
void *memset(void *dst, int value, size_t len)
{
    void *result = dst;
    /* Exact byte counts keep unaligned and page-boundary callers safe. */
    __asm__ volatile("cld; rep stosb"
                     : "+D"(dst), "+c"(len)
                     : "a"((unsigned char)value)
                     : "memory", "cc");
    return result;
}

/**
 * @brief Copy len bytes from src to dst and return dst; regions must not overlap.
 */
void *memcpy(void *dst, const void *src, size_t len)
{
    void *result = dst;
    /* No SIMD state is touched, including on interrupt and syscall paths. */
    __asm__ volatile("cld; rep movsb"
                     : "+D"(dst), "+S"(src), "+c"(len)
                     :
                     : "memory", "cc");
    return result;
}

/**
 * @brief Compare len bytes; return the first differing byte difference, or 0 when equal.
 */
int memcmp(const void *a, const void *b, size_t len)
{
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < len; ++i) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

/**
 * @brief Count bytes up to the NUL terminator; a NULL pointer is treated as length 0.
 */
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) {
        ++n;
    }
    return n;
}

/**
 * @brief Compare strings byte-wise; return the first differing unsigned byte difference.
 */
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/**
 * @brief Compare up to len bytes, stopping early at a difference or a NUL in either string.
 */
int strncmp(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        if (ca != cb || ca == 0 || cb == 0) {
            return (int)ca - (int)cb;
        }
    }
    return 0;
}
