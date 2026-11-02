/* SPDX-License-Identifier: Apache-2.0 */
#define RELIEFNT_PROC_INOTIFY_IMPLEMENTATION
#include "internal.h"
#include <reliefnt/fsnotify.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>

static const char directory[] = "/proc/sys/fs/inotify";
static const char *const keys[] = {
    "/proc/sys/fs/inotify/max_user_instances",
    "/proc/sys/fs/inotify/max_user_watches",
    "/proc/sys/fs/inotify/max_queued_events",
};
static const enum fsnotify_limit limits[] = {
    FSNOTIFY_LIMIT_INSTANCES, FSNOTIFY_LIMIT_WATCHES, FSNOTIFY_LIMIT_QUEUED_EVENTS,
};

/** @brief Map a borrowed intrinsic key to one limit without resolving paths.
 * @param name Nullable key. @return Array index or -1 for an unrelated key.
 */
static int index_for(const char *name)
{
    if (name)
        for (unsigned i = 0; i < sizeof(keys) / sizeof(*keys); ++i)
            if (!__builtin_strcmp(name, keys[i])) return (int)i;
    return -1;
}

/** @brief Classify only the directory and its exact supported leaf keys.
 * @param name Nullable key. @return File=1, directory=2, absent=0.
 */
int proc_inotify_kind(const char *name)
{
    if (name && !__builtin_strcmp(name, directory)) return 2;
    return index_for(name) >= 0;
}

/** @brief Return a stable borrowed sysctl key for an open generated file.
 * @param name Nullable intrinsic key. @return Static string or NULL.
 */
const char *proc_inotify_key(const char *name)
{
    int i = index_for(name);
    return i < 0 ? NULL : keys[i];
}

/** @brief Read the live counter policy through the shared byte-stream helper.
 * @param name Intrinsic key. @param stream Writable output stream.
 * @return Zero or -ENOENT. Execution ownership serializes limit changes.
 */
int proc_inotify_emit(const char *name, struct text_stream *stream)
{
    int i = index_for(name);
    if (i < 0) return -ENOENT;
    text_unsigned(stream, fsnotify_limit_get(limits[i]));
    text_string(stream, "\n");
    return 0;
}

/** @brief Enumerate controls as ordinary files using a stable numeric cookie.
 * @param offset Required in/out cookie. @param entry Required output.
 * @return Entry=1 or EOF=0, without allocation or retained ownership.
 */
int proc_inotify_readdir(uint64_t *offset, struct reliefos_dir_entry *entry)
{
    if (*offset >= sizeof(keys) / sizeof(*keys)) return 0;
    *entry = (struct reliefos_dir_entry){.type = RELIEFOS_FS_TYPE_FILE};
    const char *leaf = keys[(*offset)++] + sizeof(directory);
    __builtin_memcpy(entry->name, leaf, __builtin_strlen(leaf) + 1);
    return 1;
}

/** @brief Accept the whitespace delimiters used by Linux integer sysctls.
 * @param byte Input byte. @return True for ASCII space, tab or line breaks.
 */
static bool space(unsigned char byte)
{ return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r' || byte == '\v' || byte == '\f'; }

/** @brief Parse a nonnegative base-zero value bounded by signed sysctl range.
 * @param buffer Validated input. @param length Byte count.
 * @param value Required output, written only on success.
 * @return Zero or -EINVAL for empty, malformed, negative or overflowing input.
 */
static int parse(const void *buffer, uint32_t length, uint32_t *value)
{
    if (!buffer || !length || length > 64) return -EINVAL;
    const unsigned char *p = buffer;
    uint32_t begin = 0, end = length;
    while (begin < end && space(p[begin])) ++begin;
    while (end > begin && space(p[end - 1])) --end;
    if (begin < end && p[begin] == '+') ++begin;
    if (begin == end) return -EINVAL;
    unsigned base = 10;
    if (p[begin] == '0') {
        base = 8;
        if (begin + 1 < end && (p[begin + 1] == 'x' || p[begin + 1] == 'X')) {
            base = 16; begin += 2;
        }
    }
    if (begin == end) return -EINVAL;
    uint32_t result = 0;
    for (; begin < end; ++begin) {
        unsigned c = p[begin], digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return -EINVAL;
        if (digit >= base || result > (INT32_MAX - digit) / base) return -EINVAL;
        result = result * base + digit;
    }
    *value = result; return 0;
}

/** @brief Commit one authorized whole sysctl write without reallocating queues.
 * @param name Intrinsic key. @param offset Position; later writes are ignored
 * like Linux's strict sysctl writes. @param buffer Validated input.
 * @param length Byte count. @param written Required output.
 * @return Zero or negative errno. DAC and mount read-only checks precede this
 * callback; changing limits does not revoke existing instances or watches.
 */
int proc_inotify_write(const char *name, uint64_t offset, const void *buffer,
                       uint32_t length, uint32_t *written)
{
    if (!written) return -EINVAL;
    *written = 0;
    int i = index_for(name);
    if (i < 0) return -EROFS;
    if (offset || !length) { *written = length; return 0; }
    uint32_t value;
    int rc = parse(buffer, length, &value);
    if (!rc) rc = fsnotify_limit_set(limits[i], value);
    if (!rc) *written = length;
    return rc;
}
