#include "../internal.h"

/**
 * Kernel clear secret.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
static void kernel_clear_secret(void *data, uint32_t len);


/**
 * Path append char.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param ch Value supplied by the caller.
 */
void path_append_char(char *buf, uint32_t *pos, uint32_t cap, char ch)
{
    if (buf && pos && *pos + 1 < cap) {
        buf[(*pos)++] = ch;
        buf[*pos] = 0;
    }
}

/**
 * Text eq cstr.
 * @param a Value supplied by the caller.
 * @param b Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int text_eq_cstr(const char *a, const char *b)
{
    uint32_t i = 0;
    if (!a || !b) {
        return 0;
    }
    while (a[i] && b[i] && a[i] == b[i]) {
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

/**
 * Path append text.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param text NUL-terminated text supplied by the caller.
 */
void path_append_text(char *buf, uint32_t *pos, uint32_t cap, const char *text)
{
    while (text && *text) {
        path_append_char(buf, pos, cap, *text++);
    }
}

/**
 * Path append u64.
 * @param buf Value supplied by the caller.
 * @param pos Output storage updated by the function.
 * @param cap Maximum number of elements available in the related buffer.
 * @param value Value supplied by the caller.
 */
void path_append_u64(char *buf, uint32_t *pos, uint32_t cap, uint64_t value)
{
    char tmp[24];
    uint32_t n = 0;
    if (value == 0) {
        path_append_char(buf, pos, cap, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10));
        value /= 10;
    }
    while (n) {
        path_append_char(buf, pos, cap, tmp[--n]);
    }
}

/**
 * Copy text.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param src Value supplied by the caller.
 */
void copy_text(char *dst, uint32_t cap, const char *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        dst[i] = src[i];
        ++i;
    }
    dst[i] = 0;
}

/**
 * Copy user string fixed.
 * @param dst Value supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @param user_ptr Value supplied by the caller.
 * @param out_len Value supplied by the caller.
 * @return The value or status produced by the operation.
 */
int copy_user_string_fixed(char *dst, uint32_t cap, uint64_t user_ptr, uint32_t *out_len)
{
    size_t len;
    if (!dst || !cap) {
        return -RELIEFOS_EINVAL;
    }
    if (!user_ptr || !user_range_ok(user_ptr, 1)) {
        return -RELIEFOS_EFAULT;
    }
    len = user_strlen((const char *)(uintptr_t)user_ptr, cap);
    if (len == cap || !user_range_ok(user_ptr, len + 1)) {
        return -RELIEFOS_EFAULT;
    }
    for (size_t i = 0; i <= len; ++i) {
        dst[i] = ((const char *)(uintptr_t)user_ptr)[i];
    }
    if (out_len) {
        *out_len = (uint32_t)len;
    }
    return 0;
}

/**
 * Kernel string len cap.
 * @param text NUL-terminated text supplied by the caller.
 * @param cap Maximum number of elements available in the related buffer.
 * @return The value or status produced by the operation.
 */
uint32_t kernel_string_len_cap(const char *text, uint32_t cap)
{
    uint32_t len = 0;
    while (text && len < cap && text[len]) {
        ++len;
    }
    return len;
}

/**
 * Kernel clear secret.
 * @param data Value supplied by the caller.
 * @param len Maximum number of elements available in the related buffer.
 */
static void kernel_clear_secret(void *data, uint32_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)data;
    while (p && len) {
        *p++ = 0;
        --len;
    }
}
