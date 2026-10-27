/*
 * ReliefOS freestanding string primitives: implements basic memory operations.
 * Supplies memset, memcpy, memmove, and related routines to kernel code.
 */
#include <reliefnt/types.h>
#include <reliefnt/export.h>

/**
 * @brief Fill a caller-owned byte range with the low byte of a value.
 * @param dst Writable caller-owned range of at least len bytes.
 * @param value Value whose low byte is stored.
 * @param len Exact byte count; no alignment or page-size rounding is performed.
 * @return Original destination pointer; no ownership transfer, locks or SIMD state.
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
EXPORT_SYMBOL(memset);

/**
 * @brief Copy exact CPU bytes without touching SIMD state or other memory.
 * @param dst Writable caller-owned extent of len bytes. @param src Readable
 * nonoverlapping extent of len bytes. @param len Exact byte count, zero allowed.
 * @return Original dst; no locks, allocation, DMA or ownership transfer. Caller
 * retains source/destination mappings throughout this synchronous operation.
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
 * @brief Move bytes between possibly overlapping caller-owned ranges.
 * @param dst Writable destination of at least len bytes; returned unchanged.
 * @param src Readable source of at least len bytes, which may overlap dst.
 * @param len Exact byte count; zero performs no memory access.
 * @return Original dst. No allocation, locks, SIMD state or direction-flag
 * window is introduced, so interrupt/syscall callers retain a clear DF.
 */
void *memmove(void *dst, const void *src, size_t len)
{
    void *result = dst;
    if (!len || dst == src) return result;
    if ((uintptr_t)dst > (uintptr_t)src && (uintptr_t)dst - (uintptr_t)src < len) {
        /* Volatile byte stores prevent a compiler from folding the backward
         * loop into a recursive memmove or using task-owned SIMD registers.
         * Keeping DF clear also avoids a transient STD window across IRQs. */
        volatile unsigned char *destination = dst;
        const unsigned char *source = src;
        while (len) { --len; destination[len] = source[len]; }
    } else {
        __asm__ volatile("cld; rep movsb"
                         : "+D"(dst), "+S"(src), "+c"(len)
                         : : "memory", "cc");
    }
    return result;
}

/** @brief Compare an exact byte extent using unsigned-byte ordering.
 * @param a Borrowed readable first extent. @param b Borrowed readable second extent.
 * @param len Exact byte count; zero performs no memory access.
 * @return Negative/zero/positive byte difference. Pure IRQ/task-safe primitive;
 * no allocation, lock, scheduling or ownership transfer, no access past len.
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
 * @brief Count kernel string bytes up to the first NUL without retaining the input.
 * @param s Borrowed terminated readable kernel string; NULL is treated as empty internally.
 * @return Length excluding the terminator, or zero for NULL.
 * Pure kernel helper in any valid memory-access context; no locks, allocation or user-pointer validation. This NULL extension does not redefine the userspace C/POSIX API.
 */
size_t strlen(const char *s)
{
    size_t n = 0;
    while (s && s[n]) {
        ++n;
    }
    return n;
}

/** @brief Compare two readable NUL-terminated strings using unsigned bytes.
 * @param a Borrowed first string. @param b Borrowed second string.
 * @return First differing unsigned-byte difference, or zero. Pure IRQ/task-safe
 * primitive; no allocation, lock, scheduling or ownership transfer.
 */
int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/** @brief Compare at most a bounded number of bytes, stopping at a NUL.
 * @param a Borrowed first readable extent/string. @param b Borrowed second extent/string.
 * @param len Maximum bytes compared; zero accesses neither pointer.
 * @return First unsigned-byte difference or zero. Pure IRQ/task-safe primitive;
 * no allocation, locks, scheduling or ownership transfer; never reads past len.
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

EXPORT_SYMBOL(memcpy);

EXPORT_SYMBOL(memcmp);
EXPORT_SYMBOL(strcmp);
EXPORT_SYMBOL(strncmp);

EXPORT_SYMBOL(strlen);
