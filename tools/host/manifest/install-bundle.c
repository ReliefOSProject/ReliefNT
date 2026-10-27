/* SPDX-License-Identifier: Apache-2.0 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct product { char *name; char hash[65]; off_t size; bool ordered; };
struct setting { char *name, *value; };
struct installation {
    int root;
    char prefix[64], abi[65];
    struct product *products; size_t count;
    struct setting *settings; size_t setting_count;
};

/** @brief Test a nonempty ASCII identifier without locale-dependent classes.
 * @param text Borrowed NUL-terminated identifier.
 * @return True for letters, digits and underscore only.
 */
static bool identifier(const char *text)
{
    if (!text || !*text) return false;
    for (; *text; ++text)
        if (!((*text >= 'a' && *text <= 'z') || (*text >= 'A' && *text <= 'Z') ||
              (*text >= '0' && *text <= '9') || *text == '_')) return false;
    return true;
}

/** @brief Validate one complete lowercase SHA-256 digest.
 * @param text Borrowed NUL-terminated value, nullable.
 * @return True for exactly 64 lowercase hexadecimal digits.
 */
static bool digest_valid(const char *text)
{
    if (!text || strlen(text) != 64) return false;
    for (size_t i = 0; i < 64; ++i)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

/** @brief Validate canonical relative product names and reject traversal.
 * @param name Borrowed name; cannot be empty or absolute.
 * @return True when each component uses the installed product alphabet.
 */
static bool relative_valid(const char *name)
{
    if (!name || !*name || *name == '/') return false;
    const char *start = name;
    for (const char *at = name;; ++at) {
        unsigned char value = (unsigned char)*at;
        if (!value || value == '/') {
            size_t length = (size_t)(at - start);
            if (!length || (length == 1 && *start == '.') || (length == 2 && !memcmp(start, "..", 2))) return false;
            if (!value) return true;
            start = at + 1;
        } else if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
                     (value >= 'A' && value <= 'Z') || strchr("._+-", value))) return false;
    }
}

/** @brief Move a successful descriptor above standard streams for child I/O.
 * @param fd Owned descriptor or negative error.
 * @return Owned descriptor >=3, or -1; closes the original on relocation.
 */
static int safe_fd(int fd)
{
    if (fd < 0 || fd >= 3) return fd;
    int next = fcntl(fd, F_DUPFD_CLOEXEC, 3);
    int saved = errno; close(fd); errno = saved;
    return next;
}

/** @brief Open a held-root product without following any symlink component.
 * @param root Borrowed installation directory descriptor.
 * @param name Canonical relative name.
 * @return Owned regular descriptor, or -1 with errno; never blocks on a FIFO.
 */
static int open_product(int root, const char *name)
{
    if (!relative_valid(name)) { errno = EINVAL; return -1; }
    char *copy = strdup(name);
    if (!copy) return -1;
    int fd = fcntl(root, F_DUPFD_CLOEXEC, 3);
    char *at = copy;
    while (fd >= 0) {
        char *slash = strchr(at, '/'); if (slash) *slash = 0;
        struct stat status;
        if (!slash && (fstatat(fd, at, &status, AT_SYMLINK_NOFOLLOW) || !S_ISREG(status.st_mode))) {
            close(fd); fd = -1; errno = EINVAL; break;
        }
        int next = safe_fd(openat(fd, at, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (slash ? O_DIRECTORY : O_NONBLOCK)));
        int saved = errno; close(fd); fd = next; errno = saved;
        if (!slash) break;
        at = slash + 1;
    }
    free(copy);
    if (fd >= 0) {
        struct stat status;
        if (fstat(fd, &status) || !S_ISREG(status.st_mode)) { close(fd); errno = EINVAL; return -1; }
    }
    return fd;
}

/** @brief Hash an already held product through standard sha256sum stdin.
 * @param fd Borrowed regular descriptor, currently positioned at its start.
 * @param hash Caller-owned 65-byte output.
 * @return Zero on successful child completion and valid digest, -1 on failure.
 */
static int hash_product(int fd, char hash[65])
{
    int channels[2];
    if (pipe(channels)) return -1;
    channels[0] = safe_fd(channels[0]); channels[1] = safe_fd(channels[1]);
    if (channels[0] < 0 || channels[1] < 0) {
        if (channels[0] >= 0) close(channels[0]);
        if (channels[1] >= 0) close(channels[1]);
        return -1;
    }
    pid_t child = fork();
    if (child < 0) { close(channels[0]); close(channels[1]); return -1; }
    if (!child) {
        if (dup2(fd, STDIN_FILENO) < 0 || dup2(channels[1], STDOUT_FILENO) < 0) _exit(126);
        close(channels[0]); close(channels[1]);
        execlp("sha256sum", "sha256sum", (char *)NULL);
        _exit(127);
    }
    close(channels[1]);
    char output[128]; size_t used = 0; int result = 0;
    while (used < sizeof(output)) {
        ssize_t count = read(channels[0], output + used, sizeof(output) - used);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { result = -1; break; }
        if (!count) break;
        used += (size_t)count;
    }
    close(channels[0]);
    int status; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (result || waited != child || !WIFEXITED(status) || WEXITSTATUS(status) || used < 66 || used == sizeof(output)) return -1;
    memcpy(hash, output, 64); hash[64] = 0;
    return digest_valid(hash) ? 0 : -1;
}

/** @brief Locate an existing installed product by its canonical full name.
 * @param install Borrowed installation snapshot.
 * @param name Borrowed full relative name.
 * @return Borrowed product, or NULL when absent.
 */
static struct product *find_product(struct installation *install, const char *name)
{
    for (size_t i = 0; i < install->count; ++i)
        if (!strcmp(install->products[i].name, name)) return &install->products[i];
    return NULL;
}

/** @brief Verify and add one unique regular product to the owned inventory.
 * @param install Mutable snapshot whose product array owns inserted names.
 * @param name Borrowed canonical full relative name.
 * @param expected Nullable expected digest from the kernel ownership manifest.
 * @return Zero on success, -1 on duplicate, corruption, I/O or allocation failure.
 */
static int add_product(struct installation *install, const char *name, const char *expected)
{
    if (find_product(install, name)) return -1;
    int fd = open_product(install->root, name);
    if (fd < 0) return -1;
    struct stat status; struct product product = {0};
    int result = fstat(fd, &status) || hash_product(fd, product.hash);
    close(fd);
    if (result || (expected && strcmp(expected, product.hash))) return -1;
    product.size = status.st_size; product.name = strdup(name);
    if (!product.name || install->count == SIZE_MAX / sizeof(product)) { free(product.name); return -1; }
    struct product *grown = realloc(install->products, (install->count + 1) * sizeof(product));
    if (!grown) { free(product.name); return -1; }
    install->products = grown; grown[install->count++] = product;
    return 0;
}

/** @brief Open one required text metadata stream from the held installation.
 * @param install Borrowed snapshot.
 * @param member Relative name inside this release's module directory.
 * @return Owned stream, or NULL on unsafe input or open failure.
 */
static FILE *metadata_stream(struct installation *install, const char *member)
{
    char name[256];
    int length = snprintf(name, sizeof(name), "%s%s", install->prefix, member);
    if (length < 0 || (size_t)length >= sizeof(name)) return NULL;
    int fd = open_product(install->root, name);
    if (fd < 0) return NULL;
    FILE *stream = fdopen(fd, "r");
    if (!stream) close(fd);
    return stream;
}

/** @brief Consume the generated ownership list, rehashing every installed member.
 * @param install Mutable inventory, initially empty.
 * @return Zero only for a complete, unique two-field checksum list.
 */
static int ownership(struct installation *install)
{
    FILE *stream = metadata_stream(install, ".reliefnt-kernel.manifest");
    if (!stream) return -1;
    char *line = NULL; size_t capacity = 0; ssize_t length; int result = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (memchr(line, 0, (size_t)length)) { result = -1; break; }
        char hash[65], name[1024], extra;
        if (sscanf(line, "%64s %1023s %c", hash, name, &extra) != 2 || !digest_valid(hash) || !relative_valid(name)) {
            result = -1; break;
        }
        size_t full_size = strlen(install->prefix) + strlen(name) + 1;
        char *full = malloc(full_size);
        if (!full) { result = -1; break; }
        snprintf(full, full_size, "%s%s", install->prefix, name);
        result = add_product(install, full, hash); free(full);
        if (result) break;
    }
    if (ferror(stream)) result = -1;
    free(line); fclose(stream);
    return result;
}

/** @brief Parse the actual installed Kconfig values without a source inventory.
 * @param install Mutable snapshot; owns every copied key and value.
 * @return Zero on complete text, -1 on duplicate/invalid keys, NUL, I/O or allocation failure.
 */
static int configuration(struct installation *install)
{
    FILE *stream = metadata_stream(install, "config");
    if (!stream) return -1;
    char *line = NULL; size_t capacity = 0; ssize_t length; int result = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (memchr(line, 0, (size_t)length)) { result = -1; break; }
        if (length && line[length - 1] == '\n') line[--length] = 0;
        if (strncmp(line, "CONFIG_", 7)) continue;
        char *equal = strchr(line + 7, '=');
        if (!equal) continue;
        *equal++ = 0;
        if (!identifier(line + 7)) { result = -1; break; }
        for (size_t i = 0; i < install->setting_count; ++i)
            if (!strcmp(install->settings[i].name, line + 7)) { result = -1; break; }
        if (result) break;
        struct setting setting = {.name = strdup(line + 7), .value = strdup(equal)};
        struct setting *grown = realloc(install->settings, (install->setting_count + 1) * sizeof(setting));
        if (!grown || !setting.name || !setting.value) {
            free(setting.name); free(setting.value);
            if (grown) install->settings = grown;
            result = -1; break;
        }
        install->settings = grown; grown[install->setting_count++] = setting;
    }
    if (ferror(stream)) result = -1;
    free(line); fclose(stream);
    return result;
}

/** @brief Check the exact .ko set against modules.order and loader configuration.
 * @param install Mutable snapshot; ordered marks are temporary validation state.
 * @return Zero for an exact set and valid module enablement, otherwise -1.
 */
static int module_order(struct installation *install)
{
    FILE *stream = metadata_stream(install, "modules.order");
    if (!stream) return -1;
    char *line = NULL; size_t capacity = 0; ssize_t length; int result = 0; size_t ordered = 0, modules = 0;
    for (size_t i = 0; i < install->count; ++i) {
        size_t size = strlen(install->products[i].name);
        if (size >= 3 && !strcmp(install->products[i].name + size - 3, ".ko")) ++modules;
    }
    while ((length = getline(&line, &capacity, stream)) >= 0) {
        if (memchr(line, 0, (size_t)length)) { result = -1; break; }
        if (length && line[length - 1] == '\n') line[--length] = 0;
        if (!relative_valid(line) || length < 3 || strcmp(line + length - 3, ".ko")) { result = -1; break; }
        size_t full_size = strlen(install->prefix) + (size_t)length + 1;
        char *full = malloc(full_size);
        if (!full) { result = -1; break; }
        snprintf(full, full_size, "%s%s", install->prefix, line);
        struct product *product = find_product(install, full); free(full);
        if (!product || product->ordered) { result = -1; break; }
        product->ordered = true; ++ordered;
    }
    if (ferror(stream) || ordered != modules) result = -1;
    bool enabled = false;
    for (size_t i = 0; i < install->setting_count; ++i)
        if (!strcmp(install->settings[i].name, "MODULES") && !strcmp(install->settings[i].value, "y")) enabled = true;
    if (modules && !enabled) result = -1;
    free(line); fclose(stream);
    return result;
}

/** @brief Quote Kconfig and metadata strings as JSON without changing their bytes.
 * @param stream Borrowed output stream.
 * @param text Borrowed NUL-terminated text; controls are escaped.
 * @return Zero on success, -1 on output error.
 */
static int quote(FILE *stream, const char *text)
{
    if (fputc('"', stream) == EOF) return -1;
    for (const unsigned char *at = (const unsigned char *)text; *at; ++at) {
        if (*at == '"' || *at == '\\') { if (fputc('\\', stream) == EOF || fputc(*at, stream) == EOF) return -1; }
        else if (*at < 32) { if (fprintf(stream, "\\u%04x", *at) < 0) return -1; }
        else if (fputc(*at, stream) == EOF) return -1;
    }
    return fputc('"', stream) == EOF ? -1 : 0;
}

/** @brief Sort installed product names for deterministic publication.
 * @param a Borrowed product record.
 * @param b Borrowed product record.
 * @return Lexical ordering of their canonical names.
 */
static int compare_products(const void *a, const void *b)
{
    const struct product *first = a, *second = b;
    return strcmp(first->name, second->name);
}

/** @brief Sort Kconfig keys independently of configuration file layout.
 * @param a Borrowed setting.
 * @param b Borrowed setting.
 * @return Lexical ordering of key names.
 */
static int compare_settings(const void *a, const void *b)
{
    const struct setting *first = a, *second = b;
    return strcmp(first->name, second->name);
}

/** @brief Produce one complete JSON snapshot in caller-owned memory.
 * @param install Validated snapshot; inventories are sorted in place.
 * @param release Borrowed validated ABI-bound release.
 * @param arch Borrowed architecture metadata.
 * @param profile Borrowed build profile metadata.
 * @param data Output allocation, released by caller even after failure.
 * @param size Output encoded byte count.
 * @return Zero on success, -1 on allocation or output failure.
 */
static int encode(struct installation *install, const char *release, const char *arch,
                  const char *profile, char **data, size_t *size)
{
    if (install->count) qsort(install->products, install->count, sizeof(*install->products), compare_products);
    if (install->setting_count) qsort(install->settings, install->setting_count, sizeof(*install->settings), compare_settings);
    FILE *stream = open_memstream(data, size);
    if (!stream) return -1;
    fprintf(stream, "{\"schema\":1,\"release\":\"%s\",\"module_abi\":\"%s\",\"arch\":", release, install->abi);
    int result = quote(stream, arch); fputs(",\"profile\":", stream); result |= quote(stream, profile);
    fprintf(stream, ",\"module_directory\":\"lib/modules/%s\",\"configuration\":{", release);
    for (size_t i = 0; i < install->setting_count; ++i) {
        if (i) fputc(',', stream);
        result |= quote(stream, install->settings[i].name); fputc(':', stream); result |= quote(stream, install->settings[i].value);
    }
    fputs("},\"modules\":[", stream); bool first = true;
    for (size_t i = 0; i < install->count; ++i) if (install->products[i].ordered) {
        if (!first) fputc(',', stream);
        first = false; result |= quote(stream, install->products[i].name);
    }
    fputs("],\"files\":{", stream);
    for (size_t i = 0; i < install->count; ++i) {
        if (i) fputc(',', stream);
        result |= quote(stream, install->products[i].name);
        fprintf(stream, ":{\"sha256\":\"%s\",\"size\":%lld}", install->products[i].hash, (long long)install->products[i].size);
    }
    fputs("}}\n", stream);
    if (ferror(stream)) result = -1;
    if (fclose(stream)) result = -1;
    return result;
}

/** @brief Publish a changed manifest through a same-directory synced temporary.
 * @param root Borrowed held installation directory.
 * @param data Borrowed complete document.
 * @param size Exact encoded byte count.
 * @return Zero on unchanged or durable publication, -1 without modifying a prior manifest on prepublication failure.
 */
static int publish(int root, const char *data, size_t size)
{
    struct stat status;
    int exists = fstatat(root, "install.bundle.json", &status, AT_SYMLINK_NOFOLLOW);
    if ((!exists && !S_ISREG(status.st_mode)) || (exists && errno != ENOENT)) return -1;
    if (!exists && status.st_size >= 0 && (uint64_t)status.st_size == size) {
        int old = open_product(root, "install.bundle.json");
        if (old < 0) return -1;
        size_t at = 0; bool equal = true;
        while (at < size) {
            char buffer[4096]; size_t want = size - at < sizeof(buffer) ? size - at : sizeof(buffer);
            ssize_t count = read(old, buffer, want);
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0 || memcmp(buffer, data + at, (size_t)count)) { equal = false; break; }
            at += (size_t)count;
        }
        close(old);
        if (equal) return 0;
    }
    char temporary[80]; int fd = -1;
    for (unsigned i = 0; i < 128; ++i) {
        snprintf(temporary, sizeof(temporary), ".install.bundle.%ld.%u", (long)getpid(), i);
        fd = openat(root, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return -1;
    size_t at = 0; int result = 0;
    while (at < size) {
        ssize_t count = write(fd, data + at, size - at);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { result = -1; break; }
        at += (size_t)count;
    }
    if (!result && fsync(fd)) result = -1;
    if (close(fd)) result = -1;
    if (!result) result = renameat(root, temporary, root, "install.bundle.json");
    if (result) (void)unlinkat(root, temporary, 0);
    else result = fsync(root);
    return result;
}

/** @brief Generate only an installed-product manifest; never compile or inspect source inventory.
 * @param argc Argument count; requires DEST RELEASE ARCH PROFILE.
 * @param argv Borrowed process argument vector.
 * @return Zero on a verified complete snapshot, nonzero on any invalid or changed product.
 */
int main(int argc, char **argv)
{
    if (argc != 5) { fputs("usage: install-bundle DEST RELEASE ARCH PROFILE\n", stderr); return 2; }
    struct installation install = {.root = -1};
    int result = -1; char *encoded = NULL; size_t encoded_size = 0;
    size_t release_length = strlen(argv[2]);
    if (release_length < 12 || release_length > 31 || !relative_valid(argv[2]) || strchr(argv[2], '/') ||
        !((argv[2][0] >= '0' && argv[2][0] <= '9') || (argv[2][0] >= 'A' && argv[2][0] <= 'Z') ||
          (argv[2][0] >= 'a' && argv[2][0] <= 'z'))) goto out;
    install.root = safe_fd(open(argv[1], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    struct stat root_status, filesystem_root;
    if (install.root < 0 || fstat(install.root, &root_status) || stat("/", &filesystem_root) ||
        (root_status.st_ino == filesystem_root.st_ino && root_status.st_dev == filesystem_root.st_dev)) goto out;
    snprintf(install.prefix, sizeof(install.prefix), "lib/modules/%s/", argv[2]);
    if (ownership(&install)) goto out;
    static const char *const kernels[] = {"kernel.sys", "kernel.debug", "kerneldebug.sys"};
    for (size_t i = 0; i < sizeof(kernels) / sizeof(kernels[0]); ++i) if (add_product(&install, kernels[i], NULL)) goto out;
    static const char *const required[] = {"config", "module.abi", "Module.symvers", "System.map", "modules.order", "modules.builtin", "modules.builtin.modinfo"};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        char member[128]; snprintf(member, sizeof(member), "%s%s", install.prefix, required[i]);
        if (!find_product(&install, member)) goto out;
    }
    FILE *abi_stream = metadata_stream(&install, "module.abi");
    if (!abi_stream) goto out;
    char bytes[128]; size_t count = fread(bytes, 1, sizeof(bytes) - 1, abi_stream);
    int read_failed = ferror(abi_stream); fclose(abi_stream);
    if (read_failed || memchr(bytes, 0, count) || count == sizeof(bytes) - 1) goto out;
    bytes[count] = 0;
    char *first = bytes; while (*first && isspace((unsigned char)*first)) ++first;
    size_t length = strlen(first); while (length && isspace((unsigned char)first[length - 1])) first[--length] = 0;
    if (!digest_valid(first) || strncmp(argv[2] + release_length - 12, first, 12)) goto out;
    memcpy(install.abi, first, sizeof(install.abi));
    if (configuration(&install) || module_order(&install) ||
        encode(&install, argv[2], argv[3], argv[4], &encoded, &encoded_size) || publish(install.root, encoded, encoded_size)) goto out;
    result = 0;
out:
    if (result) fputs("install-bundle: invalid, incomplete, unsafe or changed installed kernel products\n", stderr);
    if (install.root >= 0) close(install.root);
    for (size_t i = 0; i < install.count; ++i) free(install.products[i].name);
    for (size_t i = 0; i < install.setting_count; ++i) { free(install.settings[i].name); free(install.settings[i].value); }
    free(install.settings); free(install.products); free(encoded);
    return result ? 1 : 0;
}
