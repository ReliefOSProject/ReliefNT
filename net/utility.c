#include "internal.h"



/**
 * @brief Yield the processor while a synchronous networking loop waits for progress.
 * Host network tests use PAUSE because STI/HLT is privileged outside the kernel.
 */
void net_cpu_relax(void)
{
#ifdef RELIEFNT_HOST_TEST
    __asm__ volatile("pause");
#else
    __asm__ volatile("sti; hlt; cli");
#endif
}

/**
 * Net memcpy.
 * @param dst Value supplied by the caller.
 * @param src Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
void net_memcpy(void *dst, const void *src, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (len--) {
        *d++ = *s++;
    }
}

/**
 * Net memzero.
 * @param dst Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
void net_memzero(void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;
    while (len--) {
        *d++ = 0;
    }
}

/**
 * Net strlen.
 * @param text NUL-terminated text supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t net_strlen(const char *text, uint32_t cap)
{
    uint32_t len = 0;
    while (text && len < cap && text[len]) {
        ++len;
    }
    return len;
}

/**
 * Net mac is zero.
 * @param mac Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_mac_is_zero(const uint8_t mac[6])
{
    return (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]) == 0;
}

/**
 * Net memeq.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int net_memeq(const uint8_t *a, const uint8_t *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Net text eq len.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
int net_text_eq_len(const char *a, const char *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; ++i) {
        if (!a || !b || a[i] != b[i]) {
            return 0;
        }
    }
    return b[len] == 0;
}

/**
 * Net get u16.
 * @param p Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/**
 * Net get u32.
 * @param p Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint32_t net_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/**
 * Net put u16.
 * @param p Value supplied by the caller.
 * @param value Value supplied by the caller.
 */
void net_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

/**
 * Net put u32.
 * @param p Value supplied by the caller.
 * @param value Value supplied by the caller.
 */
void net_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/**
 * Net checksum.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint16_t net_checksum(const uint8_t *data, uint32_t len)
{
    uint32_t sum = 0;
    while (len > 1) {
        sum += net_get_u16(data);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)data[0] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/**
 * Net checksum partial.
 * @param sum Value supplied by the caller.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t net_checksum_partial(uint32_t sum, const uint8_t *data,
                                     uint32_t len)
{
    while (len > 1) {
        sum += net_get_u16(data);
        data += 2;
        len -= 2;
    }
    if (len) {
        sum += (uint16_t)data[0] << 8;
    }
    return sum;
}

/**
 * Net checksum finish.
 * @param sum Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
uint16_t net_checksum_finish(uint32_t sum)
{
    while (sum >> 16) {
        sum = (sum & 0xffffu) + (sum >> 16);
    }
    return (uint16_t)~sum;
}

/**
 * Net append char.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param ch Value supplied by the caller.
 */
void net_append_char(char *dst, uint32_t *pos, uint32_t cap, char ch)
{
    if (dst && pos && *pos + 1u < cap) {
        dst[*pos] = ch;
        ++(*pos);
        dst[*pos] = 0;
    }
}

/**
 * Net append text.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param text NUL-terminated text supplied by the caller.
 */
void net_append_text(char *dst, uint32_t *pos, uint32_t cap,
                            const char *text)
{
    while (text && *text) {
        net_append_char(dst, pos, cap, *text++);
    }
}

/**
 * Net append u32.
 * @param dst Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param value Value supplied by the caller.
 */
void net_append_u32(char *dst, uint32_t *pos, uint32_t cap,
                           uint32_t value)
{
    char tmp[12];
    uint32_t n = 0;
    if (value == 0) {
        net_append_char(dst, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n) {
        net_append_char(dst, pos, cap, tmp[--n]);
    }
}

/**
 * Net timeout expired.
 * @param start_ms Value supplied by the caller.
 * @param timeout_ms Value supplied by the caller.
 * @param spins Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int net_timeout_expired(uint64_t start_ms, uint32_t timeout_ms,
                               uint32_t spins)
{
    uint64_t now = time_uptime_ms();
    uint32_t spin_limit = timeout_ms * 2000u + 100000u;
    if (now != start_ms) {
        return now - start_ms >= timeout_ms;
    }
    return spins >= spin_limit;
}
