/* SPDX-License-Identifier: Apache-2.0 */
#include "../../block/core/storage_internal.h"
#include <reliefnt/storage_context.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const struct storage_filesystem_ops storage_squashfs_ops;
static unsigned char *image;
static size_t image_size;
static unsigned allocations;
static uint64_t device_generation;

#define CHECK(expression) do { \
    if (!(expression)) { \
        fprintf(stderr, "squashfs:%u: %s\n", __LINE__, #expression); \
        exit(1); \
    } \
} while (0)

void *kernel_malloc(size_t count)
{
    void *ptr = malloc(count);
    if (ptr) ++allocations;
    return ptr;
}

void kernel_free(void *ptr)
{
    if (ptr) --allocations;
    free(ptr);
}

int storage_read_device(const struct storage_volume *volume, uint64_t lba,
                        uint32_t count, void *output)
{
    (void)volume;
    if (lba > image_size / 512 || count > image_size / 512 - lba) return -EIO;
    memcpy(output, image + lba * 512, (size_t)count * 512);
    return 0;
}

uint64_t storage_volume_device_number(const struct storage_volume *volume)
{
    (void)volume;
    return 0x101;
}

uint64_t block_device_lookup_generation(const struct block_device_ref *reference)
{
    (void)reference;
    return device_generation;
}

void storage_kernel_context_enter(struct storage_io_context *context) { (void)context; }
void storage_kernel_context_leave(const struct storage_io_context *context) { (void)context; }

static unsigned char *load(const char *path, size_t *size)
{
    FILE *file = fopen(path, "rb");
    CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    CHECK(length > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    *size = (size_t)length;
    unsigned char *bytes = malloc(*size);
    CHECK(bytes && fread(bytes, 1, *size, file) == *size);
    CHECK(fclose(file) == 0);
    return bytes;
}

static void check_xattrs(const struct storage_filesystem_ops *ops, struct storage_volume *volume,
                         const struct storage_node *node, bool enabled)
{
    uint32_t count;
    CHECK(ops->getxattr(volume, node, "user.missing", NULL, 0, &count) == -ENODATA);
    CHECK(ops->listxattr(volume, node, NULL, 0, &count, false) == 0);
    if (!enabled) {
        CHECK(count == 0);
        CHECK(ops->getxattr(volume, node, "user.description", NULL, 0, &count) == -ENODATA);
        return;
    }

    const char *const expected[] = {"user.description", "user.empty", "user.shared"};
    char names[sizeof("user.description\0user.empty\0user.shared")];
    CHECK(count == sizeof(names));
    CHECK(ops->listxattr(volume, node, names, sizeof(names) - 1, &count, false) == -ERANGE);
    CHECK(ops->listxattr(volume, node, names, sizeof(names), &count, false) == 0);
    CHECK(count == sizeof(names));
    unsigned seen = 0;
    for (size_t offset = 0; offset < sizeof(names);) {
        const char *name = names + offset;
        const char *end = memchr(name, 0, sizeof(names) - offset);
        CHECK(end);
        unsigned index = 0;
        while (index < sizeof(expected) / sizeof(*expected) && strcmp(name, expected[index]))
            ++index;
        CHECK(index < sizeof(expected) / sizeof(*expected) && !(seen & (1u << index)));
        seen |= 1u << index;
        offset = (size_t)(end - names) + 1;
    }
    CHECK(seen == 7);

    unsigned char value[1024];
    CHECK(ops->getxattr(volume, node, "user.description", NULL, 0, &count) == 0 && count == 8);
    CHECK(ops->getxattr(volume, node, "user.description", value, 7, &count) == -ERANGE);
    CHECK(ops->getxattr(volume, node, "user.description", value, 8, &count) == 0);
    CHECK(count == 8 && !memcmp(value, "ReliefNT", count));
    CHECK(ops->getxattr(volume, node, "user.empty", value, sizeof(value), &count) == 0 && count == 0);
    CHECK(ops->getxattr(volume, node, "user.shared", value, sizeof(value), &count) == 0);
    CHECK(count == sizeof(value));
    for (unsigned i = 0; i < count; ++i) CHECK(value[i] == (unsigned char)i);
}

static void good(const char *path, const unsigned char *data, size_t size, bool xattrs)
{
    const struct storage_filesystem_ops *ops = &storage_squashfs_ops;
    image = load(path, &image_size);
    struct storage_volume volume = {.volume_id = 3, .ext_sector_count = image_size / 512};
    CHECK(ops->mount(&volume) == -EROFS);
    volume.mount_flags = MS_RDONLY;
    CHECK(ops->mount(&volume) == 0);
    struct storage_node root, file, other, dir;
    CHECK(ops->root(&volume, &root) == 0 && root.type == RELIEFOS_FS_TYPE_DIR);
    CHECK(ops->lookup(&volume, &root, "DATA", &file) == -ENOENT);
    CHECK(ops->lookup(&volume, &root, "data", &file) == 0 && file.size == size);
    check_xattrs(ops, &volume, &file, xattrs);
    CHECK(ops->lookup(&volume, &root, "hardlink", &other) == 0);
    CHECK(file.first_cluster == other.first_cluster && file.entry_id == other.entry_id);
    struct linux_stat_abi status;
    CHECK(ops->stat(&volume, &file, &status) == 0);
    CHECK(status.st_dev == 0x101 && status.st_ino == file.first_cluster && status.st_nlink == 2);
    CHECK(status.st_mode == (LINUX_S_IFREG | 0640) && status.st_uid == 1234 && status.st_gid == 4321);
    CHECK(status.mtime_sec == 1700000000 && status.st_size == (int64_t)size);
    struct reliefos_permissions permissions;
    CHECK(ops->permissions(&volume, &file, &permissions, false) == 0);
    CHECK(permissions.mode == 0640 && permissions.uid == 1234 && permissions.gid == 4321);
    CHECK(ops->permissions(&volume, &file, &permissions, true) == -EROFS);
    struct linux_statfs_abi fsstatus = {0};
    CHECK(ops->statfs(&volume, &fsstatus) == 0 && fsstatus.f_type == 0x73717368 && fsstatus.f_bsize == 4096);
    unsigned char *buffer = malloc(size);
    uint32_t count;
    CHECK(buffer);
    CHECK(ops->read(&volume, &file, 4093, buffer, 9195, &count, NULL, 0) == 0 && count == 9195);
    CHECK(memcmp(buffer, data + 4093, count) == 0);
    CHECK(ops->read(&volume, &file, 0, buffer, size, &count, NULL, 0) == 0 && count == size);
    CHECK(memcmp(buffer, data, size) == 0);
    CHECK(ops->read(&volume, &file, size + 1, buffer, 1, &count, NULL, 0) == 0 && count == 0);
    CHECK(ops->lookup(&volume, &root, "sparse", &file) == 0);
    CHECK(ops->read(&volume, &file, 4095, buffer, 13000, &count, NULL, 0) == 0 && count == 12293);
    CHECK(memcmp(buffer + count - 4, "tail", 4) == 0);
    for (uint32_t i = 0; i < count - 4; ++i) CHECK(buffer[i] == 0);
    free(buffer);

    for (unsigned index = 0; index < 24; ++index) {
        char name[20], small[720];
        snprintf(name, sizeof(name), "small-%02u", index);
        CHECK(ops->lookup(&volume, &root, name, &file) == 0);
        CHECK(ops->read(&volume, &file, 0, small, sizeof(small), &count, NULL, 0) == 0);
        CHECK(count == 701);
        for (unsigned i = 0; i < count; ++i) CHECK(small[i] == (char)('A' + index));
    }

    struct reliefos_dir_entry entry;
    uint64_t cookie = 0;
    CHECK(ops->lookup(&volume, &root, "empty", &dir) == 0);
    CHECK(ops->readdir(&volume, &dir, &cookie, &entry, 0) == 0);
    CHECK(ops->lookup(&volume, &root, "bigdir", &dir) == 0);
    for (unsigned index = 0; index < 1300; ++index) {
        char expected[20];
        snprintf(expected, sizeof(expected), "file-%04u", index);
        CHECK(ops->readdir(&volume, &dir, &cookie, &entry, 0) == 1);
        CHECK(entry.type == RELIEFOS_FS_TYPE_FILE && strcmp(entry.name, expected) == 0);
    }
    CHECK(ops->readdir(&volume, &dir, &cookie, &entry, 0) == 0);
    CHECK(ops->lookup(&volume, &dir, "file-1299", &file) == 0);
    CHECK(ops->lookup(&volume, &root, "ldir", &dir) == 0 && dir.size > 65535);
    CHECK(ops->lookup(&volume, &root, "names", &dir) == 0);
    char long_name[256], listed_name[256];
    memset(long_name, 'x', 255);
    long_name[255] = 0;
    CHECK(ops->lookup(&volume, &dir, long_name, &file) == 0);
    cookie = 0;
    CHECK(ops->readdir_name(&volume, &dir, &cookie, listed_name, sizeof(listed_name), 0) == 1);
    CHECK(!strcmp(listed_name, "."));
    CHECK(ops->readdir_name(&volume, &dir, &cookie, listed_name, sizeof(listed_name), 0) == 1);
    CHECK(!strcmp(listed_name, ".."));
    CHECK(ops->readdir_name(&volume, &dir, &cookie, listed_name, sizeof(listed_name), 0) == 1);
    CHECK(!strcmp(listed_name, long_name));
    CHECK(ops->readdir_name(&volume, &dir, &cookie, listed_name, sizeof(listed_name), 0) == 0);
    CHECK(ops->lookup(&volume, &root, "link", &file) == 0 && file.type == RELIEFOS_FS_TYPE_SYMLINK);
    char target[32];
    CHECK(ops->readlink(&volume, &file, target, sizeof(target), &count) == 0);
    CHECK(count == sizeof("bigdir/../data") - 1 && memcmp(target, "bigdir/../data", count) == 0);
    CHECK(ops->lookup(&volume, &root, "fifo", &file) == 0 && file.type == RELIEFOS_FS_TYPE_FIFO);
    ++device_generation;
    CHECK(ops->stat(&volume, &file, &status) == -EIO);
    ops->destroy(&volume);
    CHECK(allocations == 0);
    free(image);
}

static void bad(const char *path, int expected)
{
    image = load(path, &image_size);
    struct storage_volume volume = {.volume_id = 3, .mount_flags = MS_RDONLY,
        .ext_sector_count = image_size / 512};
    CHECK(storage_squashfs_ops.mount(&volume) == expected);
    storage_squashfs_ops.destroy(&volume);
    CHECK(allocations == 0);
    free(image);
}

static void empty_root(const char *path)
{
    const struct storage_filesystem_ops *ops = &storage_squashfs_ops;
    image = load(path, &image_size);
    struct storage_volume volume = {.volume_id = 3, .mount_flags = MS_RDONLY,
        .ext_sector_count = image_size / 512};
    struct storage_node root, child;
    struct reliefos_dir_entry entry;
    uint64_t cookie = 0;
    CHECK(ops->mount(&volume) == 0);
    CHECK(ops->root(&volume, &root) == 0);
    CHECK(ops->readdir(&volume, &root, &cookie, &entry, 0) == 0);
    CHECK(ops->lookup(&volume, &root, "missing", &child) == -ENOENT);
    ops->destroy(&volume);
    CHECK(allocations == 0);
    free(image);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 9);
    size_t length;
    unsigned char *data = load(argv[1], &length);
    bad(argv[2], -EINVAL);
    empty_root(argv[3]);
    for (int i = 4; i < argc; ++i) good(argv[i], data, length, i > 4);
    free(data);
    puts("SquashFS: codecs, fragments, xattrs, directories, links and sparse data passed");
    return 0;
}
