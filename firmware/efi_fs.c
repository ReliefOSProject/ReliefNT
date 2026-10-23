#include <reliefnt/efi_fs.h>
#include <reliefnt/console.h>
#include <reliefnt/lock.h>

#define EFI_BY_PROTOCOL 2ULL
#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL

#define EFI_SUCCESS 0ULL
#define EFI_BUFFER_TOO_SMALL 0x8000000000000005ULL
#define EFI_NOT_FOUND 0x8000000000000014ULL

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_system_table {
    struct efi_table_header hdr;
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    void *console_in_handle;
    void *con_in;
    void *console_out_handle;
    void *con_out;
    void *standard_error_handle;
    void *std_err;
    void *runtime_services;
    struct efi_boot_services *boot_services;
};

typedef uint64_t efi_status_t;
typedef void *efi_handle_t;

typedef efi_status_t (__attribute__((ms_abi)) *efi_handle_protocol_fn)(
    efi_handle_t handle,
    struct efi_guid *protocol,
    void **interface);
typedef efi_status_t (__attribute__((ms_abi)) *efi_locate_handle_buffer_fn)(
    uint64_t search_type,
    struct efi_guid *protocol,
    void *search_key,
    uint64_t *no_handles,
    efi_handle_t **buffer);
typedef efi_status_t (__attribute__((ms_abi)) *efi_get_memory_map_fn)(
    uint64_t *memory_map_size,
    void *memory_map,
    uint64_t *map_key,
    uint64_t *descriptor_size,
    uint32_t *descriptor_version);
typedef efi_status_t (__attribute__((ms_abi)) *efi_free_pool_fn)(void *buffer);

struct efi_boot_services {
    struct efi_table_header hdr;
    efi_status_t (*raise_tpl)(uint64_t tpl);
    void (*restore_tpl)(uint64_t tpl);
    void *allocate_pages;
    void *free_pages;
    efi_get_memory_map_fn get_memory_map;
    void *allocate_pool;
    efi_free_pool_fn free_pool;
    char _pad1[48];
    efi_status_t (*install_protocol_interface)(void);
    efi_status_t (*reinstall_protocol_interface)(void);
    efi_status_t (*uninstall_protocol_interface)(void);
    efi_handle_protocol_fn handle_protocol;
    void *_reserved;
    efi_status_t (*register_protocol_notify)(void);
    efi_status_t (*locate_handle)(void);
    efi_status_t (*locate_device_path)(void);
    efi_status_t (*install_configuration_table)(void);
    efi_status_t (*load_image)(void);
    efi_status_t (*start_image)(void);
    efi_status_t (*exit)(void);
    efi_status_t (*unload_image)(void);
    efi_status_t (*exit_boot_services)(void);
    efi_status_t (*get_next_monotonic_count)(void);
    efi_status_t (*stall)(void);
    efi_status_t (*set_watchdog_timer)(void);
    efi_status_t (*connect_controller)(void);
    efi_status_t (*disconnect_controller)(void);
    efi_status_t (*open_protocol)(void);
    efi_status_t (*close_protocol)(void);
    efi_status_t (*open_protocol_information)(void);
    efi_status_t (*protocols_per_handle)(void);
    efi_locate_handle_buffer_fn locate_handle_buffer;
};

struct efi_simple_file_system_protocol;
struct efi_file_protocol;

typedef efi_status_t (__attribute__((ms_abi)) *efi_open_volume_fn)(
    struct efi_simple_file_system_protocol *self,
    struct efi_file_protocol **root);
typedef efi_status_t (__attribute__((ms_abi)) *efi_file_open_fn)(
    struct efi_file_protocol *self,
    struct efi_file_protocol **new_handle,
    uint16_t *file_name,
    uint64_t open_mode,
    uint64_t attributes);
typedef efi_status_t (__attribute__((ms_abi)) *efi_file_close_fn)(
    struct efi_file_protocol *self);
typedef efi_status_t (__attribute__((ms_abi)) *efi_file_read_fn)(
    struct efi_file_protocol *self,
    uint64_t *buffer_size,
    void *buffer);
typedef efi_status_t (__attribute__((ms_abi)) *efi_file_set_position_fn)(
    struct efi_file_protocol *self,
    uint64_t position);
typedef efi_status_t (__attribute__((ms_abi)) *efi_file_get_info_fn)(
    struct efi_file_protocol *self,
    struct efi_guid *information_type,
    uint64_t *buffer_size,
    void *buffer);

struct efi_simple_file_system_protocol {
    uint64_t revision;
    efi_open_volume_fn open_volume;
};

struct efi_file_protocol {
    uint64_t revision;
    efi_file_open_fn open;
    efi_file_close_fn close;
    void *delete_file;
    efi_file_read_fn read;
    void *write;
    void *get_position;
    efi_file_set_position_fn set_position;
    efi_file_get_info_fn get_info;
    void *set_info;
    void *flush;
    void *open_ex;
    void *read_ex;
    void *write_ex;
    void *flush_ex;
};

struct efi_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t pad1;
    uint32_t nanosecond;
    int16_t timezone;
    uint8_t daylight;
    uint8_t pad2;
};

struct efi_file_info {
    uint64_t size;
    uint64_t file_size;
    uint64_t physical_size;
    struct efi_time create_time;
    struct efi_time last_access_time;
    struct efi_time modification_time;
    uint64_t attribute;
    uint16_t file_name[1];
};

static struct efi_boot_services *boot_services;
static struct efi_file_protocol *root_dir;
static bool ready;
static bool sealed;

static struct efi_guid sfs_guid = {
    0x964e5b22,
    0x6459,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b},
};

static struct efi_guid file_info_guid = {
    0x09576e92,
    0x6d3f,
    0x11d2,
    {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b},
};

static int status_to_errno(efi_status_t status)
{
    if (status == EFI_NOT_FOUND) {
        return -2;
    }
    if (status == EFI_BUFFER_TOO_SMALL) {
        return -12;
    }
    return -22;
}

static void copy_ascii(char *dst, uint32_t cap, const uint16_t *src)
{
    uint32_t i = 0;
    if (!dst || cap == 0) {
        return;
    }
    while (src && src[i] && i + 1 < cap) {
        uint16_t ch = src[i];
        dst[i] = (ch >= 32 && ch <= 126) ? (char)ch : '?';
        ++i;
    }
    dst[i] = 0;
}

static int build_efi_path(const char *path, uint16_t *out, uint32_t cap)
{
    uint32_t pos = 0;

    if (!path || !out || cap < 2) {
        return -22;
    }
    if (path[0] != '/') {
        return -22;
    }

    out[pos++] = '\\';
    for (const char *p = path + 1; *p; ++p) {
        if (pos + 1 >= cap) {
            return -22;
        }
        out[pos++] = (*p == '/') ? '\\' : (uint8_t)*p;
    }
    while (pos > 1 && out[pos - 1] == '\\') {
        --pos;
    }
    out[pos] = 0;
    return 0;
}

static void close_file(struct efi_file_protocol *file)
{
    if (file && file != root_dir && file->close) {
        file->close(file);
    }
}

static int open_path(const char *path, struct efi_file_protocol **out_file)
{
    uint16_t efi_path[RELIEFOS_FS_PATH_LEN];
    efi_status_t status;
    int ret;

    if (!ready || !root_dir || !root_dir->open || !out_file) {
        return -2;
    }
    if (path[0] == '/' && path[1] == 0) {
        *out_file = root_dir;
        return 0;
    }
    ret = build_efi_path(path, efi_path, RELIEFOS_FS_PATH_LEN);
    if (ret < 0) {
        return ret;
    }

    status = root_dir->open(root_dir, out_file, efi_path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS || !*out_file) {
        console_error("[reliefnt] efi fs open failed path=%s status=0x%llx\n",
                       path,
                       (unsigned long long)status);
        return status_to_errno(status);
    }
    return 0;
}

/** @brief Locate and open the EFI system volume before page-table replacement.
 * @param system_table_addr Firmware-owned EFI system table address.
 * @return None. Leaves only the root file handle active for bounded early reads.
 */
static void efi_fs_init_impl(uint64_t system_table_addr)
{
    struct efi_system_table *st;
    uint64_t handle_count = 0;
    efi_handle_t *handles = 0;
    efi_status_t status;

    if (ready || sealed) {
        return;
    }
    if (!system_table_addr) {
        console_warn("[reliefnt] efi fs init skipped: no EFI system table\n");
        return;
    }

    st = (struct efi_system_table *)(uintptr_t)system_table_addr;
    if (!st || !st->boot_services || !st->boot_services->locate_handle_buffer ||
        !st->boot_services->handle_protocol || !st->boot_services->free_pool) {
        console_warn("[reliefnt] efi fs init failed: boot services unavailable\n");
        return;
    }

    boot_services = st->boot_services;
    status = boot_services->locate_handle_buffer(EFI_BY_PROTOCOL, &sfs_guid, 0,
                                                 &handle_count, &handles);
    if (status != EFI_SUCCESS || handle_count == 0 || !handles) {
        console_error("[reliefnt] efi fs locate failed status=0x%llx handles=%llu\n",
                       (unsigned long long)status,
                       (unsigned long long)handle_count);
        if (handles && boot_services->free_pool)
            boot_services->free_pool(handles);
        return;
    }

    for (uint64_t i = 0; i < handle_count; ++i) {
        struct efi_simple_file_system_protocol *fs = 0;
        struct efi_file_protocol *volume = 0;

        status = boot_services->handle_protocol(handles[i], &sfs_guid, (void **)&fs);
        if (status != EFI_SUCCESS || !fs || !fs->open_volume) {
            continue;
        }
        status = fs->open_volume(fs, &volume);
        if (status == EFI_SUCCESS && volume) {
            root_dir = volume;
            ready = true;
            break;
        }
    }

    if (handles && boot_services->free_pool)
        boot_services->free_pool(handles);
    if (ready) {
        console_info("[reliefnt] efi fs root volume ready handles=%llu\n",
                     (unsigned long long)handle_count);
    } else {
        console_error("[reliefnt] efi fs init failed: no readable FAT32 volume\n");
    }
}

/** @brief Initialize the EFI filesystem while firmware mappings are still active.
 * @param system_table_addr Firmware-owned EFI system table address.
 * @return None. Preserves the caller's interrupt state across firmware calls.
 */
void efi_fs_init(uint64_t system_table_addr)
{
    /* UEFI boot services execute at TPL with interrupts enabled and return
     * with IF set.  Kernel callers keep interrupts disabled across firmware
     * crossings — early boot must not run handlers before irq_init() — so
     * every entry point below brackets its work with the caller's RFLAGS. */
    uint64_t irq_flags = kernel_irq_save();
    efi_fs_init_impl(system_table_addr);
    kernel_irq_restore(irq_flags);
}

bool efi_fs_ready(void)
{
    return ready;
}

/** @brief Close the root volume and seal the EFI filesystem against later firmware calls.
 * @return None. BSP boot context while firmware boot services are still live.
 */
static void efi_fs_shutdown_impl(void)
{
    struct efi_file_protocol *volume = root_dir;
    if (volume && volume->close) volume->close(volume);
    root_dir = NULL;
    boot_services = NULL;
    ready = false;
    sealed = true;
}

/** @brief Stop all EFI filesystem use before the kernel replaces firmware mappings.
 * @return None. Preserves the caller's interrupt state across firmware calls.
 */
void efi_fs_shutdown(void)
{
    uint64_t irq_flags = kernel_irq_save();
    if (!sealed) efi_fs_shutdown_impl();
    kernel_irq_restore(irq_flags);
}

/** @brief Copy one live EFI memory map into caller-owned early-boot storage.
 * @param system_table_addr Firmware-owned EFI system table address.
 * @param buffer Writable destination of buffer_bytes bytes.
 * @param buffer_bytes Destination capacity.
 * @param out_entry_size Required output for descriptor stride.
 * @param out_entry_count Required output for descriptor count.
 * @return 0 on a complete map capture or a negative errno-style value. The
 *         explicit system-table pointer remains usable after filesystem
 *         shutdown, until the kernel replaces the firmware page tables.
 */
static int efi_boot_memory_map_impl(uint64_t system_table_addr, void *buffer,
                                    uint64_t buffer_bytes, uint64_t *out_entry_size,
                                    uint32_t *out_entry_count)
{
    struct efi_system_table *st;
    uint64_t map_size = buffer_bytes;
    uint64_t map_key = 0;
    uint64_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    efi_status_t status;

    if (!buffer || !buffer_bytes || !out_entry_size || !out_entry_count) {
        return -22;
    }
    st = (struct efi_system_table *)(uintptr_t)system_table_addr;
    if (!st || !st->boot_services || !st->boot_services->get_memory_map) {
        return -19;
    }
    status = st->boot_services->get_memory_map(&map_size, buffer, &map_key,
                                               &descriptor_size,
                                               &descriptor_version);
    if (status != EFI_SUCCESS || !descriptor_size ||
        map_size > buffer_bytes || (map_size % descriptor_size) != 0) {
        console_error("[reliefnt] efi memory map capture failed status=0x%llx bytes=%llu\n",
                       (unsigned long long)status,
                       (unsigned long long)map_size);
        return -1;
    }
    *out_entry_size = descriptor_size;
    *out_entry_count = (uint32_t)(map_size / descriptor_size);
    return 0;
}

int efi_boot_memory_map(uint64_t system_table_addr, void *buffer,
                        uint64_t buffer_bytes, uint64_t *out_entry_size,
                        uint32_t *out_entry_count)
{
    uint64_t irq_flags = kernel_irq_save();
    int ret = efi_boot_memory_map_impl(system_table_addr, buffer, buffer_bytes,
                                       out_entry_size, out_entry_count);
    kernel_irq_restore(irq_flags);
    return ret;
}

/** @brief Read a bounded file into caller storage while EFI file protocols are live.
 * @param path Absolute EFI path beginning with slash.
 * @param buffer Caller-owned writable destination.
 * @param capacity Maximum accepted complete file size.
 * @param out_len Required output for the number of bytes read.
 * @return 0 on a complete read or a negative errno-style value; never allocates.
 */
static int efi_fs_read_file_into_impl(const char *path, void *buffer,
                                      size_t capacity, size_t *out_len)
{
    struct efi_file_protocol *file = 0;
    struct efi_file_info *info;
    uint8_t info_buf[512];
    uint64_t info_size = sizeof(info_buf);
    efi_status_t status;
    uint64_t file_size;
    uint64_t read_size;
    int ret;

    if (!path || !buffer || !capacity || !out_len) {
        return -22;
    }
    *out_len = 0;

    ret = open_path(path, &file);
    if (ret < 0) {
        return ret;
    }

    if (!file->get_info || !file->read) {
        close_file(file);
        return -22;
    }

    status = file->get_info(file, &file_info_guid, &info_size, info_buf);
    if (status != EFI_SUCCESS || info_size < sizeof(struct efi_file_info)) {
        console_error("[reliefnt] efi fs get_info failed path=%s status=0x%llx size=%llu\n",
                       path,
                       (unsigned long long)status,
                       (unsigned long long)info_size);
        close_file(file);
        return status_to_errno(status);
    }

    info = (struct efi_file_info *)info_buf;
    file_size = info->file_size;
    if (file_size == 0 || file_size > capacity) {
        close_file(file);
        return file_size ? -27 : -2;
    }

    read_size = file_size;
    status = file->read(file, &read_size, buffer);
    close_file(file);
    if (status != EFI_SUCCESS || read_size != file_size) {
        console_error("[reliefnt] efi fs read failed path=%s status=0x%llx read=%llu size=%llu\n",
                       path,
                       (unsigned long long)status,
                       (unsigned long long)read_size,
                       (unsigned long long)file_size);
        return status_to_errno(status);
    }

    *out_len = (size_t)file_size;
    return 0;
}

/** @brief Read one bounded file into storage that remains valid across EFI callbacks.
 * @param path Absolute EFI path beginning with slash.
 * @param buffer Caller-owned writable destination of capacity bytes.
 * @param capacity Maximum complete file size accepted; oversized files are not read.
 * @param out_len Required output, set to zero on failure and the complete size on success.
 * @return 0 on a complete read or a negative errno-style value. Boot context
 *         before the firmware mappings are replaced; performs no allocation.
 */
int efi_fs_read_file_into(const char *path, void *buffer, size_t capacity,
                          size_t *out_len)
{
    if (out_len) *out_len = 0;
    uint64_t irq_flags = kernel_irq_save();
    int ret = efi_fs_read_file_into_impl(path, buffer, capacity, out_len);
    kernel_irq_restore(irq_flags);
    return ret;
}

static int efi_fs_list_dir_impl(const char *path, struct reliefos_dir_entry *entries,
                                uint32_t capacity, uint32_t *out_count)
{
    struct efi_file_protocol *dir = 0;
    uint32_t count = 0;
    uint8_t info_buf[4096];
    int ret;

    if (!out_count) {
        return -22;
    }
    *out_count = 0;

    ret = open_path(path, &dir);
    if (ret < 0) {
        return ret;
    }
    if (!dir->read) {
        close_file(dir);
        return -22;
    }
    if (dir->set_position) {
        dir->set_position(dir, 0);
    }

    for (;;) {
        uint64_t info_size = sizeof(info_buf);
        efi_status_t status = dir->read(dir, &info_size, info_buf);
        if (status != EFI_SUCCESS) {
            close_file(dir);
            return status_to_errno(status);
        }
        if (info_size == 0) {
            break;
        }

        struct efi_file_info *info = (struct efi_file_info *)info_buf;
        if (info->file_name[0] == 0) {
            continue;
        }
        if (info->file_name[0] == '.' &&
            (info->file_name[1] == 0 ||
             (info->file_name[1] == '.' && info->file_name[2] == 0))) {
            continue;
        }

        if (count < capacity && entries) {
            entries[count].type = (info->attribute & EFI_FILE_DIRECTORY) ?
                RELIEFOS_FS_TYPE_DIR : RELIEFOS_FS_TYPE_FILE;
            copy_ascii(entries[count].name, sizeof(entries[count].name), info->file_name);
        }
        ++count;
    }

    close_file(dir);
    if (count > capacity) {
        count = capacity;
    }
    *out_count = count;
    return (int)count;
}

int efi_fs_list_dir(const char *path, struct reliefos_dir_entry *entries,
                    uint32_t capacity, uint32_t *out_count)
{
    uint64_t irq_flags = kernel_irq_save();
    int ret = efi_fs_list_dir_impl(path, entries, capacity, out_count);
    kernel_irq_restore(irq_flags);
    return ret;
}
