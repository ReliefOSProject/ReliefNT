#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "../../block/core/storage_internal.h"

struct storage_volume g_volumes[STORAGE_MAX_VOLUMES];
static unsigned char *image;
static size_t image_length;
static uint32_t max_pages;
static int fail_alloc;

/** @brief Allocate a bounded host buffer as a stand-in for kernel page ownership. */
uint64_t mm_alloc_pages(uint32_t count)
{
    if (count > max_pages) max_pages = count;
    return fail_alloc ? 0 : (uintptr_t)calloc(count, 4096);
}
/** @brief Release only pages returned by this fixture's allocator. */
void mm_free_pages(uint64_t address, uint32_t count) { (void)count; free((void *)(uintptr_t)address); }
/** @brief Present one private in-memory FAT directory metadata file. */
int storage_read_file(const char *path, const void **out, size_t *length)
{
    (void)path;
    if (!image) return -2;
    uint64_t address = mm_alloc_pages((image_length + 4095) / 4096);
    if (!address) return -12;
    memcpy((void *)(uintptr_t)address, image, image_length);
    *out = (void *)(uintptr_t)address; *length = image_length;
    return 0;
}
/** @brief Atomically retain production metadata bytes for the next API call. */
int storage_write_file(const char *path, const void *data, uint32_t length)
{
    (void)path;
    unsigned char *next = malloc(length);
    assert(next);
    memcpy(next, data, length); free(image); image = next; image_length = length;
    return 0;
}
/** @brief Route only fixture paths to its synthetic mounted FAT volume. */
int storage_path_volume_id(const char *path, uint32_t *out) { (void)path; *out = 0; return 0; }
/** @brief Provide the storage module's ordinary string/memory helpers. */
void storage_memzero(void *out, size_t count) { memset(out, 0, count); }
/** @brief Copy exactly the requested bytes. */
void storage_memcpy(void *out, const void *in, size_t count) { memcpy(out, in, count); }
/** @brief Return a borrowed string's length. */
size_t storage_strlen(const char *text) { return strlen(text); }
/** @brief Compare FAT names without case. */
int storage_text_eq_ci(const char *left, const char *right) { return !strcasecmp(left, right); }
/** @brief Compare ordinary absolute paths exactly. */
int storage_text_eq(const char *left, const char *right) { return !strcmp(left, right); }
/** @brief Bound a NUL-terminated path copy. */
void storage_copy_text(char *out, uint32_t size, const char *in) { snprintf(out, size, "%s", in); }

/** @brief Preserve more than one GRUB directory's worth of mode/owner records. */
int main(void)
{
    g_volumes[0].ready = true; g_volumes[0].filesystem = STORAGE_FILESYSTEM_FAT32;
    for (unsigned i = 0; i < 600; ++i) {
        char path[128]; snprintf(path, sizeof(path), "/boot/grub/i386-pc/module-%04u.mod", i);
        struct reliefos_permissions value = {0640, 1000 + i, 2000 + i};
        int ret = storage_sidecar_permissions(path, &value, true);
        if (ret) fprintf(stderr, "record %u failed: %d\n", i, ret);
        assert(ret == 0);
    }
    for (unsigned i = 0; i < 600; ++i) {
        char path[128], backup[132];
        snprintf(path, sizeof(path), "/boot/grub/i386-pc/module-%04u.mod", i);
        snprintf(backup, sizeof(backup), "%s~", path);
        assert(storage_sidecar_note_renamed(path, backup) == 0);
        struct reliefos_permissions old, fresh = {0600, 0, 0};
        assert(storage_sidecar_permissions(backup, &old, false) == 0);
        assert(old.uid == 1000 + i && old.gid == 2000 + i && old.mode == 0640);
        assert(storage_sidecar_permissions(path, &fresh, true) == 0);
    }
    assert(image_length > 8192 && max_pages < 64);
    unsigned char *before = malloc(image_length); assert(before); memcpy(before, image, image_length);
    fail_alloc = 1;
    struct reliefos_permissions value = {0777, 99, 99};
    assert(storage_sidecar_permissions("/boot/grub/i386-pc/new.mod", &value, true) == -12);
    assert(!memcmp(before, image, image_length)); fail_alloc = 0; free(before);
    image[12] ^= 1;
    assert(storage_sidecar_permissions("/boot/grub/i386-pc/module-0000.mod", &value, false) == -5);
    free(image);
    puts("PASS FAT metadata: 1200 mode/owner records, GRUB backup renames, bounded allocation and corruption/failure refusal");
    return 0;
}
