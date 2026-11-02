/* SPDX-License-Identifier: Apache-2.0 */
#include "../../fs/proc/internal.h"
#include <reliefnt/fsnotify.h>
#include <reliefnt/text_stream.h>
#include <linux/errno.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void *kernel_malloc(size_t length) { return calloc(1, length); }
void kernel_free(void *pointer) { free(pointer); }

static void read_value(const char *key, const char *expected)
{
    char buffer[32] = {0};
    struct text_stream stream = {.buffer=buffer, .capacity=sizeof(buffer)-1};
    assert(!proc_inotify_emit(key, &stream));
    assert(!strcmp(buffer, expected));
    stream = (struct text_stream){.offset=1,.buffer=buffer,.capacity=1};
    assert(!proc_inotify_emit(key, &stream));
    assert(stream.written == 1 && buffer[0] == expected[1]);
}

static void write_value(const char *key, const char *value, int expected)
{
    uint32_t written = 99;
    assert(proc_inotify_write(key, 0, value, strlen(value), &written) == expected);
    assert(written == (expected ? 0 : strlen(value)));
}

int main(void)
{
    const char *instances = "/proc/sys/fs/inotify/max_user_instances";
    const char *watches = "/proc/sys/fs/inotify/max_user_watches";
    const char *queued = "/proc/sys/fs/inotify/max_queued_events";
    assert(!proc_inotify_kind(NULL));
    assert(proc_inotify_kind("/proc/sys/fs/inotify") == 2);
    assert(proc_inotify_kind(instances) == 1);
    assert(!proc_inotify_kind("/proc/sys/fs/inotify/max_user_instances/x"));
    assert(!proc_inotify_key("/proc/sys/fs/inotify"));
    assert(!strcmp(proc_inotify_key(instances), instances));
    read_value(instances, "128\n");
    read_value(watches, "8192\n");
    read_value(queued, "16384\n");
    uint64_t cookie = 0;
    struct reliefos_dir_entry entry;
    const char *names[] = {"max_user_instances", "max_user_watches", "max_queued_events"};
    for (unsigned i = 0; i < 3; ++i) {
        assert(proc_inotify_readdir(&cookie, &entry) == 1);
        assert(entry.type == RELIEFOS_FS_TYPE_FILE && !strcmp(entry.name, names[i]));
    }
    assert(!proc_inotify_readdir(&cookie, &entry) && cookie == 3);
    struct fsnotify_instance *first = NULL, *second = NULL;
    write_value(instances, " 1\n", 0);
    assert(!fsnotify_create(1000, NULL, NULL, &first));
    assert(fsnotify_create(1000, NULL, NULL, &second) == -EMFILE);
    write_value(instances, "0", 0);
    assert(fsnotify_create(2000, NULL, NULL, &second) == -EMFILE);
    fsnotify_put(first);
    write_value(instances, "+0x80\n", 0);
    read_value(instances, "128\n");
    write_value(watches, "020000\n", 0);
    assert(fsnotify_limit_get(FSNOTIFY_LIMIT_WATCHES) == 8192);
    write_value(queued, "2147483647", 0);
    assert(fsnotify_limit_get(FSNOTIFY_LIMIT_QUEUED_EVENTS) == INT32_MAX);
    const char *bad[] = {"-1", "2147483648", "0xffffffff", "0x", "08", "1junk", "1 2", "\n"};
    for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); ++i)
        write_value(queued, bad[i], -EINVAL);
    assert(fsnotify_limit_get(FSNOTIFY_LIMIT_QUEUED_EVENTS) == INT32_MAX);
    uint32_t written = 99;
    assert(!proc_inotify_write(queued, 1, "2", 1, &written) && written == 1);
    assert(fsnotify_limit_get(FSNOTIFY_LIMIT_QUEUED_EVENTS) == INT32_MAX);
    assert(!proc_inotify_write(queued, 0, NULL, 0, &written) && !written);
    assert(proc_inotify_write("/proc/sys/fs/inotify/unknown", 0, "1", 1, &written) == -EROFS);
    puts("inotify proc: live limits, UID accounting, enumeration and numeric validation passed");
    return 0;
}
